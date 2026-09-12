# OS_by_AI

x86_64向けのOSとブートローダーを、基本C言語で段階的に開発します。
操作環境はシリアルコンソールです。

現在は**ローダーとカーネルのビルドを分離した段階**です。
カーネルファイルを読む処理はまだありません。ローダーはUEFIでログを表示して待機し、
Boot Servicesの終了やカーネルへの制御移譲は行いません。

## ビルド

必要なツールはLLVM版Clang / LLD、GNU Make、Python 3.9以上です。
QEMU実行には `qemu-system-x86_64` とOVMF / EDK IIファームウェアを使います。
Pythonの追加パッケージは不要です。

```sh
make build   # ローダーとカーネルを別々にビルド
make loader  # ローダーのみ
make kernel  # カーネルのみ
make test    # 生成物の形式・エントリーポイント・ロードセグメントを確認
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
`kernel/linker.ld`で入口を `kernel_main`、リンクアドレスを `0x100000` に指定しています。
将来のローダーは、このロード領域を確保してからセグメントを配置する必要があります。
カーネル本体は引き続きCLI / HLTループだけです。

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
ローダーはUEFI Serial IOプロトコルで次のログを表示し、UEFI内で待機します。

```text
BOOT: UEFI x86_64 C entry
BOOT: EFI_SERIAL_IO_PROTOCOL via LocateProtocol
BOOT: serial ready (115200 8N1)
BOOT: kernel.elf loading not implemented yet
```

終了は **Ctrl-aを押して離し、x**。Ctrl-aを押して離し、cでQEMUモニターに切り替わります。
この段階ではカーネルを実行しないため、以前のQMPによるHLTテストは外しています。
`make test`は生成物の検証のみで、カーネル起動成功を意味しません。

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
GDBは今回インストールしていません。現在の実行対象はローダーまでです。

## 主なファイル

```text
boot/main.c             UEFIローダーの入口・ログ出力
boot/efi.h              UEFI ABIの宣言
kernel/main.c           最小カーネル
kernel/main.h           カーネル入口の宣言
kernel/linker.ld        ELF64の配置と入口
scripts/check_build.py  生成物の検証
scripts/qemu.py         QEMU起動・ファームウェア検出
Makefile               独立したコンパイル・リンク規則
```

次の段階でELFカーネルの読み込みを実装し、その後Boot Services終了と制御移譲を接続します。
