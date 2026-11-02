# KosabaOS ブート処理 (k210 / M モード)
# =======================================
# kflash -s で SRAM 0x80000000 に置かれた生バイナリへ、ROM の ISP
# スタブが M モードのままジャンプしてくる。OpenSBI のような
# ファームウェアはいないので、virt (S モード) と違いここでやる:
#   1. hart1 を駐車する (K210 はデュアルコアで、両 hart がここへ来る)
#   2. 事故止めのトラップ先 (mtvec) を駐車ループへ向ける
#   3. スタックと .bss を用意して kmain を呼ぶ
# スタックと .bss の用意の仕方は boards/virt/start.S と同じ。
#
# mhartid は「自分が何番の hart か」を読む CSR (M モード専用)。
# 命令の意味を忘れたら instructions.md の早見表を見よ。

    .section .text.start    # リンカが真っ先に置く領域 (kernel.ld)
    .globl _start
_start:
# --- 1. hart の振り分け ----------------------------------------------
    csrr t0, mhartid        # t0 = 自分の hart 番号
    bnez t0, park           # hart0 以外は駐車 (hart1 はここで眠る)

# --- 2. 事故止めのトラップ先 -----------------------------------------
# まだトラップ処理は無い。何か起きたら暴走せず駐車ループへ落とす。
    la t0, park
    csrw mtvec, t0

# --- 3. スタックを用意する (virt と同じ pcrel 方式) -------------------
.Lsp: auipc sp, %pcrel_hi(stack_top)
    addi sp, sp, %pcrel_lo(.Lsp)

# --- 4. .bss をゼロで埋める (virt と同じ) ------------------------------
.Lbss: auipc t0, %pcrel_hi(__bss_start)
    addi t0, t0, %pcrel_lo(.Lbss)
.Lbss_end: auipc t1, %pcrel_hi(__bss_end)
    addi t1, t1, %pcrel_lo(.Lbss_end)
1:  beq t0, t1, 2f
    sb zero, 0(t0)
    addi t0, t0, 1
    j 1b

# --- 5. C のカーネル本体を呼ぶ ----------------------------------------
2:  call kmain

# --- 駐車 ---------------------------------------------------------------
park:
    wfi
    j park

    .section .bss
    .align 4
stack:
    .space 4096
stack_top:

# ユーザーモード用トラップスタック (virt と同じ置き方)。
# K210 ではまだ使わないが、.bss の形をボード間で揃えておく。
    .align 4
    .globl trapstack_top
trapstack:
    .space 4096
trapstack_top:
