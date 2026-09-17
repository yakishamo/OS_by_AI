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
カーネルは起動情報とスタック位置を確認し、自身のシリアルドライバーでログを出してHLTループへ進みます。

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
KERNEL: serial ready (COM1, 115200 8N1)
KERNEL: boot information verified
KERNEL: GDT/IDT/TSS ready
KERNEL: physical pages ready (4 KiB, self-test passed)
KERNEL: paging ready (own CR3, RAM check passed)
KERNEL: dynamic paging checks passed
KERNEL: halting
```

終了は **Ctrl-aを押して離し、x**。Ctrl-aを押して離し、cでQEMUモニターに切り替わります。
ログの後、カーネルがHLTで停止するため、入力に反応しないのが正常です。
`BOOT:` はBoot Services終了前のローダーによる出力、`KERNEL:` は終了後のカーネルによる出力です。

## カーネルのシリアル出力

`kernel/serial.c` はQEMU PCのCOM1（I/Oポート `0x3f8`）を直接操作します。
UEFIのプロトコルや割り込みには依存しません。設定は115200 baud / 8N1、FIFO有効、UART割り込み無効です。

- `serial_init()`：ローダーの残りの送信を待ち、UARTを初期化。
- `serial_write()`：送信可能になるまでポーリングし、文字列を出力。LFはCRLFへ変換。
- `serial_flush()`：FIFOとシフトレジスターの両方が空になるまで待機。

各関数は成功・失敗をboolで返します。ポーリングには回数上限を設けていますが、
カーネルタイマーが未実装なので実時間のタイムアウトではありません。
出力失敗時はPAUSEループに入り、正常なHLT到達として扱いません。
現在は単一CPUの起動ログ用で、入力・送信割り込み・複数CPU間の排他制御は未実装です。

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
- カーネルは専用スタックを使います。GDT・IDT・TSSとページテーブルをカーネル専用のものに切り替えます。通常スタックとダブルフォルト用スタックにガードページを設け、コード・データ別のページ保護を適用します。
- ExitBootServicesを一度試みた後はUEFI出力や呼び出し元への復帰をしません。終了失敗やカーネルからの想定外の復帰では、`boot_exit_failure` にステータスを保存し、ローダー内のPAUSEループで停止します。

## 起動情報とスタック

`include/boot_info.h` がUEFIに依存しない共通ABIです。識別子、バージョン、構造体サイズに加え、
次の情報を渡します。アドレスは起動時に恒等マッピングされている物理アドレスです。

- 最終メモリマップのアドレス・有効バイト数・descriptor size・descriptor version。
- 確保したカーネル領域の先頭とサイズ。
- 専用スタックの先頭とサイズ。
- Boot Services終了済みフラグ。

ローダーは最終マップ取得前に、35ページの連続したEfiLoaderData領域を確保します。
配置は、起動情報用4 KiB、メモリマップ用64 KiB、下側ガード4 KiB、スタック用64 KiB、上側ガード4 KiBの順です。
起動情報ABIはバージョン2です。`stack_base` と `stack_size` はガードを含まない使用可能範囲を表します。
ガード用ページを持たない旧ローダーとの組み合わせは拒否します。
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

## GDT・IDTと例外処理

`kernel/tables.c` と `kernel/interrupts.S` で、次のカーネル専用テーブルを構築・ロードします。

- GDT：null、ring 0の64-bitコード（0x08）、データ（0x10）、64-bit TSS（0x18、2スロット）。
- TSS：RSP0にカーネルスタック上端を設定し、IST1に16 KiBのダブルフォルト専用スタックを設定。
- IDT：256個すべてをDPL0の64-bit interrupt gateとして登録。ベクター8（#DF）のみIST1を使用。

GDTはLGDTの後にfar returnでCSを再ロードし、DS・ES・SSも更新します。LTRでTSSを、LIDTでIDTをロードします。
割り込み許可フラグは無効のままです。PIC/APIC、タイマー、外部IRQの処理はまだ実装していません。

例外入口は、CPUがエラーコードを積まないベクターには0を補い、番号とエラーコードを共通形式にします。
Cのハンドラーは番号、エラーコード、RIP、CS、RFLAGS、例外前のRSPをシリアルへ出力し、
ページフォルトではCR2も出力して停止します。
現在はすべて致命的な例外として扱い、IRETによる復帰や汎用レジスターの保存・復元は行いません。

ダブルフォルト用スタックはカーネルBSSにあり、通常のスタックが壊れた場合にも例外を報告できるようにしています。
NMI専用スタックと再入可能なログ出力は今後の段階です。

例外テストはビルド済みELFのコピーの停止箇所をテスト用命令で置き換えます。
通常のカーネルには例外を発生させるコードを追加しません。
ダブルフォルトテストはRSPを壊して例外配信自体を失敗させ、専用ISTへの切り替えを実際に確認します。

## 物理ページ管理

`kernel/pmm.c` は最終UEFIメモリマップを読み、4 KiB単位で物理ページを管理します。
初期段階の管理範囲は1 MiB以上・4 GiB未満です。`EfiConventionalMemory` のうち
runtime属性のない領域だけを使い、カーネル、起動情報、メモリマップ、専用スタックを除外します。
Boot Services用メモリの回収はまだ行いません。UEFIが使用していたページテーブルも回収対象外です。

- `pmm_init(info)`：初期化。descriptor sizeを使って走査し、重複・非整列・範囲の桁あふれを拒否します。成功後の再初期化はできません。
- `pmm_alloc(&address)`：1ページ確保し、物理アドレスを返します。失敗時は出力を変更しません。
- `pmm_free(address)`：確保済みページを解放します。二重解放、管理対象外、非整列アドレスを拒否します。
- `pmm_total()` / `pmm_available()`：管理対象・空きページの数を返します。
- `pmm_is_allocated(address)`：整列した物理ページが管理対象かつ確保済みか確認します。

管理対象と確保状態を別々のビットマップに記録します。合計256 KiBの静的BSSを使用し、
管理情報自体もカーネル領域として保護します。単一CPU・割り込み無効での使用を前提にしています。
確保時のゼロ初期化、連続した複数ページの確保、ヒープ、ページテーブルの構築は含みません。
起動時の自己テストは確保した3ページ全体への書き込み・読み戻し、解放・再利用を確認し、
成功時は自己テストで確保したページをすべて解放します。このテストをページテーブル切り替え前後に実行します。

## カーネル専用ページテーブル

`kernel/paging.c` の `paging_init(info)` を物理ページ管理の初期化後に呼び出します。
4段のページテーブルを4 KiB単位で構築し、CR3を切り替えます。
管理範囲は1 MiB以上・4 GiB未満で、仮想アドレスと物理アドレスを一致させます。

- 最終メモリマップの `EfiLoaderCode`、`EfiLoaderData`、`EfiConventionalMemory` が対象です。
  runtime属性を持つ領域は除外し、対象RAMにはwrite-back対応を要求します。
- MMIO、予約領域、ACPI領域、Boot Services領域、ページ0はマッピングしません。
  シリアル出力はポートI/Oを使うため、MMIOのマッピングは不要です。
- ページテーブル用ページは `pmm_alloc()` で確保し、ゼロ初期化します。
  有効なページテーブルは確保状態のまま保持し、構築失敗時は確保したページを解放します。
- 切り替え前にカーネル全体、スタック、起動情報、メモリマップ、全ページテーブルのマッピングを確認します。
  CR4.PGEを一時的に無効にして、UEFIから残ったglobal TLBエントリも無効化します。

初期化は単一CPU・割り込み無効で一度だけ行います。構築中はUEFIの恒等マッピングを使用します。
5段ページングとPCIDが有効な場合、またはPATの先頭エントリがwrite-backでない場合は切り替えを拒否します。
恒等マッピングはsupervisor用で、リンカが定義したページ境界を使い、次の権限を適用します。

- `.text`：読み取り・実行可能、書き込み禁止。
- `.rodata`：読み取り専用、実行禁止。
- `.data`・`.bss`・両スタック・ページテーブル・その他の通常RAM：読み書き可能、実行禁止。

通常スタックの両端と、16 KiBのダブルフォルト用ISTスタックの両端に、それぞれ4 KiBの未マップページを設けます。
ISTスタックとガードはリンカの `.stacks` 領域に独立して配置し、他のBSSとページを共有しません。
ガードの物理ページはローダー用領域またはカーネル領域に保持されるため、物理ページ管理で再利用されません。
ガードは自前のCR3への切り替えから有効になります。通常スタックの下端を越え、例外フレームも積めなくなった場合は
ダブルフォルト用ISTへ切り替えて診断・停止します。ガードを飛び越える大きなスタック移動の検出や、IST自体の枯渇からの復旧は未対応です。

NX対応CPUを要求し、EFER.NXEとCR0.WPを有効にして動的マッピングの権限をカーネルモードでも適用します。

## 動的なmap/unmap

動的操作は `0xffff800000000000` から512 GiBの専用仮想アドレス領域に限定します。
アドレスは4 KiB整列が必要です。既存の恒等マッピングやカーネル・スタックは変更できません。

- `paging_map(virtual, physical, flags)`：呼び出し側が確保した物理ページをマップします。既存マッピングの上書き、未確保ページ、ページテーブル自身のマッピングは拒否します。
- `paging_unmap(virtual)`：マッピングを解除し、空になった中間ページテーブルを回収します。データの物理ページは解放しません。
- `paging_protect(virtual, flags)`：既存マッピングの書き込み・実行権限を変更します。
- `paging_query(virtual, &physical, &flags)`：対応する物理ページと権限を取得します。失敗時は出力を変更しません。

`flags` は `PAGING_WRITE` と `PAGING_EXEC` の組み合わせです。0なら読み取り専用・実行禁止です。
すべてsupervisor用で、ユーザーモード用ページはまだ扱いません。
更新時は `invlpg` でTLBを無効化します。単一CPU・割り込み無効が前提で、他CPUへのTLB無効化要求は未実装です。
物理ページの所有権は呼び出し側に残ります。同じ物理ページへの動的マッピングをすべて解除してから `pmm_free()` してください。
参照カウントや自動解放は行いません。権限は仮想アドレスごとに適用され、恒等マッピング経由のアクセス権限は変わりません。

ページテーブルは初期マッピングと合わせて最大4096ページです。容量や物理メモリが不足したmapは、
その呼び出しで作成した中間テーブルを解放し、既存マッピングを維持して失敗します。
カーネル本体の高位配置とプロセス別アドレス空間は今後の段階です。
カーネル領域とガードはPMMの確保済みページではないため、動的APIで別名マッピングを作ることも拒否します。
動的に確保したページについては従来どおり明示的な実行許可を設定できます。全別名を横断したW^Xの強制は未実装です。

## 自動テスト

`scripts/test_pmm.py` は同じ `pmm.c` をホスト用共有ライブラリとして読み込み、合成メモリマップで
領域の除外、4 GiB境界、不正な記述子、枯渇、二重解放、再利用を検証します。
macOSではCommand Line ToolsのSDKを使用します。別の配置なら `HOST_SDK` で指定できます。
標準の `make` がXcodeライセンス確認で起動できない環境では、Homebrewの `gmake test` / `gmake run` を使えます。

`make test` は生成物の検証に加え、次の24ケースをQEMUで逐次実行します。

- 本物の `kernel.elf` を起動し、`kernel_halt`でのRIP、`HLT=1`、`IF=0`、RIP直前のHLT命令、および動的ページ管理テスト成功を含むシリアルログ7行をCRLFも含めて確認。
- カーネルファイルの欠落、ELF識別子の破損、ファイル範囲外のセグメント、無効な入口を拒否。
- PT_LOADを追加したテスト用ELFで、データのコピーとBSS先頭・末尾のゼロ初期化を確認。
- 重複するロードセグメントを拒否。
- 不正命令、一般保護例外、ページフォルト、ダブルフォルトを発生させ、例外番号・エラーコード・停止位置を確認。ページフォルトではCR2、ダブルフォルトではISTスタックの使用も確認。
- ページ0への読み取りでページフォルトが発生し、CR2が0になることを確認。
- 動的マッピングの読み取り専用化、実行許可の取り消し、unmapの後にアクセスし、ページフォルトのエラーコード（3・17・0）とCR2を確認。TLBに変換が残る状態から権限を変更します。

追加の9ケースではコード・読み取り専用データへの書き込み、データ・スタックの実行、両スタックの4つのガードへのアクセスを検証します。
実際の通常スタック下端でpushして、ダブルフォルトがIST上で処理されることも確認します。
ページテーブル全体の検証では各領域の権限とガード4ページの不在を確認します。

起動時の `paging_boot_check()` は別名マッピングの読み書き、重複・不正入力の拒否、権限照会、
再マッピングによる物理ページの切り替え、中間テーブルの回収を検証します。
さらに一時的に空き物理ページを残り2ページまで確保し、3段の中間テーブルを必要とするmapの失敗と後始末を確認します。
保持したページはすべて解放し、再びmapできることと空きページ数の復元を確認します。

テスト用の変更は一時ディスクだけに適用し、ビルド済みカーネルは変更しません。
正常起動ケースでは、受け取った起動情報、専用スタックの上端・C処理開始時・停止時のRSP、
最終メモリマップ上のカーネルと起動情報領域のメモリ種別・範囲も確認します。
GDTR・IDTR・セグメントセレクター・TR、およびIDT全256エントリとTSSのIST設定も読み取ります。
さらにCR3の変更とページテーブル全体を検証し、メモリマップから求めた対象RAMだけが恒等マッピングされていること、
全ページテーブルが物理ページ管理で確保済みとして保持されていることを確認します。
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
include/x86.h            x86命令を操作ごとに関数化した共通ヘッダー
kernel/entry.S          専用スタックへの切り替えとC入口への移行
kernel/main.c           起動情報の確認と最小カーネル
kernel/serial.c         カーネル用COM1ポーリング出力
kernel/serial.h         シリアル出力のインターフェース
kernel/tables.c         GDT・IDT・TSS構築と例外診断
kernel/tables.h         テーブル初期化と例外フレームの定義
kernel/interrupts.S     テーブル切り替えと各例外の入口
kernel/pmm.c            物理ページの管理・確保・解放
kernel/pmm.h            物理ページ管理API
kernel/pmm_check.c      起動時の実メモリ自己テスト
kernel/paging.c         恒等マッピングの構築とCR3切り替え
kernel/paging.h         ページテーブル初期化API
kernel/paging_check.c   動的マッピングの自己テストとQEMU用の例外テスト入口
kernel/main.h           カーネル入口の宣言
kernel/linker.ld        ELF64の配置・保護境界・ISTスタックとガード
kernel/layout.h         リンカが定義するページ境界の宣言
scripts/check_build.py  生成物の検証
scripts/test_boot.py    QEMUでのカーネル起動と異常系の検証
scripts/test_pmm.py     合成メモリマップによる物理ページ管理の検証
scripts/qemu.py         QEMU起動・ファームウェア検出
Makefile               独立したコンパイル・リンク規則
```

Cコードから使うインラインアセンブラは `include/x86.h` の操作別関数に集約します。
レジスタ、MSR、CPUID、ポートI/O、割り込み無効化、待機、TLB無効化は `x86_*()` 経由で操作します。
RSP取得は呼び出し元のスタックを測定するため、常にインライン展開します。
スタック切り替えや例外入口など、既存の `.S` ファイル内の処理は引き続きアセンブリ関数として扱います。

次の段階では、タイマー割り込みなどを小さく追加していきます。

## 参照資料

- [UEFI Loaded Image](https://uefi.org/specs/UEFI/2.10/09_Protocols_EFI_Loaded_Image.html): 起動元デバイスの取得。
- [UEFI Media Access](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html): ファイル操作。
- [UEFI Boot Services](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html): メモリ確保・Boot Services終了。

- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): 64-bit GDT、IDT、TSS、ISTと例外フレーム。
