# カーネルビルドの棚卸し — gcc ビルドの構成要素と IwaC 化の分類

これは調査記録（読んで学ぶためのドキュメント）です。仕様では
ありません。2026-11-08、長期目標「KosabaOS カーネルを IwaC で
ビルドする（gcc を使わない）」の最初の棚卸しとして、現行の
gcc ビルドを構成する要素を全部洗い出し、各要素が IwaC 化の
観点でどこに分類されるかを、ファイル名と実測の数つきで
残します。将来の設計（Asari のアセンブリ入力前段、IwaC の
カーネル向け出口）の入力資料になることを目的とします。

対象は main `be3160b` 時点のツリー。数はすべて
`arch/riscv64/` 配下（共通コード＋ boards/virt ＋ boards/k210）
を grep / wc で実測したもので、推測で書いた箇所にはその旨を
明記します。

## 規模の全体像

| 要素 | 量 |
|---|---|
| カーネル共通 C（11 ファイル） | 6,388 行 |
| ボード C（virt 3 ＋ k210 10 ファイル） | 2,000 行 |
| ヘッダ（共通＋ボード） | 786 行 |
| アセンブリ .S（7 本） | 683 行 |
| リンカスクリプト（kernel.ld ×2 ＋ user.ld） | 123 行 |
| 同梱ユーザープログラム C（13 本） | 2,314 行 |
| build.sh | 129 行 |

カーネル C の大きい順は trap.c 2,228 行、fat.c 1,293 行、
fs.c 545 行、gfx.c 460 行、proc.c 412 行、vm.c 392 行。
ボード C は k210 側が大半（lcd.c 438、sd.c 329、i2s.c 281、
mshim.c 256、rtc.c 170、sd_hw.c 145、uart.c 113）で、
virt 側は uart.c 76、ramdisk.c 110、rtc.c 42 のみ。

## 1. ビルドの流れ（build.sh）

ツール選択は「環境変数 $CC/$QEMU → リポジトリ外の
`~/workspace/kosaba-os/tools/`（xPack riscv-none-elf-gcc
15.2.0 とポータブル QEMU 8.2.2、合計約 2.0GB、コミット
対象外）→ システムの riscv64-linux-gnu-gcc」の順。
IwaC 化の文脈では、置き換える対象はこの gcc 一式と
objcopy で、QEMU は実行環境なので無関係です。

共通フラグは `-march=rv64gc -mabi=lp64 -mcmodel=medany
-ffreestanding -nostdlib -no-pie -Wl,--build-id=none`。
march/mabi はツールチェーンごとのデフォルト差を消すため
明示（build.sh のコメントに明記）。medany はカーネルが
0x80200000 に居るため。最適化フラグは指定なし（-O0 相当）
で、rv64gc の G が乗除算を実命令にするため libgcc の
補助関数も呼ばれない（後述のとおり memcpy 等の暗黙呼び
出しも実測 0 件）。fptest.elf と tone.elf だけが検証目的で
`-mabi=lp64d`（FP レジスタ渡し）。

ビルド順序:

1. ユーザープログラム 13 本を 1 本ずつ単独でコンパイル
   （各 .c ＋ user.ld → .elf）。
2. （virt のみ・任意）Ikesu 素材が揃えばコピーして
   `-DIKESU_EMBED` を付ける。
3. （virt のみ）make_sd_image.py で sd.img を生成
   （SD exec 検証用に user.elf を UPROG.ELF として取り込む）。
4. カーネル本体: **$CC の 1 回の呼び出し**で、ボードの
   .S ＋ kernel.c ＋ ボードの .c 群 ＋ 共通の .S/.c 群
   （trap.S、trap.c、vm.c、page.c、klog.c、time.c、user.S、
   embed.S、proc.c、switch.S、elf.c、fs.c、fat.c、gfx.c）
   をコンパイルとリンクまで一括で行い、`-I. -Iboards/$BOARD`
   と `-T boards/$BOARD/kernel.ld` で kernel.elf を作る。
5. objcopy -O binary で kernel.bin 化。
6. （virt のみ）QEMU で 5 秒の起動確認（BUILD_NO_RUN=1 で省略可）。

つまり現行ビルドで gcc のドライバが担っている役は 4 つ:
C コンパイラ、（cpp を通す）アセンブラ、リンカドライバ、
そして別立ての objcopy。自作側の対応は iwac（C）、
Asari（符号化）、nori（リンク）で、objcopy 相当
（ELF → 生バイナリ）は現行の自作側には無い。

### リンカスクリプトの担う約束

- boards/virt/kernel.ld（64 行）: 配置基点 0x80200000、
  .text → .rodata → .data（.sdata 含む）→ .bss（.sbss、
  COMMON 含む）の順、`__bss_start` / `__bss_end` シンボルの
  書き出し、BSS がページプール（0x82200000）に食い込まない
  ことの ASSERT。
- boards/k210/kernel.ld（33 行）: 基点 0x80000000、
  `.text.start` を真っ先に置く指定が追加（ROM の ISP スタブが
  ファイル先頭へ直接ジャンプしてくるため）。ASSERT は無い。
- user.ld（26 行）: コード 0x40000000 / データ 0x40200000 の
  分離（W^X）。これは IwaC 側の kosaba.ld が同じ約束で
  複製を持っている（IwaC リポジトリ kosaba/kosaba.ld）。

nori には現状、スクリプトを読む仕組みも任意基点への配置も
無く、ユーザープログラムの固定レイアウト専用です
（IwaC リポジトリ doc/linking.md）。カーネルの配置の約束
（基点、セクション順、境界シンボル、ASSERT）は、IwaC 化
では nori 側の新しい仕事になります。

## 2. アセンブリ（.S 7 本、683 行）

| ファイル | 行 | 役割 |
|---|---|---|
| trap.S | 181 | S モードのトラップ入口 ×2（カーネル用 trap_entry、ユーザー用 trap_entry_user）。31 レジスタの退避/復元、C ハンドラ呼び出し、sret |
| user.S | 106 | ユーザーモードへの出入り（user_launch / user_exit_jump）。カーネル文脈の保存、CSR 設定（sscratch/stvec/sstatus/fcsr）、sret |
| embed.S | 107 | 生成物の埋め込み（後述）。.incbin 13 本＋ IKESU_EMBED 時 3 本 |
| boards/virt/start.S | 91 | S モード起動（OpenSBI から）。スタック設定、BSS ゼロ埋め、kmain 呼び出し、駐車 |
| boards/k210/mshim.S | 87 | K210 の M モードトラップ入口。mscratch 交換、全レジスタ退避、C ハンドラ呼び出し、mret |
| boards/k210/start.S | 62 | M モード起動。hart1 駐車、mtvec 設定、スタック/BSS、mshim_boot 呼び出し |
| switch.S | 49 | コンテキストスイッチ。ra/sp/s0〜s11 の保存と復元、ret |

### 使っている GNU アセンブラの機能（実測）

- ディレクティブ: `.section`（10 箇所。固有名 .text.start
  を含む）、`.globl`（42）、`.align`（8。トラップ入口の
  4 バイト境界要件を含む）、`.space`（5。スタックと
  kernel_context の確保）、`.incbin`（16）。
  **`.macro` と `.option` は不使用**（実測 0 件）。
- ラベル: 通常ラベル、`.L` ローカルラベル、数字ラベルと
  `1f` / `2b` の前後参照。
- 疑似命令: la（2）、call、j、mv、li、ret。
- 相対アドレス: `%pcrel_hi` / `%pcrel_lo` のペアが 12 組
  （start.S のスタック/BSS 参照、user.S の kernel_context
  等）。リンカ側のリロケーション対（HI20/LO12）の解決が前提。
- 特権命令: CSR 系（csrr/csrw/csrs/csrc/csrrw）が .S 内に
  15 行、sret が 3 箇所、mret が 1 箇所、wfi が各 start.S。
- **cpp 前処理を通る**: 大文字 .S は gcc ドライバが cpp を
  かける。実例は embed.S の `#ifdef IKESU_EMBED`。

### Asari の現状との差

Asari は現状「コンパイラの符号化段」で、入力は .ias
（1 命令 1 行、位置はバイト住所ではなく命令番号で表す
テキスト。IwaC リポジトリ doc/asari.md）。符号化できる
命令の語彙は gen が出す OP 表そのもので、実測で
数えると 40 種: 整数演算（add/sub/and/or/xor/slt 系と
即値形）、mul/div/rem 系、ロード/ストア（lb/lbu/lh/lhu/
lw/lwu/ld、sb/sh/sw/sd）、分岐は **beq/bne のみ**、
jal/jalr、lui/auipc、ecall、wfi。

カーネルの .S が要るのに OP 表に無いもの: **CSR 系すべて、
sret、mret、sfence.vma、fence.i**。逆に switch.S は
sd/ld/ret だけでできていて、語彙の範囲内に収まる唯一の
1 本です。データ側の仕組み（.space/.align/.incbin と
開始/終了ラベルの対）も .ias の世界には無いので、
Asari のアセンブリ入力前段は「文法を読む」だけでなく
「データと配置を扱う」段にもなります。

embed.S の特殊性: 中身は命令ではなく生成物（ビルド済み
ユーザープログラム ELF）のバイト列で、仕事は
「ファイルのバイト列を .rodata に置き、開始と終了の
ラベルを付ける」だけです。同型の埋め込みは
boards/virt/ramdisk.c も C のトップレベル `__asm__` で
行っています（sd.img の .incbin）。どちらも言語機能と
いうよりビルド工程の機能です。

## 3. カーネル C の gcc 依存（実測）

### asm volatile: 39 文＋α

| ファイル | 件数 | 中身 |
|---|---|---|
| trap.c | 15 | CSR 読み書き（scause/sepc/sstatus/stval/time/stvec/sie/sscratch/fcsr）、wfi |
| boards/k210/mshim.c | 18 | M モード CSR（mcause/mepc/mtvec/mscratch/medeleg/mideleg/mie/mstatus/fcsr）、生 CSR 番号 0x180（K210 の sptbr）、`.word 0x10400073`（sfence.vm の生符号化）、mret、wfi |
| proc.c | 3 | csrr time、csrr stvec、csrs sstatus |
| vm.c | 1 | csrw satp（ページング点火） |
| elf.c | 1 | fence.i（コード読み込み後の命令キャッシュ無効化） |
| boards/virt/ramdisk.c | 1 | トップレベル `__asm__` による .incbin 埋め込み（上記） |

ほかに trap.c の SBI 時刻設定呼び出しは、レジスタ変数宣言
（`register uint64_t a0 asm("a0")` 等 3 件）と ecall の対で
できています。board.h のマクロ `BOARD_SFENCE_VMA()` の
中にも asm が隠れています（virt = sfence.vma 命令、
k210 = .word 生符号化。共通コードは vm.c からマクロ越しに
使う）。asm の大半は CSR アクセサとして static inline 関数
に包まれており（trap.c に 12 関数の集中がある）、利用側の
コードは通常の関数呼び出しの形をしています。

### __attribute__: 7 件、全部 aligned

vm.c のページテーブル 3 つ（aligned(4096)）、
boards/k210/mshim.c のブート用ページテーブル 2 つ
（aligned(4096)）と M スタック（aligned(16)）、
proc.c のタスクスタック（aligned(16)）。配置の約束を
コンパイラに伝える用途のみで、`packed` は実測 0 件。

### volatile と MMIO の型

asm を除いた volatile は 9 ファイル 16 行ぶん。実態は
MMIO 読み書きの型で、2 つの形に集約されます:

- ドライバごとのマクロ: `REG` / `REG32` / `REG64` の
  定義が 7 箇所（boards/virt/uart.c と boards/k210 の
  uart.c、sd_hw.c、lcd.c、i2s.c、rtc.c、mshim.c）にあり、
  展開形はすべて `(*(volatile T *)(住所))`。利用箇所は
  合計約 114 箇所（ファイル別: i2s.c 25、lcd.c 23、
  sd_hw.c 22（FPIOA 含む）、rtc.c 20、virt uart.c 14、
  k210 uart.c 10）。
- 直接の `(volatile ...)` キャストが 13 箇所。

IwaC には volatile という語がありません（キーワード
実測 0）。最適化をしない生成なら実害は小さいはずですが、
「MMIO は必ずメモリへ読み書きする」という保証の置き場が
IwaC 化では要ります。

### 前処理（プリプロセッサ）

カーネル C＋ヘッダの実測: `#include` 111 行
（うち `<stdint.h>` が 34、残りは自前のヘッダのみ。
stdio/string 等の libc ヘッダは不使用）、`#define` 297 行、
`#if` 25 行、`#ifndef` 18 行（ヘッダガード）、`#ifdef` 2 行。

IwaC の前処理（IwaC リポジトリ doc/design.md で確認）は
`#define`（名前のみ、単語単位の置換。v13 から）と
`#ifdef` / `#ifndef` / `#else` / `#endif`（v18 から）を
持ち、`__IWAC__` / `__KOSABA__` の定義済みマクロがある。
**無いもの: `#include`、引数つきマクロ、`#if`（式の評価）**。

対応づけると:

- #define 297 行の大半は定数マクロで、IwaC の #define の
  範囲に概ね入る。ただし引数つきマクロは定義箇所で
  実測 13 件あり（REG 1 件、REG32 5 件、REG64 1 件、
  FPIOA、LEAF、PTR、LCD_RGB、BOARD_SFENCE_VMA 2 件）
  で、これらは範囲外。
- #if 25 件は `#if BOARD_FEATURE_*` 形（board.h が 0/1 を
  定義し、機能の有無を切り替える型）が中心で、#ifdef への
  書き換えか、ボード別ファイル集合の切り替えが要る。
  IwaC 自身のビルドがプロファイルごとに断片の集合を変える
  方式（shirasu / frag）なのは、後者の前例です。
- #include 111 行と多ファイル構成そのものは、IwaC の
  「断片を連結して 1 翻訳単位にする」モデルへの
  組み替えが要る（宣言の受け渡しはプロトタイプ v24 で
  扱える）。ヘッダガードは連結モデルでは不要になる。

### IwaC サブセットとの差分（構文・意味の単位で実測）

IwaC 側の現状は IwaC リポジトリの doc/design.md、
doc/struct.md と字句解析のキーワード実測で裏を取りました
（switch / case / enum / union / volatile / goto / float /
double はキーワードとして存在しない）。

| 項目 | カーネルの使用 | IwaC の現状 |
|---|---|---|
| switch 文 | 2 件（trap.c の syscall 振り分け、lcd.c） | 未対応 |
| do-while | 1 件（boards/k210/uart.c） | 未対応 |
| sizeof | 44 件、**すべて式の形**（sizeof(buf) 等） | 型の形のみ（v37） |
| 配列初期化子 | 約 48 件、**すべて関数内 static**（lcd.c の字形表 44、time.c の dim、proc.c の names、mshim.c の slot） | ファイルスコープのみ（v40） |
| 構造体の指定初期化子 | 2 件（ramdisk_dev、sd_dev。どちらも struct blkdev） | 未対応（構造体の初期化リスト自体が対象外、doc/struct.md） |
| 関数ポインタ | 3 概念（後述） | 言語に無い |
| 構造体の値渡し・代入 | grep の範囲で使用箇所なし（カーネルはメンバ単位で読み書きする書き方） | エラーになる（v37 の制限と偶然一致） |
| 三項演算子・%・64bit 除算 | 使用あり | 対応あり |

関数ポインタの 3 概念は範囲が限られます:
(1) blkdev の操作表（struct blkdev の read/write フィールド、
blkdev.h。実装は ramdisk と SD の 2 つ）、
(2) ELF ローダの読み口（elf.h の typedef elf_read_at_fn。
実体は fs_read_at 系と fat_read_at 系のアダプタ 2 つ）、
(3) カーネルタスクの本体ポインタ（proc.c の struct task の
fn フィールド。実体は時計タスク等の数個）。
どれも実装の集合が小さく固定で、タグ＋if 分岐への
書き換えが機械的にできます。ただし呼び出し箇所は
ドライバとローダとスケジューラに散ります。

### 使われていないもの（確認できた範囲）

以下はカーネル C＋ヘッダの全数 grep で **0 件**でした。
IwaC 化の障害にはなりません:

`__builtin_*`、可変長引数（va_list / stdarg）、enum、
union、ビットフィールド、匿名の `typedef struct { ... } 名前;`、
文式 `({ ... })`、複合リテラル、goto、memcpy / memset /
memmove / memcmp の呼び出し（カーネルは自前の文字列関数を
別名で持ち、gcc の暗黙呼び出しも -O0 のため発生しない）。
浮動小数点もカーネル C では不使用（fcsr の初期化は asm 側）。
print 系は固定引数の自前実装（klog.c / kernel.c）で、
可変長引数を避けた書き方が既になっています。

## 4. 分類

### (a) Asari のアセンブリ入力前段があれば扱えるもの

- switch.S（49 行）: 語彙が現行 OP 表の範囲内。前段の
  文法（ラベル、.section、.globl）が揃えばそのまま通る
  見込みの唯一の 1 本。
- start.S ×2、trap.S、user.S、mshim.S の命令列の骨格
  （退避/復元の sd/ld、call、j、数値ラベル、%pcrel ペア）:
  文法としては前段の担当。ただし後述 (b) の符号化拡張が
  無いと特権命令の行で止まる。%pcrel_hi/lo の対は、
  バイト住所ベースのリロケーションを nori 側が解く
  仕組みとセットで要る。
- 規模: .S 683 行のうち embed.S を除く 576 行がこの範囲。

### (b) IwaC / nori にカーネル向けの口が要るもの

コンパイラ・リンカ側の新設が要るもので、ソースの
書き換えでは吸収できません:

- **特権命令の符号化**: CSR 系、sret、mret、sfence.vma、
  fence.i が Asari の OP 表に無い。.S 側で 15 行、
  これが無いと (a) が骨格だけになる。逃げ道の前例として、
  カーネル自身が K210 の sfence.vm を `.word` の生符号化で
  済ませている点は記録に値します（board.h と mshim.c の
  2 箇所）。
- **CSR アクセスを C から使う口**: asm volatile 39 文＋
  レジスタ変数 3 件＋マクロ隠れ 2 件。IwaC には
  syscall3 組み込み（v27）の前例があるので、「組み込み
  関数の形で CSR 読み書きと ecall を出す」設計が候補に
  なります（これは設計の話で、本書は棚卸しに留めます）。
- **配置**: カーネル基点（0x80200000 / 0x80000000）、
  .text.start の先行、__bss_start / __bss_end の書き出し、
  プール境界の ASSERT、ELF → 生バイナリの変換（objcopy
  相当）。nori の現在の固定レイアウトの外の仕事。
- **入口の契約**: カーネルに main は無く、_start（asm）→
  kmain の形。IwaC のプログラム形（main 入口、データ領域
  の相対配置）とは別の出口が要る。
- **volatile の保証**: MMIO の型（マクロ約 114 箇所＋
  直接 13 箇所）の土台。言語に volatile を足すか、
  MMIO 用の組み込みに寄せるかの判断が要る。
- **aligned の指定**: 7 件（ページテーブルの 4096 揃えが
  本質）。言語の属性か、nori の配置機能か、どちらかの
  受け口が要る。

### (c) ソースの書き換えで IwaC サブセットに寄せられるもの

機械的な書き換えで済むもの。対象はカーネル C＋ヘッダ
約 9,174 行の組み替えが土台にあります:

- 多ファイル＋ #include（111 行）→ 連結モデルへの
  組み替え。#define 定数（297 行の大半）は IwaC の
  #define で概ね受かる。#if BOARD_FEATURE_*（25 件）は
  #ifdef 化かボード別ファイル集合へ。
- 引数つきマクロ 13 定義箇所の解体（REG 系は利用約 114 箇所と
  散らばるが、展開形は一様）。(b) の volatile の受け口と
  セット。
- sizeof の式形式 44 件 → 型形式または定数へ。
- 関数内 static の配列初期化子 約 48 件 → ファイルスコープ
  への引き上げ。
- switch 2 件 → if-else。do-while 1 件 → while。
- 構造体の指定初期化子 2 件 → 初期化関数でのメンバ代入へ。
- 関数ポインタ 3 概念 → タグ＋分岐へ（実装集合は
  2 / 2 / 数個と小さい）。
- static inline の CSR アクセサ群（17 箇所）は (b) の
  組み込みができれば通常の関数として残せる。

### (d) ビルド対象から外せる / 生成物として扱うもの

- embed.S（107 行）と ramdisk.c の .incbin 埋め込み:
  言語ではなく工程の機能。「バイト列＋開始/終了ラベル」を
  nori のオブジェクトにデータとして載せる形に置換できる。
  埋め込まれる側のユーザープログラム ELF 自体は次の節の
  話で、生成物であることに変わりはない。
- kernel.bin への objcopy 変換: 成果物の形式の話。
  nori が生バイナリを出すか、ブート側が ELF を読むかに
  寄る（現行の契約は virt = OpenSBI が kernel.bin を、
  K210 = kflash が生バイナリを置く形）。

## 5. 同梱ユーザープログラム（13 本、2,314 行）

カーネルとは難易度が違う、というのがこの節の結論です。
現行は全て gcc ビルドですが、ユーザープログラムの
IwaC 化には実績のある経路（iwac --kosaba --elf → nori、
入口は IwaC リポジトリの kosaba_crt.S、syscall は
組み込み syscall3、配置は kosaba.ld = user.ld の複製）が
あり、ツールチェーン自身と Ikesu の独自デコーダが
その経路で動いています。

プログラム側の実測: 各プログラムは syscall ラッパを
1 つ自前で持ち（レジスタ変数＋ecall の asm が 1 本あたり
約 10 行、13 本共通の型）、switch は 0 件、#include は
`<stdint.h>` が 2 本のみ（他は自前の型宣言で済ませて
いる）。規模は nish.c 751 行が突出し、他は 46〜285 行。
浮動小数点を使うのは fptest.c と tone.c だけで、
どちらも FP 検証が目的のプログラムです（tone のサイン波
漸化式は固定小数点化が可能、fptest は FP のテスト自体が
目的なので gcc 側に残す判断もありうる）。
nami（WAV プレーヤー）の IwaC 化が、この節の現在の
一歩として別ブランチで進行中です。

## 6. まとめ — 残る仕事の形

棚卸しの headline は次のとおりです:

- カーネルの本体は約 9,174 行の C と 683 行の .S。
  gcc 依存の「点」は思ったより少なく、数えられる単位に
  収まった: asm 39 文、__attribute__ 7 件、関数ポインタ
  3 概念、switch 2 件、sizeof の書き換え 44 件、
  配列初期化子の引き上げ約 48 件。
- 構造的な障害は点ではなく面のほうにある:
  (1) Asari の符号化語彙に特権命令が無いこと、
  (2) IwaC の出口がユーザープログラム専用で、カーネルの
  配置・入口・特権の口が無いこと、(3) 多ファイル＋
  #include＋#if という構成の型が、連結モデルの IwaC と
  違うこと。
- 使われていない拡張が長いリストになったのは収穫で、
  カーネルの書き方は偶然ではなく実際に質素です
  （可変長引数なし、union/enum/ビットフィールドなし、
  libc 呼び出しなし）。IwaC のサブセットとの距離は、
  「言語を大きくする」より「カーネル向けの出口を 1 つ
  足す」ほうが本筋、という見立てが棚卸しからは出ます。

本書はここまでを記録に留め、設計（Asari 前段の文法範囲、
IwaC のカーネル向け出口の形、移行の順序）は次の仕事に
分けます。
