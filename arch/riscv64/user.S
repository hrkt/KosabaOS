# user.S -- ユーザーモードへの出入り (riscv64)
# ======================================================
# user_launch(entry, user_stack_top)
#   カーネルの文脈 (戻り先やレジスタ) を kernel_context に保存して
#   から、ユーザーモードへ sret で降りる。ユーザープログラムが
#   exit システムコールを呼ぶと、user_exit_jump が保存した文脈を
#   復元して、user_launch の呼び出し元に「戻って」くる。
#   いわば行って戻るための往復切符。
#
# ユーザーモードへ降りる前の設定:
#   sscratch = トラップ専用スタックの末尾 (trap.S の入口で使う)
#   stvec    = trap_entry_user (ユーザーからのトラップ専用入口)
#   sstatus  : SPP = 0 (降り先は U モード)、SPIE = 1
#              (sret 後に割り込み許可になる)
#   sepc     = ユーザープログラムの入口

    .section .bss
    .align 4
kernel_context:
    .space 8 * 14           # ra, sp, s0〜s11 の保存場所

    .section .text
    .globl user_launch
user_launch:
    # カーネルの文脈を保存する
.Lctx: auipc t0, %pcrel_hi(kernel_context)
    addi t0, t0, %pcrel_lo(.Lctx)
    sd ra, 0(t0)
    sd sp, 8(t0)
    sd s0, 16(t0)
    sd s1, 24(t0)
    sd s2, 32(t0)
    sd s3, 40(t0)
    sd s4, 48(t0)
    sd s5, 56(t0)
    sd s6, 64(t0)
    sd s7, 72(t0)
    sd s8, 80(t0)
    sd s9, 88(t0)
    sd s10, 96(t0)
    sd s11, 104(t0)

    # sscratch にトラップ専用スタックの末尾を入れる
.Lts: auipc t1, %pcrel_hi(trapstack_top)
    addi t1, t1, %pcrel_lo(.Lts)
    csrw sscratch, t1

    # トラップの飛び先をユーザー専用の入口に切り替える
.Lvec: auipc t1, %pcrel_hi(trap_entry_user)
    addi t1, t1, %pcrel_lo(.Lvec)
    csrw stvec, t1

    # sstatus: SPP (bit8) を 0 = U モード、SPIE (bit5) を 1
    li t1, 0x100
    csrc sstatus, t1
    li t1, 0x20
    csrs sstatus, t1

    csrw sepc, a0           # 降り先はプログラムの入口
    mv sp, a1               # スタックはユーザー側のものに
    sret                    # ユーザーモードへ

    .globl user_exit_jump
user_exit_jump:
    # exit やユーザーのフォルトから、保存したカーネルの文脈へ戻る
.Lctx2: auipc t0, %pcrel_hi(kernel_context)
    addi t0, t0, %pcrel_lo(.Lctx2)
    ld ra, 0(t0)
    ld sp, 8(t0)
    ld s0, 16(t0)
    ld s1, 24(t0)
    ld s2, 32(t0)
    ld s3, 40(t0)
    ld s4, 48(t0)
    ld s5, 56(t0)
    ld s6, 64(t0)
    ld s7, 72(t0)
    ld s8, 80(t0)
    ld s9, 88(t0)
    ld s10, 96(t0)
    ld s11, 104(t0)

    # トラップの飛び先をカーネル側の入口に戻す
.Lvec2: auipc t1, %pcrel_hi(trap_entry)
    addi t1, t1, %pcrel_lo(.Lvec2)
    csrw stvec, t1

    ret                     # user_launch の呼び出し元へ
