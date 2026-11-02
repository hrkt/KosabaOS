# mtrap.S -- M モードのトラップ入口 (k210)
# ======================================================
# S モード版 (trap.S) の M 版。タイマ割り込みなどが起きると CPU は
# mtvec の指す住所へ飛んでくる。仕事は S 版と同じ 3 つ:
#   1. レジスタを全部スタックに退避する
#   2. C の mtrap_handler を呼ぶ
#   3. レジスタを全部戻して mret で帰る
# 違いは帰り方 (mret) と、飛び先レジスタ (mtvec) だけ。
# カーネル自身が M モードで動いているので、割り込まれても
# スタックは同じものを使える (sscratch 交換のような細工は不要)。
#
# トラップが起きた瞬間に CPU が mstatus.MIE を 0 にするので、
# ハンドラの中に次の割り込みが割り込むことはない。

    .section .text
    .globl mtrap_entry
    .align 4                # mtvec の住所は 4 バイト境界が必須
mtrap_entry:
    addi sp, sp, -256       # 退避用に 256 バイト確保
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

    call mtrap_handler      # C 側の処理へ (mtrap.c)

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

    mret                    # 割り込まれた場所へ帰る (mepc の住所へ)
