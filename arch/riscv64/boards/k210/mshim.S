# mshim.S -- M モードシムのトラップ入口 (k210)
# ======================================================
# S モード (場合により U モード) で割り込み・例外が起きて M に
# 落ちてきたときの入口。仕事は 3 つ:
#   1. mscratch と sp を交換して M 専用スタックに切り替える
#      (割り込まれた側の sp はユーザーの仮想アドレスかもしれず、
#       M では物理でしか触れないため、そのまま使えない)
#   2. レジスタを全部退避して C の mshim_handler を呼ぶ
#   3. レジスタを戻し、sp を交換し直して mret で帰る
# 退避の並びは mtrap.S (step 3) と同じ (a0 はオフセット 64、
# a7 は 120)。mscratch には起動時に M スタックの末尾を入れて
# ある (mshim.c の mshim_boot)。

    .section .text
    .globl mshim_entry
    .align 4                # mtvec の住所は 4 バイト境界が必須
mshim_entry:
    csrrw sp, mscratch, sp  # sp = M スタック末尾、mscratch = 元の sp
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

    mv a0, sp               # ハンドラへ trapframe を渡す
    call mshim_handler      # C 側の処理へ (mshim.c)

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

    csrrw sp, mscratch, sp  # sp = 元の sp、mscratch = M スタック末尾
    mret                    # 割り込まれた場所へ帰る (mepc の住所へ)
