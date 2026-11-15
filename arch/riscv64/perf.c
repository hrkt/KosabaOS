/* perf.c -- perf_read syscall (33) の検証用ユーザープログラム
 *
 * ハードウェアの性能カウンタ (サイクル数 / 実行命令数) を
 * 読む syscall の動作を見るための道具で、ベンチの物差し
 * としてもそのまま使える:
 *
 *   run perf.elf
 *     1. ループ (100 万回) の前後でカウンタを読み、差を
 *        "perf cycles=<差> instret=<差>" と出す
 *     2. サイクルの差が正、命令数の差がループ回数以上
 *        (1 回の反復で最低 1 命令は実行を終える) であること
 *     3. もう一度読んでも値が減らないこと (単調)
 *     4. 不正な種類 (99) には -1 が返ること
 *     全部 OK なら "perf ok" と出してステータス 0 で終わり、
 *     駄目なら "perf FAIL: 理由" を出してステータス 9x で
 *     終わる (test.sh のシナリオ AE、test-k210.py の K24 が
 *     この出力を見る)。
 *
 *     カウンタはマシン全体の通算 (他のタスクやカーネルの
 *     分も入る) なので、絶対値の一致は求めず、進み方と
 *     大まかな下限だけで判定する (uptime.c と同じ考え方)。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_PERF_READ 33

#define PERF_CYCLES 0
#define PERF_INSTRET 1

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

static void put_dec_(long v)
{
    char buf[20];
    int n = 0;

    if (v == 0) {
        putc_('0');
        return;
    }
    while (v > 0) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        putc_(buf[--n]);
    }
}

static void exit_(long status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

static unsigned long perf_(long kind)
{
    return (unsigned long)syscall3(SYS_PERF_READ, kind, 0, 0);
}

static void fail(const char *reason, long status)
{
    puts_("perf FAIL: ");
    puts_(reason);
    putc_('\n');
    exit_(status);
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    unsigned long c0, c1, c2, i0, i1, dc, di;
    long sum, k;

    (void)argc;
    (void)argv;

    /* 1. ループの前後で読む */
    c0 = perf_(PERF_CYCLES);
    i0 = perf_(PERF_INSTRET);
    sum = 0;
    for (k = 0; k < 1000000L; k++) {
        sum += k;
    }
    c1 = perf_(PERF_CYCLES);
    i1 = perf_(PERF_INSTRET);
    if (sum != 499999500000L) {
        fail("loop broken", 91);
    }
    dc = c1 - c0;
    di = i1 - i0;
    puts_("perf cycles=");
    put_dec_((long)dc);
    puts_(" instret=");
    put_dec_((long)di);
    putc_('\n');

    /* 2. 進んでいること。命令数はループ回数が下限 */
    if (dc == 0) {
        fail("cycles stuck", 92);
    }
    if (di < 1000000UL) {
        fail("instret too small", 93);
    }

    /* 3. 単調 (減らない) こと */
    c2 = perf_(PERF_CYCLES);
    if (c2 < c1) {
        fail("went backwards", 94);
    }

    /* 4. 不正な種類は -1 */
    if (perf_(99) != (unsigned long)-1) {
        fail("bad kind not -1", 95);
    }

    puts_("perf ok\n");
    exit_(0);
}
