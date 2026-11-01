# WSL2 で KosabaOS を試すガイド

ステップ 4 までの KosabaOS を、Windows 上の WSL2 (Ubuntu) で
ビルドして QEMU で動かすための手順です。

## 1. WSL2 と Ubuntu の用意

管理者権限の PowerShell で:

```powershell
wsl --install -d Ubuntu
```

再起動後、Ubuntu のターミナルが開いてユーザー名とパスワードの
設定を求められるので済ませます。以後の作業はすべて Ubuntu の
ターミナルの中で行います。

## 2. 必要なツールを入れる

```sh
sudo apt update
sudo apt install -y git gcc-riscv64-linux-gnu qemu-system-riscv
```

- `gcc-riscv64-linux-gnu`: クロスコンパイラ (`riscv64-linux-gnu-gcc`)
- `qemu-system-riscv`: `qemu-system-riscv64` と OpenSBI ファームウェア
  (Ubuntu の版が古い場合は `qemu-system-misc` に含まれています。
  その場合はそちらを入れてください)

これが一番手軽な方法です。`build.sh` は、プロジェクト専用の
`tools/` が無ければシステム側のツールを自動で使うので、
WSL2 ではこの方法でそのまま動きます。

## 3. 取得とビルド

clone 先は Windows 側 (`/mnt/c/...`) ではなく、WSL2 のホーム
配下にしてください (ファイル IO が速く、権限周りも素直です)。

```sh
cd ~
git clone https://github.com/hrkt/KosabaOS.git
cd KosabaOS
./build.sh
```

`build.sh` はビルドの後に 5 秒だけ QEMU を起動して確認する
ところまでやります。次のように出れば成功です:

```
build: CC=riscv64-linux-gnu-gcc
build: QEMU=qemu-system-riscv64
vm: Sv39 paging on (identity map)
hello from KosabaOS (S-mode)
tick 1
tick 2
...
```

## 4. 対話で試す

自分でキーを打って試すときは、QEMU を直接起動します:

```sh
cd ~/KosabaOS/arch/riscv64
qemu-system-riscv64 -machine virt -bios default -kernel kernel.bin -m 128M -nographic
```

キー操作:

| 操作 | 動き |
| --- | --- |
| 普通のキー | カーネルがそのまま打ち返す (echo)。1 秒ごとに `tick N` が割り込む |
| `u` | ユーザープログラムをユーザーモードで実行。`hello from user mode!` と出て、そのプログラム側の echo になる。実行中も tick は続く |
| `q` (ユーザープログラム実行中) | プログラムが exit してカーネルの echo に戻る |
| `x` | ページフォルトのデモ。マップされていない住所を触って `page fault (code=13)` と `stval` を表示し、カーネルは停止する |
| `Ctrl-A` の後 `x` | QEMU を終了する (上の `x` キーとは別物) |

困ったときの切り分け:

- `trap: user program fault ...` と出てカーネルに戻るのは、
  ユーザープログラムだけが終了した正常な保護動作です
- `x` のデモの後はカーネルが止まったままになるのが仕様です。
  QEMU ごと終了してください

## 5. 補足

- 別のツールチェーンを使いたいときは環境変数で差し替えられます:
  `CC=riscv64-unknown-elf-gcc QEMU=qemu-system-riscv64 ./build.sh`
- 常用環境では、xPack のコンパイラ等をリポジトリ外の `tools/` に
  置く方法もあります (リポジトリのトップ README のセットアップ
  手順を参照)。`tools/` があると `build.sh` はそちらを優先します。
  ただし既存の `tools/riscv-gcc` は絶対パスのシンボリックリンク
  なので、別のマシンへ持ち込むときはリンクを張り直してください
