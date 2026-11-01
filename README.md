# KosabaOS

特定のチップに縛られない自作 OS。最初のターゲットは riscv64、
将来 arm64 (Raspberry Pi など) と x86_64 にも広げられるよう、
アーキテクチャ依存のコードは `arch/<arch>/` に閉じ込め、
共通コードはその外に置く。

関連リポジトリ: この OS の上で動かす自作コンパイラ IwaC
(https://github.com/hrkt/IwaC) と対で開発している。
clone は KosabaOS と IwaC をセットで実施するのが前提で、
「OS のシェルからコンパイラで Hello World」のデモは
両方を組み合わせて確認している。OS 側の ABI を定義して
ABI 向けバイナリをツールとして OS 側に置く話は別途検討中。

## 現状: arch/riscv64 の最小カーネル

- 起動: QEMU virt + デフォルト BIOS (OpenSBI) の下で S-mode に入る
- 構成: `arch/riscv64/start.S` がスタックと .bss の準備をして
  C の `kmain` (`kernel.c`) を呼ぶ。UART ドライバ (`uart.c`) 越しに
  1 行出した後は echo を続け、トラップ処理 (`trap.S` / `trap.c`) が
  タイマ割り込みで 1 秒ごとに `tick N` を割り込ませる。
  ページング (`vm.c`) は Sv39 の恒等マッピングで有効になっており、
  echo 中に `x` キーを押すとページフォルトのデモが動く。
  `u` キーを押すとユーザープログラム (`uprog.c`) をユーザーモードで
  実行する: 挨拶と echo はシステムコール経由 (仕様は
  `doc/syscalls.md`)、`q` でカーネルに帰る。
  `m` キーを押すとマルチタスクのデモ (`proc.c`): 2 つのタスクが
  協調的 (ABAB…) → プリエンプティブ (塊で交互) に文字を出す。
  `l` キーで RAM ファイルシステム (`fs.c`) のファイル一覧が出る。
  ユーザープログラムは `uprog.elf` というファイルとして
  ファイルシステムから名前で引かれて実行される。
  `s` キーでユーザー空間のシェル (`shell.c`) が起動する:
  `ls` / `cat <名前>` / `write <名前> <本文>` / `run <名前>` /
  `exit` が使え、
  `run uprog.elf` のようにシェルからプログラムを実行して
  シェルに戻ってこられる (システムコール v1、`doc/syscalls.md`)
- 実行: `./build.sh` または `./build.sh riscv64`
  （ビルドして QEMU で5秒だけ起動）
- 確認済み出力: `hello from KosabaOS (S-mode)` の後に echo が続く
- ファイル: `arch/riscv64/` に start.S、kernel.c、uart.c / uart.h、
  trap.S、trap.c / trap.h、vm.c / vm.h、user.S (U モードへの出入り)、
  uprog.c / user.ld (ユーザープログラム)、shell.c (シェル)、
  embed.S (ELF 埋め込み)、
  elf.c / elf.h (ELF ローダ)、fs.c / fs.h (RAM ファイルシステム)、
  proc.c / proc.h / switch.S (タスクとコンテキストスイッチ)、
  kernel.ld (0x80200000)、build.sh、test.sh (自動検証)、run.log、
  instructions.md (命令・レジスタ・メモリの早見表)
- ドキュメント: `doc/roadmap-multitasking.md` (計画と決定事項)、
  `doc/steps-1-3.html` (初心者向けまとめ)、`doc/syscalls.md`
  (システムコール仕様)
- 先の計画: `doc/roadmap-multitasking.md`（メモリ保護付き
  マルチタスクへの計画案）

はまった点（再現するときの注意）:
- `la` はデフォルトで GOT 経由になり、裸のバイナリでは動かない。PC相対の
  `auipc + addi (%pcrel_hi/lo)` で書いてある
- `.note.gnu.build-id` が先頭に入ると `_start` が 0x80200000 からずれ、
  OpenSBI のジャンプ先がコードにならない。`--build-id=none` で回避
- ELF を `-kernel` で渡すと OpenSBI の Next Address がずれる場合があったため、
  `objcopy -O binary` した `kernel.bin` を渡している
- C のコードは `-mcmodel=medany` でビルドする。デフォルトのコードモデル
  では 0x80200000 のような住所を扱えない

## 必要なツール（自分でインストールする）

自動セットアップはしない。ビルドと実行には以下が要るので、
使う人が自分で入れる（Ubuntu 24.04、確認済み: QEMU 8.2.2 /
gcc 13.3.0）。

```sh
sudo apt-get update
sudo apt-get install -y qemu-system-misc gcc-riscv64-linux-gnu
qemu-system-riscv64 --version
riscv64-linux-gnu-gcc --version
```

詳しい揃え方は、ひとつ上のトップ README (`../README.md`) に書いてある。

## 置き場所のルール

- このディレクトリ (`os/`) が自作物の Git リポジトリ。入れるのは
  自分で書いたソースだけ。ビルド生成物 (`*.elf`, `*.bin`, `run.log`) は
  `.gitignore` で除外済み。
- QEMU やクロスコンパイラなどのツールはリポジトリの外の `../tools/` に
  置く。コミットはしないが、ダウンロードしたものがローカルに
  溜まっていってよい。
- xv6 など他の人のコードは `../reference/` に置き、参照だけする。
  ここへ丸ごと取り込まない。コミットはしないが、こちらも
  ローカルに溜まっていってよい。
