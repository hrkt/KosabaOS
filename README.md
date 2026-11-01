# KosabaOS

特定のチップに縛られない自作 OS。最初のターゲットは riscv64、
将来 arm64 (Raspberry Pi など) と x86_64 にも広げられるよう、
アーキテクチャ依存のコードは `arch/<arch>/` に閉じ込め、
共通コードはその外に置く。

## 現状: arch/riscv64 の最小カーネル

- 起動: QEMU virt + デフォルト BIOS (OpenSBI) の下で S-mode に入る
- 構成: `arch/riscv64/start.S` がスタックと .bss の準備をして
  C の `kmain` (`kernel.c`) を呼び、UART ドライバ (`uart.c`) 越しに
  1 行出し、その後は打たれたキーをそのまま打ち返す (echo)
- 実行: `./build.sh` または `./build.sh riscv64`
  （ビルドして QEMU で5秒だけ起動）
- 確認済み出力: `hello from KosabaOS (S-mode)` の後に echo が続く
- ファイル: `arch/riscv64/` に start.S、kernel.c、uart.c / uart.h、
  kernel.ld (0x80200000)、build.sh、run.log、instructions.md
  (命令・レジスタ・メモリの早見表)
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
