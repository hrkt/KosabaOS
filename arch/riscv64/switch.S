# switch.S -- コンテキストスイッチ (riscv64)
# ======================================================
# switch_context(old, new): 実行の文脈を切り替える。
#
# 文脈として保存するのは「呼び出し規約で、呼ばれた側が
# 保存することになっているもの」だけ: 戻りアドレス ra、
# スタックポインタ sp、s0〜s11。関数呼び出しの約束では
# それ以外 (t や a レジスタ) は呼び出し側が「壊れてよい」前提で
# 扱うので、切り替え先から戻ってきたときに必要になるのは
# この組だけになる (proc.h の struct context と並びを一致させる)。
#
# 動きは単純: old に今の文脈を書き出し、new の文脈を読み込み、
# ret する。読み込んだ ra が切り替え先の「戻り先」なので、
# ret 一発で相手の続きに飛ぶ。初めて動くタスクについては、
# proc.c が ra に入口 (トランポリン) を仕込んでおく。

    .section .text
    .globl switch_context
switch_context:
    sd ra, 0(a0)
    sd sp, 8(a0)
    sd s0, 16(a0)
    sd s1, 24(a0)
    sd s2, 32(a0)
    sd s3, 40(a0)
    sd s4, 48(a0)
    sd s5, 56(a0)
    sd s6, 64(a0)
    sd s7, 72(a0)
    sd s8, 80(a0)
    sd s9, 88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)

    ld ra, 0(a1)
    ld sp, 8(a1)
    ld s0, 16(a1)
    ld s1, 24(a1)
    ld s2, 32(a1)
    ld s3, 40(a1)
    ld s4, 48(a1)
    ld s5, 56(a1)
    ld s6, 64(a1)
    ld s7, 72(a1)
    ld s8, 80(a1)
    ld s9, 88(a1)
    ld s10, 96(a1)
    ld s11, 104(a1)
    ret
