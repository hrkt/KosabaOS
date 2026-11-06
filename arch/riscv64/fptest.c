/* fptest.c -- ユーザープログラムで浮動小数点が使えることの検証用
 *
 * build.sh で -mabi=lp64d としてビルドされる (他のユーザー
 * プログラムは lp64)。プログラム開始時にカーネルが
 * sstatus.FS を Initial にしているので、FP 命令がそのまま
 * 動くはず、ということを確認するのが目的。
 *
 * 入力は volatile な大域変数から読み、コンパイル時の定数
 * 畳み込みで演算自体が消えないようにする。値は全て 2 進で
 * 正確に表せるものだけを使い、等号で照合する:
 *   float : 1.5 * 2.25 + 0.625 = 4.0、7.0 / 2.0 = 3.5
 *   double: (10.0 / 4.0) * 3.0 - 0.5 = 7.0、2.5 * 4.0 = 10.0
 *   変換  : (long)(7.0 * 2.0) = 14、(int)(3.5f * 2.0f) = 7
 * 段階ごとに "fptest: ... ok" を出し、全部済んだら
 * "fptest: done" を出してステータス 0。期待と違えば
 * "fptest: FAIL" を出してステータス 1 で終わる。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3

static long syscall3(long num, long a0, long a1, long a2)
{
    register long r0 asm("a0") = a0;
    register long r1 asm("a1") = a1;
    register long r2 asm("a2") = a2;
    register long r7 asm("a7") = num;
    asm volatile("ecall"
                 : "+r"(r0)
                 : "r"(r1), "r"(r2), "r"(r7)
                 : "memory");
    return r0;
}

static void putc_(char c) { syscall3(SYS_PUTC, c, 0, 0); }

static void puts_(const char *s)
{
    while (*s != '\0') {
        putc_(*s);
        s++;
    }
}

static void exit_(long status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

static void fail(const char *what)
{
    puts_("fptest: FAIL ");
    puts_(what);
    putc_('\n');
    exit_(1);
}

/* volatile: 実行時に本物の FP 命令で計算させるための入力 */
static volatile float fa = 1.5f, fb = 2.25f, fc = 0.625f;
static volatile float f7 = 7.0f, f2 = 2.0f;
static volatile double d10 = 10.0, d4 = 4.0, d3 = 3.0;
static volatile double d05 = 0.5, d25 = 2.5;

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    float fr;
    double dr;
    long lv;
    int iv;
    (void)argc;
    (void)argv;

    /* float の四則 (乗算・加算・除算) */
    fr = fa * fb + fc;
    if (fr != 4.0f) {
        fail("float-mul-add");
    }
    fr = f7 / f2;
    if (fr != 3.5f) {
        fail("float-div");
    }
    puts_("fptest: float ok\n");

    /* double の四則 (除算・乗算・減算) */
    dr = (d10 / d4) * d3 - d05;
    if (dr != 7.0) {
        fail("double-arith");
    }
    dr = d25 * d4;
    if (dr != 10.0) {
        fail("double-mul");
    }
    puts_("fptest: double ok\n");

    /* 整数への変換 (fcvt) も命令として動くこと。
     * 変換元は実行時の計算結果 (リテラル同士だと定数
     * 畳み込みで消えうる) */
    lv = (long)(((d10 / d4) * d3 - d05) * 2.0);
    if (lv != 14) {
        fail("conv-double-long");
    }
    iv = (int)((f7 / f2) * 2.0f);
    if (iv != 7) {
        fail("conv-float-int");
    }
    puts_("fptest: conv ok\n");

    puts_("fptest: done\n");
    exit_(0);
}
