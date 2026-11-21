# KosabaOS ブート処理 (riscv64)
# ===================================
# 起動直後、OpenSBI (ファームウェア) がこのコードを 0x80200000 に置いて
# S モードで渡してくる。C で書いたカーネル本体 (kernel.c の kmain) を
# 呼ぶには、その前にアセンブラでしかできない準備が要る。このファイルの
# 仕事はその 3 つだけ:
#   1. 作業場所 (スタック) を用意する
#   2. .bss (初期値ゼロのはずの領域) を実際にゼロで埋める
#   3. kmain を呼ぶ
# kmain から戻ってくることは想定していないが、戻ってきたら駐車する。
#
# 使うレジスタ (CPU の中にある超高速の小さな変数箱):
#   sp  : スタック (作業机) の場所
#   t0/t1: ゼロ埋めループの一時的な計算用 (呼び出しで壊れてよい約束)
#   ra  : call で飛ぶときに「戻り先の住所」を入れる箱
#
# 命令の意味を忘れたら、同じフォルダの instructions.md に早見表がある。

    .section .text          # ここから先は「命令 (コード)」の領域
    .globl _start           # _start を外から見える入口として公開
_start:                     # カーネルの入口 (kernel.ld の ENTRY がここ)

# --- 1. スタックを用意する -------------------------------------------
# stack_top (下で確保した 4KB の末尾) のアドレスを sp に入れる。
# スタックは下向きに伸びるので、末尾を指すのが決まり。
# RISC-V はアドレスを 1 命令で運べないため、上半分 (auipc) と
# 下半分 (addi) の 2 命令で運ぶ。しかも「今いる場所 (PC) から数えた
# 相対位置」で計算するので、配置がずれても壊れない。
# (普通の la 命令は裏で GOT を経由し、こういう裸のバイナリでは動かない)
#
# ここで使う 2 命令の意味:
#   auipc (Add Upper Immediate to PC)
#     計算: rd = PC + (即値 << 12)
#     元になるのはレジスタではなく PC (今実行している命令のアドレス)。
#     即値は 20 ビットで、12 ビット左にずらして使うため、足し算は
#     4KB ページ単位の大ざっぱなものになる。「目的地のページまで行く」役。
#     %pcrel_hi(目標) は、PC から目標までの距離のうち、この上位部分。
#   addi (Add Immediate)
#     計算: rd = rs + 即値
#     元になるのは指定したレジスタ。即値は命令の中に直接書かれた
#     12 ビットの符号付き数 (-2048 〜 +2047) で、小さい調整専用。
#     「ページの中の細かい位置合わせ」役。%pcrel_lo(ラベル) は、
#     ペアになる auipc が運んだ上位部分の、残りの下位部分を指す。
#
# 実例 (このファイルのビルド結果の実数):
#   PC = 0x80200000、目標 stack_top = 0x80201210 のとき
#   auipc sp, 1       → sp = 0x80200000 + 0x1000 = 0x80201000
#   addi  sp, sp, 528 → sp = 0x80201000 + 0x210  = 0x80201210 で完成
.Lsp: auipc sp, %pcrel_hi(stack_top)
    addi sp, sp, %pcrel_lo(.Lsp)

# --- 2. .bss をゼロで埋める ------------------------------------------
# .bss は「初期値ゼロの領域」という約束の場所 (スタックもこの中)。
# ファイルには実体が無く、RAM の中身は本来は不定なので、C の側が
# 「ゼロのはず」と信じて使う前に、ここで実際にゼロを書いておく。
# 範囲は kernel.ld が __bss_start / __bss_end として教えてくれる。
.Lbss: auipc t0, %pcrel_hi(__bss_start)
    addi t0, t0, %pcrel_lo(.Lbss)     # t0 = 埋め始めの住所
.Lbss_end: auipc t1, %pcrel_hi(__bss_end)
    addi t1, t1, %pcrel_lo(.Lbss_end) # t1 = 埋め終わりの住所 (ここは含まない)
1:  beq t0, t1, 2f          # 始めと終わりが一致したら埋め終わり
    sb zero, 0(t0)          # 1 バイトゼロを書く (zero は常に 0 のレジスタ)
    addi t0, t0, 1          # 住所を 1 進める
    j 1b                    # 繰り返す

# --- 3. C のカーネル本体を呼ぶ ----------------------------------------
2:  call kmain              # kernel.c の kmain へ (戻り先は ra に入る)

# --- 4. 駐車 ------------------------------------------------------------
# kmain は無限ループで戻らない想定。万一戻ってきたら、暴走しない
# ようにここで眠って止まる。
3:  wfi                     # 割り込みが来るまで CPU を眠らせる
    j 3b                    # 目覚めてもまた眠る (無限ループ)

    .section .bss           # 初期値ゼロの領域 (バイナリファイルに実体は入らない)
    .align 4                # 次の住所を 2^4 = 16 バイトの倍数に揃える。
                            # スタック先頭を 16 で割り切れる場所にするため
                            # (詳しくは instructions.md の「メモリの話」)
stack:
    .space 4096             # スタック用に 4KB 確保して
stack_top:                  # その末尾がスタックの出発点

# ユーザーモードからのトラップ専用のスタック。ユーザーモードで
# 実行中に割り込みが来ると、カーネル側のスタックは「呼び出しの
# 途中」で使われている最中なので、割り込み用に別の場所が要る。
# trap_entry_user は sscratch 経由でここの末尾を受け取る。
    .align 4
    .globl trapstack_top
trapstack:
    .space 4096
trapstack_top:

# --- hart1 の入口 (SMP 段1、doc/smp-design.md) -------------------
# SBI HSM の hart_start で起こされると、ここから S モードで
# 走り出す (a0 = hartid、a1 = hart0 が opaque で渡した satp)。
# ページングはまだ無効で、物理アドレスで実行している。像は
# VA == PA で載っているので、satp を入れた後も同じ場所を
# そのまま実行できる。仕事は 4 つだけ:
#   1. satp を入れて hart0 と同じページ表を共有する
#   2. hart1 専用スタックに乗り換える
#   3. tp にハート別状態 (hart1_state) を指させる
#   4. トラップベクタを事故止めに向け、hart1_main を呼ぶ
# .bss のゼロ埋めは hart0 が起動時に済ませているので不要。
    .section .text
    .globl hart1_entry
hart1_entry:
    csrw satp, a1               # ページ表を hart0 と共有
    sfence.vma                  # 変換の効き始めを確定させる
.Lh1sp: auipc sp, %pcrel_hi(hart1_stack_top)
    addi sp, sp, %pcrel_lo(.Lh1sp)
.Lh1tp: auipc tp, %pcrel_hi(hart1_state)
    addi tp, tp, %pcrel_lo(.Lh1tp)
.Lh1tv: auipc t0, %pcrel_hi(hart1_trap)
    addi t0, t0, %pcrel_lo(.Lh1tv)
    csrw stvec, t0              # 事故は hart1_trap へ
    csrci sstatus, 2            # SIE = 0: 割り込みは受けない
    call hart1_main             # a0 = hartid のまま渡る
1:  wfi                         # 戻ってきたら駐車
    j 1b

# hart1 の事故止め: scause をメールボックスに残して駐車する。
# hart0 には触らない (巻き込まない) のが段1 の規律。
hart1_trap:
    csrr t0, scause
.Lh1f: auipc t1, %pcrel_hi(smp_hart1_fault)
    addi t1, t1, %pcrel_lo(.Lh1f)
    sd t0, 0(t1)
1:  wfi
    j 1b

    .section .bss
    .align 4
hart1_stack:
    .space 4096
hart1_stack_top:
