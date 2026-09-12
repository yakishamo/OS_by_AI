# OS_by_AI

x86_64向けのOSとブートローダーを、基本C言語で段階的に開発します。
操作環境はシリアルコンソールです。

現在は**独立したELFカーネルをUEFIローダーが読み込んで起動する段階**です。
ローダーは起動元ボリュームの `kernel.elf` を読み込み、メモリ配置を済ませて
Boot Servicesを終了します。その後、起動情報をカーネルに渡し、専用スタックへ切り替えてから内容を確認し、HLTで停止します。

## ビルド

必要なツールはLLVM版Clang / LLD、GNU Make、Python 3.9以上です。
QEMU実行には `qemu-system-x86_64` とOVMF / EDK IIファームウェアを使います。
Pythonの追加パッケージは不要です。

```sh
make build   # ローダーとカーネルを別々にビルド
make loader  # ローダーのみ
make kernel  # カーネルのみ
make test    # 生成物検証とQEMUによる起動・異常系テスト
make clean   # build/ の生成物を削除
make doctor  # ツールとファームウェアの配置確認
```

生成物とコンパイル対象:

| 生成物 | 形式 | ターゲット / ABI |
|---|---|---|
| `build/esp/EFI/BOOT/BOOTX64.EFI` | PE32+ UEFIアプリ | `x86_64-pc-windows-msvc` / UEFI x64 |
| `build/esp/kernel.elf` | ELF64静的実行ファイル | `x86_64-unknown-none-elf` / System V AMD64 |

双方ともfreestanding C17で、ホストのlibcにリンクしません。
ローダーにカーネルのオブジェクトはリンクせず、カーネルはUEFIのヘッダーを使いません。
`kernel/linker.ld`で入口を `kernel_entry`、リンクアドレスを `0x100000` に指定しています。
ローダーはELFの指定するロード領域を確保してからセグメントを配置します。
カーネルは起動情報とスタック位置を確認してHLTループへ進みます。

macOS arm64 / LLVM 21.1.8 / QEMU 10.2.0 / Python 3.14.6 / GNU Make 3.81で開発しています。
HomebrewのLLVMを使う場合のPATH設定例:

```sh
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

## QEMUでの現在の動作

```sh
make run
```

QEMUはq35 / x86_64 / TCG / 1 CPU / 256 MiBで起動します。
`BOOTX64.EFI`と`kernel.elf`を別ファイルとして読み取り専用の仮想FATディスクへ配置します。
ローダーはUEFI Serial IOプロトコルで次のログを表示し、カーネルを起動します。

```text
BOOT: UEFI x86_64 C entry
BOOT: EFI_SERIAL_IO_PROTOCOL via LocateProtocol
BOOT: serial ready (115200 8N1)
BOOT: loading kernel.elf
BOOT: kernel entry=0x0000000000100000
```

終了は **Ctrl-aを押して離し、x**。Ctrl-aを押して離し、cでQEMUモニターに切り替わります。
ログの後、カーネルがHLTで停止するため、入力に反応しないのが正常です。
ログはすべてBoot Services終了前のローダーによる出力です。

## カーネル読み込みの手順と範囲

1. Loaded Imageプロトコルから、ローダー自身を読み込んだデバイスを取得します。
2. そのデバイスのSimple File Systemプロトコルを使い、ルートの `\kernel.elf` を読み取り専用で開きます。
3. ELFヘッダーとプログラムヘッダーを検証します。x86_64 / little-endian / ELF64 / ET_EXECを対象にします。
4. PT_LOAD全体を収めるページ領域を `AllocatePages(AllocateAddress, EfiLoaderCode, ...)` で確保します。セグメント間の隙間も含めてゼロ初期化し、各セグメントのファイル部分をコピーします。
5. ファイルを閉じ、一時バッファを解放します。最終メモリマップを取得して `ExitBootServices()` を呼びます。キーが無効になった場合は最大8回、再取得して試します。
6. 終了成功後に起動情報へ終了済みフラグを設定し、System V AMD64の呼び出し規約でELFの入口へ移ります。RDIに起動情報のポインター、RSIに専用スタックの上端を渡します。

現段階の制約:

- ファイルは最大16 MiB、プログラムヘッダーは最大128個、配置領域全体は最大64 MiBです。
- ロード先は1 MiB以上・4 GiB未満で、仮想アドレスと物理アドレスが一致する固定配置を扱います。
- 動的リンクや再配置は未対応です。セグメントの範囲外参照、サイズ矛盾、重複、無効な入口等は拒否します。
- 初期メモリマップのバッファは64 KiBです。不足時は明示的に失敗します。
- カーネルは専用スタックを使います。ページテーブル・GDT・IDT等はまだ既存のものを引き継ぎます。スタックのガードページや独自のページ保護は未実装です。
- ExitBootServicesを一度試みた後はUEFI出力や呼び出し元への復帰をしません。終了失敗やカーネルからの想定外の復帰では、`boot_exit_failure` にステータスを保存し、ローダー内のPAUSEループで停止します。

## 起動情報とスタック

`include/boot_info.h` がUEFIに依存しない共通ABIです。識別子、バージョン、構造体サイズに加え、
次の情報を渡します。アドレスは起動時に恒等マッピングされている物理アドレスです。

- 最終メモリマップのアドレス・有効バイト数・descriptor size・descriptor version。
- 確保したカーネル領域の先頭とサイズ。
- 専用スタックの先頭とサイズ。
- Boot Services終了済みフラグ。

ローダーは最終マップ取得前に、33ページの連続したEfiLoaderData領域を確保します。
配置は、起動情報用4 KiB、メモリマップ用64 KiB、スタック用64 KiBの順です。
この領域はカーネル起動後も解放せず、今後のメモリ管理でも使用中領域として保護します。
マップキーを取り直す場合は、渡すマップのサイズと記述子情報も更新します。

`kernel/entry.S` はCLI・CLDを実行し、旧スタックに触れずにRSPを専用スタック上端へ変更します。
16バイト境界へ揃えてから `kernel_main(const BOOT_INFO *)` を呼び、C関数の入口では
リターンアドレス分の8バイトを考慮したSystem V AMD64の整列条件を満たします。
ローダーへは戻りません。

カーネルは構造体の識別子・版・サイズ、終了済みフラグ、マップ形式、スタック範囲等を確認します。
成功後、`kernel_boot_info` にポインターを保持し、`kernel_halt` で停止します。
検証失敗時はPAUSEループで待機するため、正常なHLTとは区別できます。
メモリマップの走査では、構造体のsizeofではなく、渡された `descriptor_size` を刻み幅に使います。

## 自動テスト

`make test` は生成物の検証に加え、次の7ケースをQEMUで逐次実行します。

- 本物の `kernel.elf` を起動し、`kernel_halt`でのRIP、`HLT=1`、`IF=0`、RIP直前のHLT命令を確認。
- カーネルファイルの欠落、ELF識別子の破損、ファイル範囲外のセグメント、無効な入口を拒否。
- PT_LOADを追加したテスト用ELFで、データのコピーとBSS先頭・末尾のゼロ初期化を確認。
- 重複するロードセグメントを拒否。

テスト用の変更は一時ディスクだけに適用し、ビルド済みカーネルは変更しません。
正常起動ケースでは、受け取った起動情報、専用スタックの上端・C処理開始時・停止時のRSP、
最終メモリマップ上のカーネルと起動情報領域のメモリ種別・範囲も確認します。
ログは `build/tests/<ケース名>/` 以下の `serial.log`、`cpu.log`、`qemu.log`、`boot-info.json` に保存します。
QMPはパイプ経由で接続し、ネットワークポートは使いません。
各ケースの制限時間は60秒です。全体を同時実行せず、逐次実行してください。

```sh
python3 scripts/qemu.py test --timeout 120
```

HLT確認の停止位置はELFの `kernel_halt` シンボルから取得します。
カーネルを拡張するときは、テストの到達点も更新します。

## ツールとファームウェア

Homebrew同梱の `edk2-x86_64-code.fd` / `edk2-i386-vars.fd` と、一般的なLinuxのOVMF配置先を検索します。
Linux上での実行は未検証です。ファームウェアにはSerial IOプロトコルが必要です。
自動検出できない場合は、対応するコードと変数テンプレートを指定します。

```sh
export OVMF_CODE=/path/to/OVMF_CODE.fd
export OVMF_VARS=/path/to/OVMF_VARS.fd
make doctor
```

```sh
make build CLANG=/path/to/clang LLD=/path/to/lld
QEMU=/path/to/qemu-system-x86_64 make run
make test PYTHON=/path/to/python3
```

ツールやコンパイル設定を変えた場合はクリーンビルドしてください。
起動ごとにファームウェア変数領域を複製し、インストール済みのテンプレートは変更しません。

## デバッグ

`make build`後、リポジトリ直下のx86_64対応GDBから接続できます。

```text
target remote | make -s debug
continue
```

ファームウェアの入口で接続を待ちます。シリアル出力は `build/serial-debug.log` に保存します。
GDBは今回インストールしていません。カーネルは固定アドレスなので、GDBで
`symbol-file build/esp/kernel.elf` を指定してカーネルのシンボルを読み込めます。

## 主なファイル

```text
boot/main.c             ログ・Boot Services終了・カーネルへの引き渡し
boot/load.c             起動元ファイルの読み込み・ELF検証・メモリ配置
boot/load.h             ロード結果の型とインターフェース
boot/efi.h              UEFI ABIの宣言
include/boot_info.h      ローダーとカーネルの共通起動情報ABI
kernel/entry.S          専用スタックへの切り替えとC入口への移行
kernel/main.c           起動情報の確認と最小カーネル
kernel/main.h           カーネル入口の宣言
kernel/linker.ld        ELF64の配置と入口
scripts/check_build.py  生成物の検証
scripts/test_boot.py    QEMUでのカーネル起動と異常系の検証
scripts/qemu.py         QEMU起動・ファームウェア検出
Makefile               独立したコンパイル・リンク規則
```

次の段階では、カーネル自身のログ出力と例外処理を小さく追加していきます。

## 参照資料

- [UEFI Loaded Image](https://uefi.org/specs/UEFI/2.10/09_Protocols_EFI_Loaded_Image.html): 起動元デバイスの取得。
- [UEFI Media Access](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html): ファイル操作。
- [UEFI Boot Services](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html): メモリ確保・Boot Services終了。
