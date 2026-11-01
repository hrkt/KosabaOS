# trap.S -- トラップ (例外・割り込み) の入口 (riscv64)
# ======================================================
# 例外 (ページフォルトなど) や割り込み (タイマなど) が起きると、
# CPU は stvec レジスタの指す住所へ強制的にジャンプしてくる。
# その飛び先がこの trap_entry。仕事は 3 つ:
#   1. 使っていたレジスタを全部スタックに退避する
#   2. C の trap_handler を呼ぶ
#   3. レジスタを全部元に戻して sret で元の処理に帰る
#
# 割り込まれた側 (echo のループなど) から見れば「何もなかった
# かのように」続きが動く必要があるので、退避は 1 つ残らず行う。
# x0 (zero) は常に 0 で退避不要。sp 自身は「今のスタックの続き」を
# 使う形なので退避しない (カーネルは S モードのままなので、
# 割り込まれてもスタックは同じものを使える)。
#
# なお、トラップが起きた瞬間に CPU が sstatus.SIE を 0 にするので、
# ハンドラの中に次の割り込みが割り込むことはない (多重割り込みなし)。

    .section .text
    .globl trap_entry
    .align 4                # stvec の住所は 4 バイト境界が必須
trap_entry:
    addi sp, sp, -256       # 退避用に 256 バイト確保 (31 個 × 8 + 余り。
                            # 16 の倍数にして sp の整列を保つ)
    sd ra, 0(sp)
    sd gp, 8(sp)
    sd tp, 16(sp)
    sd t0, 24(sp)
    sd t1, 32(sp)
    sd t2, 40(sp)
    sd s0, 48(sp)
    sd s1, 56(sp)
    sd a0, 64(sp)
    sd a1, 72(sp)
    sd a2, 80(sp)
    sd a3, 88(sp)
    sd a4, 96(sp)
    sd a5, 104(sp)
    sd a6, 112(sp)
    sd a7, 120(sp)
    sd s2, 128(sp)
    sd s3, 136(sp)
    sd s4, 144(sp)
    sd s5, 152(sp)
    sd s6, 160(sp)
    sd s7, 168(sp)
    sd s8, 176(sp)
    sd s9, 184(sp)
    sd s10, 192(sp)
    sd s11, 200(sp)
    sd t3, 208(sp)
    sd t4, 216(sp)
    sd t5, 224(sp)
    sd t6, 232(sp)

    mv a0, sp               # 引数 1: 退避したレジスタ一式の先頭
    li a1, 0                # 引数 2: カーネルからのトラップ (0)
    call trap_handler       # C 側の処理へ (trap.c)

    ld ra, 0(sp)
    ld gp, 8(sp)
    ld tp, 16(sp)
    ld t0, 24(sp)
    ld t1, 32(sp)
    ld t2, 40(sp)
    ld s0, 48(sp)
    ld s1, 56(sp)
    ld a0, 64(sp)
    ld a1, 72(sp)
    ld a2, 80(sp)
    ld a3, 88(sp)
    ld a4, 96(sp)
    ld a5, 104(sp)
    ld a6, 112(sp)
    ld a7, 120(sp)
    ld s2, 128(sp)
    ld s3, 136(sp)
    ld s4, 144(sp)
    ld s5, 152(sp)
    ld s6, 160(sp)
    ld s7, 168(sp)
    ld s8, 176(sp)
    ld s9, 184(sp)
    ld s10, 192(sp)
    ld s11, 200(sp)
    ld t3, 208(sp)
    ld t4, 216(sp)
    ld t5, 224(sp)
    ld t6, 232(sp)
    addi sp, sp, 256        # 退避用領域を返す

    sret                    # 割り込まれた場所へ帰る (sepc の住所へ。
                            # 割り込み許可も元の状態に戻る)

# ======================================================================
# ユーザーモードからのトラップの入口
# ======================================================================
# ユーザーモードで動いているときにトラップが来ると、sp は
# ユーザー側のスタックを指したままなので、そのままでは
# カーネルの退避場所にできない。そこで最初に sscratch と sp を
# 交換する: sscratch にはユーザーモードへ入るときに
# 「トラップ専用スタック (trapstack) の末尾」を入れてある
# (user.S の user_launch)。交換すると sp がカーネル側になり、
# sscratch にはユーザーの sp が残る。帰りは逆の交換で戻す。
# それ以外 (退避・復元・ハンドラ呼び出し) はカーネル側と共通で、
# ハンドラへは「ユーザーモードから (1)」を伝える。

    .globl trap_entry_user
    .align 4
trap_entry_user:
    csrrw sp, sscratch, sp  # sp ← トラップ専用スタック、sscratch ← ユーザーの sp
    addi sp, sp, -256
    sd ra, 0(sp)
    sd gp, 8(sp)
    sd tp, 16(sp)
    sd t0, 24(sp)
    sd t1, 32(sp)
    sd t2, 40(sp)
    sd s0, 48(sp)
    sd s1, 56(sp)
    sd a0, 64(sp)
    sd a1, 72(sp)
    sd a2, 80(sp)
    sd a3, 88(sp)
    sd a4, 96(sp)
    sd a5, 104(sp)
    sd a6, 112(sp)
    sd a7, 120(sp)
    sd s2, 128(sp)
    sd s3, 136(sp)
    sd s4, 144(sp)
    sd s5, 152(sp)
    sd s6, 160(sp)
    sd s7, 168(sp)
    sd s8, 176(sp)
    sd s9, 184(sp)
    sd s10, 192(sp)
    sd s11, 200(sp)
    sd t3, 208(sp)
    sd t4, 216(sp)
    sd t5, 224(sp)
    sd t6, 232(sp)

    mv a0, sp               # 引数 1: 退避したレジスタ一式の先頭
    li a1, 1                # 引数 2: ユーザーモードからのトラップ (1)
    call trap_handler

    ld ra, 0(sp)
    ld gp, 8(sp)
    ld tp, 16(sp)
    ld t0, 24(sp)
    ld t1, 32(sp)
    ld t2, 40(sp)
    ld s0, 48(sp)
    ld s1, 56(sp)
    ld a0, 64(sp)
    ld a1, 72(sp)
    ld a2, 80(sp)
    ld a3, 88(sp)
    ld a4, 96(sp)
    ld a5, 104(sp)
    ld a6, 112(sp)
    ld a7, 120(sp)
    ld s2, 128(sp)
    ld s3, 136(sp)
    ld s4, 144(sp)
    ld s5, 152(sp)
    ld s6, 160(sp)
    ld s7, 168(sp)
    ld s8, 176(sp)
    ld s9, 184(sp)
    ld s10, 192(sp)
    ld s11, 200(sp)
    ld t3, 208(sp)
    ld t4, 216(sp)
    ld t5, 224(sp)
    ld t6, 232(sp)
    addi sp, sp, 256

    csrrw sp, sscratch, sp  # sp ← ユーザーのスタックに戻す
    sret
