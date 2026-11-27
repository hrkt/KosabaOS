# KosabaOS ブート処理 (k210 / M モード)
# =======================================
# kflash -s で SRAM 0x80000000 に置かれた生バイナリへ、ROM の ISP
# スタブが M モードのままジャンプしてくる。OpenSBI のような
# ファームウェアはいないので、virt (S モード) と違いここでやる:
#   1. hart1 を駐車する (K210 はデュアルコアで、両 hart がここへ来る)
#   2. 事故止めのトラップ先 (mtvec) を駐車ループへ向ける
#   3. スタックと .bss を用意して M シム (mshim_boot) を呼ぶ。
#      シムがページングを点火し、S モードの kmain へ mret で
#      降りる (step 4 でこの形に。経緯は mshim.c の冒頭)
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
# (park は hart1 専用になったので、hart0 用は park0)
    la t0, park0
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

# --- 5. M シムを呼ぶ (戻ってこない。S の kmain へ mret される) -------
2:  call mshim_boot

# --- hart0 の駐車 -------------------------------------------------------
park0:
    wfi
    j park0

# --- hart1 の駐車 = 解放ワード待ち (SMP 段1、doc/smp-design.md) --------
# hart1 はここで k210_hart1_release (boards/k210/smp.c) が
# 立つのを回して待つ。割り込みは使わない (起こす合図は
# メモリだけ)。M 用スタックを用意してから待つのは、解放
# 後に C の入口 (k210_hart1_m_entry) を呼ぶため。待って
# いる間はスタックには触らないので、hart0 の .bss ゼロ
# 埋めと競合しない。
park:
    # 切り分けの印 (最優先で書く): ここに着いたこと自体を
    # CLINT mtimecmp1 (0x02004008、キャッシュ無効) へ残す
    li t0, 0x02004008
    li t1, 0xA0
    sd t1, 0(t0)
.Lh1msp: auipc sp, %pcrel_hi(hart1_m_stack_top)
    addi sp, sp, %pcrel_lo(.Lh1msp)
    call k210_hart1_m_entry     # 戻らない (S へ mret)
    j park                      # 万一戻ったら待ち直す

# --- hart1 の S モード入口 ------------------------------------------------
# k210_hart1_m_entry が sptbr と mstatus.VM を点火し、
# mepc = ここ にして mret で降りてくる。仕事は virt の
# hart1_entry と同じ (スタック、tp、事故止め、hart1_main)。
    .section .text
    .globl hart1_entry
hart1_entry:
.Lh1sp: auipc sp, %pcrel_hi(hart1_stack_top)
    addi sp, sp, %pcrel_lo(.Lh1sp)
.Lh1tp: auipc tp, %pcrel_hi(hart1_state)
    addi tp, tp, %pcrel_lo(.Lh1tp)
.Lh1tv: auipc t0, %pcrel_hi(hart1_trap)
    addi t0, t0, %pcrel_lo(.Lh1tv)
    csrw stvec, t0
    csrci sstatus, 2            # SIE = 0: 割り込みは受けない
    li a0, 1                    # hartid = 1 (ここは hart1 専用)
    call hart1_main
1:  wfi
    j 1b

# hart1 の事故止め (S): scause を残して駐車 (hart0 は巻き込まない)
    .globl hart1_trap
hart1_trap:
    csrr t0, scause
.Lh1f: auipc t1, %pcrel_hi(smp_hart1_fault)
    addi t1, t1, %pcrel_lo(.Lh1f)
    sd t0, 0(t1)
1:  wfi
    j 1b

# hart1 の事故止め (M): mcause を残して駐車
    .globl hart1_m_trap
hart1_m_trap:
    csrr t0, mcause
.Lh1mf: auipc t1, %pcrel_hi(smp_hart1_fault)
    addi t1, t1, %pcrel_lo(.Lh1mf)
    sd t0, 0(t1)
1:  wfi
    j 1b

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
    # 16KB (旧 4KB)。4KB では job_run の syscall 連鎖
    # (do_syscall → do_job_run → do_exec → elf_load →
    # fs/fat 読み) が実測で約 4.3KB に達し、床を突き抜けて
    # 隣のメインスタックの最上位フレーム (start_user_named
    # の ra / s0) を踏み潰す事故があった (2026-11-27 計測:
    # trapstack の最低到達点が床に到達 + 約 0xc0 バイト
    # 超過)。Nish のセッションにジョブがあると exit 時の
    # 復帰でカーネルが暴走する形で顕在化した
    .space 16384
trapstack_top:

# hart1 用スタック (SMP 段1)。M 用は解放後の C 入口のため、
# S 用は hart1_main のため
    .align 4
hart1_m_stack:
    .space 1024
hart1_m_stack_top:
    .align 4
hart1_stack:
    .space 4096
hart1_stack_top:
