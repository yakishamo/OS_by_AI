# OS_by_AI

x86_64向けのOSとブートローダーを、小さな段階に分けてC言語で開発するプロジェクトです。
当面の操作環境はシリアルコンソールとし、GUIは対象にしません。

現在は**ビルド・実行環境の構築段階**です。自作UEFIプログラムを直接起動し、
Boot Servicesの `LocateProtocol()` で取得した `EFI_SERIAL_IO_PROTOCOL` を使って入出力します。
カーネル、自作OSのシェル、ELFローダー、メモリ管理は未実装です。

## 必要なツール

- Clang / LLD（LLVM版。ホストとは別のx86_64向けコードを生成）
- GNU Make
- Python 3.9以上（ホスト側の起動・テスト用。追加パッケージ不要）
- `qemu-system-x86_64`
- OVMF / EDK IIのx86_64 UEFIファームウェアと対応する変数領域のテンプレート

この作業環境では、macOS arm64 / Clang・LLD 21.1.8 / QEMU 10.2.0 /
Python 3.14.6 / GNU Make 3.81で確認しています。追加インストールは行っていません。

HomebrewのLLVMを使う場合のPATH例:

```sh
export PATH="$(brew --prefix llvm)/bin:$PATH"
make doctor
```

`make doctor` はバージョンとファームウェアの検出結果を表示します。
最終的な環境の動作確認には `make test` を使ってください。
ホスト向けのmacOS SDKやWindows SDK、libc、NASMは現在のビルドには不要です。

## ビルドと実行

リポジトリ直下で実行します。

```sh
make build
make run
```

起動後、ファームウェアのログに続いて次が表示されます。

```text
BOOT: UEFI x86_64 C entry
BOOT: EFI_SERIAL_IO_PROTOCOL via LocateProtocol
BOOT: serial ready (115200 8N1)
BOOT: environment probe; kernel not loaded
Type to echo via UEFI Serial IO; Ctrl-a x exits QEMU.
SERIAL>
```

`SERIAL>` は自作プログラムの入力確認用プロンプトです。UEFI Shellではありません。
入力文字をUEFIの `Read()` で受信し、`Write()` で返します。
終了は **Ctrl-aを押して離し、x**。Ctrl-aを押して離し、cでQEMUモニターに切り替わります。
グラフィカルウィンドウは開きません。

```sh
make test    # プロトコル取得・起動とシリアル往復を自動検証してQEMUを終了
make clean   # build/ 以下の生成物を削除
make help
```

`make test` はプロトコル経由の起動メッセージを確認してから、毎回異なる文字列を送り、
エコーと次のプロンプトを確認します。
成功時は `PASS: UEFI Serial IO protocol discovery and serial input/output` を表示します。
アプリケーションのエラー、起動失敗、送受信失敗、60秒のタイムアウトは非ゼロで終了します。
ログは `build/serial-test.log` に保存します。同じ作業ディレクトリでのテストは逐次実行してください。

遅い環境では、ビルド後に制限時間を変更できます。

```sh
python3 scripts/qemu.py test --timeout 120
```

## 起動構成

```text
QEMU q35 / x86_64 / TCG / 1 CPU / 256 MiB
  → OVMF（既存のUEFIファームウェア）
  → EFI/BOOT/BOOTX64.EFI（自作UEFIプログラム）
  → BootServices->LocateProtocol(EFI_SERIAL_IO_PROTOCOL_GUID, ...)
  → SerialIo->SetAttributes() / Write() / Read()
  → SERIAL>（入出力確認）
```

- arm64ホストでも実行できるよう、CPUエミュレーションには明示的にTCGを使います。
- Clangのターゲットは `x86_64-pc-windows-msvc`。UEFI x64の呼び出し規約とPE/COFF形式のためで、Windowsには依存しません。
- C17のfreestanding環境でビルドし、ホストの標準ライブラリーにはリンクしません。
- 現時点のプログラムはUEFI実行環境内に留まります。`ExitBootServices` とカーネルへの制御移譲は次の段階です。
- シリアル機器が1台のQEMU構成を前提に、最初に見つかったSerial IOプロトコルを使います。複数ポートからの選択は未実装です。
- `SetAttributes()` で115200 baud / 8 data bits / no parity / 1 stop bitに設定します。1文字あたりのタイムアウトは10 msです。
- 自作コードでUARTのI/Oポートを直接操作せず、送受信はファームウェアに任せます。入力待ちの `EFI_TIMEOUT` は正常な待機として扱います。
- 送受信のエラーと実際の転送バイト数を確認します。初期化・送受信失敗時はUEFIの `StdErr`（なければ `ConOut`）に処理名とEFIステータスを表示し、エラーを返します。
- 長時間の入力待ちに備え、`SetWatchdogTimer(0, ...)` でブートウォッチドッグを無効にします。
- Serial IOプロトコルはBoot Servicesが有効な間だけ使います。将来 `ExitBootServices()` した後のカーネル出力は別途実装します。
- 起動用FATディスクには `build/esp/EFI/BOOT/BOOTX64.EFI` のコピーを配置し、QEMUが読み取り専用で提供します。以前の `osboot.efi` はコピーしません。単独配布用ディスクイメージは未作成です。
- 起動ごとに変数領域と起動ファイルを一時ディレクトリに複製します。インストール済みのファームウェアを変更せず、起動設定も毎回初期状態から始まります。
- 仮想ネットワーク機器は無効にしています。

## ファームウェア・ツールの指定

Homebrew同梱の `edk2-x86_64-code.fd` と `edk2-i386-vars.fd`、および一般的なLinuxの
OVMF配置先を検索します。Homebrewの変数テンプレートはx86_64用コードと組み合わせる場合も
`i386` というファイル名です。Linux上での実行はまだ検証していません。

検出できない場合は、同じ配布物・サイズ構成のコードと変数テンプレートを両方指定してください。
ファームウェアには `EFI_SERIAL_IO_PROTOCOL` が必要です。現在のHomebrew同梱版で動作確認済みです。
`make doctor` はファイルの存在確認までで、プロトコル取得と送受信は `make test` で確認します。

```sh
export OVMF_CODE=/path/to/OVMF_CODE.fd
export OVMF_VARS=/path/to/OVMF_VARS.fd
make doctor
make test
```

コンパイラー・リンカー・PythonはMake変数、QEMUは環境変数で変更できます。
ツールやコンパイル設定を変更した場合は `make clean` してから再ビルドしてください。

```sh
make build CLANG=/path/to/clang LLD=/path/to/lld
QEMU=/path/to/qemu-system-x86_64 make run
make test PYTHON=/path/to/python3
```

## デバッグの入口

`make debug` はGDBとの接続用で、通常の対話コンソールとは異なります。
先に `make build` を実行し、リポジトリ直下で起動したx86_64対応GDBから次を実行します。

```text
target remote | make -s debug
continue
```

QEMUをファームウェアの最初で停止させ、標準入出力でGDBと接続します。
TCPポートは使いません。ゲストのシリアル出力は `build/serial-debug.log` に保存します。
この方式はデバッグ中のシリアル入力には対応していません。起動ログは確認できます。
GDB自体は今回インストールしていません。QEMU側の接続口はプロトコル応答まで確認し、
UEFIプログラムのロードアドレスに合わせたソースデバッグ用のシンボル登録は今後整備します。

## ディレクトリ

```text
boot/main.c       UEFIエントリーポイントとSerial IOプロトコルによる入出力検証
boot/efi.h        使用するUEFI ABIの型・プロトコル宣言
scripts/qemu.py   ファームウェア検出、QEMU起動、自動テスト
Makefile          ビルドと各操作の入口
build/            生成物（Git管理外）
```

## 今後の区切り

各段階をさらに小さく分割し、QEMUで合格条件を満たしてから次へ進みます。

1. **現在:** ビルド環境、自作UEFIプログラムの直接起動、UEFI Serial IOによる送受信。
2. ELF64カーネルの読み込み、起動情報とメモリマップの受け渡し、UEFIからの離脱。
3. 例外処理とpanicによる障害の可視化。
4. 物理・仮想メモリ管理。
5. タイマー割り込みとスケジューリング。
6. ユーザー空間、syscall、プロセス分離。
7. RAMファイルシステムとシリアルシェルからのプログラム実行。
8. 永続ストレージ。以降の機能は必要性とテスト方法を決めてから着手。

## 参照資料

- [UEFI Console Support](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html): Serial IOとテキストコンソールのプロトコル。
- [UEFI Boot Services](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html): LocateProtocolとブートサービス。
- [UEFI x64環境・呼び出し規約](https://uefi.org/specs/UEFI/2.10/02_Overview.html): x64向けUEFIアプリの実行環境。
- [QEMU Disk Images](https://www.qemu.org/docs/master/system/images.html): ディレクトリからの仮想FATディスク。
- [QEMU起動オプション](https://www.qemu.org/docs/master/system/qemu-manpage.html): シリアル、TCG、デバッグ接続。
