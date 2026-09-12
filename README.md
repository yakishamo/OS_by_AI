# OS_by_AI

x86_64向けのOSとブートローダーを、小さな段階に分けて基本C言語で開発します。
操作環境はシリアルコンソールです。

現在は、**UEFIのBoot Servicesを終了して最小カーネルに制御を渡す段階**です。
カーネルは通常の割り込みを無効にし、`hlt`を繰り返します。入力待ちやシェルはありません。

## ビルド・実行

必要なツールはLLVM版Clang / LLD、GNU Make、Python 3.9以上、`qemu-system-x86_64`、
対応するOVMF / EDK IIファームウェアです。Pythonの追加パッケージは不要です。
macOS arm64 / LLVM 21.1.8 / QEMU 10.2.0 / Python 3.14.6 / GNU Make 3.81で開発しています。

```sh
make doctor  # ツールとファームウェアの配置確認
make build   # ローダーとカーネルをビルド
make run     # QEMUで起動
make test    # カーネルのHLT状態を自動確認
make clean   # build/ の生成物を削除
```

HomebrewのLLVMを使う場合のPATH設定例:

```sh
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

`make run`では、ファームウェアのログに続いて次が表示され、その後カーネルが停止します。
最後のアドレスはロード先によって変わります。これらのログはブートローダーの出力です。

```text
BOOT: UEFI x86_64 C entry
BOOT: EFI_SERIAL_IO_PROTOCOL via LocateProtocol
BOOT: serial ready (115200 8N1)
BOOT: preparing ExitBootServices; kernel will halt
BOOT: kernel entry=0x...
```

画面が進まず入力に反応しないのが、この段階での正常動作です。
終了は **Ctrl-aを押して離し、x**。Ctrl-aを押して離し、cでQEMUモニターに切り替わります。
GUIウィンドウは開きません。

## 今回の起動構成

```text
QEMU q35 / x86_64 / TCG / 1 CPU / 256 MiB
  → OVMF
  → EFI/BOOT/BOOTX64.EFI
    → boot/main.c: UEFI Serial IOでログ出力
    → GetMemoryMap → ExitBootServices
    → kernel/main.c: CLI → HLTループ
```

- ローダーとカーネルは別のCファイルとしてコンパイルし、同じPE/COFF起動イメージにリンクします。独立したカーネルファイルやELFローダーは次の段階です。
- コンパイル対象は `x86_64-pc-windows-msvc` のfreestanding C17です。UEFI x64のABIとPE/COFF形式のための指定で、Windowsやホストのlibcには依存しません。
- カーネルはUEFIのヘッダーやプロトコルを使いません。現段階では、起動時のスタック、ページテーブル、GDT等を引き継ぎ、メモリの再利用は行いません。
- `LocateProtocol()`で取得した `EFI_SERIAL_IO_PROTOCOL` を使い、115200 baud / 8N1でログを出します。Serial IOは1台の構成を前提とします。
- ウォッチドッグを無効化し、ログ出力をすべて済ませてから最終メモリマップを取得します。マップ取得と `ExitBootServices()` の間には別のサービス呼び出しを挟みません。
- マップキーが無効になった場合はマップを取り直し、最大8回試します。最小構成としてマップ用バッファは固定64 KiBです。最初の取得で不足した場合はエラー表示して戻ります。
- 一度でも終了を試みた後は、失敗してもUEFIの出力や呼び出し元への復帰を行いません。`boot_exit_failure`にステータスを保存し、ローダー内のPAUSEループで停止します。カーネルのHLTとは区別できます。
- 終了成功時だけ `kernel_main()` に進みます。割り込み処理は未実装なので、CLIで通常の割り込みを無効化します。NMI等への対応は今後の課題です。
- 起動ディスクには今回生成した `BOOTX64.EFI` だけをコピーします。読み取り専用の仮想FATディスクをQEMUが提供し、変数領域も毎回テンプレートから複製します。

## 自動テスト

`make test` はシリアルログに加え、QMP経由でQEMUのCPU状態を読み取ります。
成功条件は次のすべてです。

- ローダーが出力したカーネルの実行アドレス付近にRIPがある。
- CPUが `HLT=1`、割り込み許可フラグが `IF=0` である。
- RIP直前の命令が実際に `hlt` である。

現在の数命令だけのカーネルに合わせ、入口から32バイト未満を確認します。
カーネルを拡張するときはテストの到達点も更新します。
QMPは標準入出力のパイプで接続し、ネットワークポートは使いません。

ログは `build/serial-test.log`、CPU状態は `build/kernel-test.log`、
QEMUのエラーは `build/qemu-test.log` に保存します。テストは同時実行せず、逐次実行してください。
既定の制限時間は60秒で、失敗・タイムアウトは非ゼロで終了します。

```sh
python3 scripts/qemu.py test --timeout 120
```

## ツール・ファームウェアの指定

Homebrew同梱の `edk2-x86_64-code.fd` と `edk2-i386-vars.fd`、および一般的なLinuxの
OVMF配置先を検索します。Linux上での実行は未検証です。
自動検出できない場合は、同じ配布物・サイズ構成のコードと変数テンプレートを指定します。

```sh
export OVMF_CODE=/path/to/OVMF_CODE.fd
export OVMF_VARS=/path/to/OVMF_VARS.fd
make doctor
make test
```

ファームウェアにはSerial IOプロトコルが必要です。Shellの内蔵は不要です。
コンパイラー・リンカー・PythonはMake変数、QEMUは環境変数で変更できます。
ツールやコンパイル設定を変えた場合はクリーンビルドしてください。

```sh
make build CLANG=/path/to/clang LLD=/path/to/lld
QEMU=/path/to/qemu-system-x86_64 make run
make test PYTHON=/path/to/python3
```

## デバッグ

先に `make build` を実行し、リポジトリ直下で起動したx86_64対応GDBから接続します。

```text
target remote | make -s debug
continue
```

ファームウェアの入口で停止して接続を待ちます。シリアル出力は `build/serial-debug.log` へ保存します。
GDB自体は今回インストールしていません。UEFIイメージの再配置先に合わせたシンボル登録は今後整備します。

## ファイルと次の段階

```text
boot/main.c       UEFI起動、ログ、Boot Services終了、カーネルへの引き渡し
boot/efi.h        使用するUEFI ABIの型・プロトコル宣言
kernel/main.c     CLI / HLTだけの最小カーネル
kernel/main.h     カーネル入口の宣言
scripts/qemu.py   QEMU起動、ファームウェア検出、停止状態テスト
Makefile          ビルドと操作の入口
```

次は独立したカーネルファイルの読み込みと起動情報の受け渡しを小さく実装します。
その後、例外処理、メモリ管理、割り込み、ユーザー空間、シリアルシェルへ順に進めます。

## 参照資料

- [UEFI Boot Services](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html): GetMemoryMap / ExitBootServicesの呼び出し条件。
- [UEFI Console Support](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html): Serial IOプロトコル。
- [QMP仕様](https://www.qemu.org/docs/master/interop/qmp-spec.html): QEMUとの制御用通信。
