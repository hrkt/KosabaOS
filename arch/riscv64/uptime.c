/* uptime.c -- uptime syscall (31) の検証用ユーザープログラム
 *
 * 起動以来のミリ秒を返す syscall の動作を見るためのテストの
 * 道具 (通常の利用のためのプログラムではない):
 *
 *   run uptime.elf
 *     1. t0 = uptime() を読み、すぐもう一度読んで、値が
 *        減っていないこと (単調) を見る
 *     2. uptime が t0 + 500 以上になるまで待ち、t1 を読む。
 *        待ちの分だけ値が進んでいること
 *     3. プログラムは起動直後に動くので、t0 は小さいはず。
 *        上限 (120 秒) を超えたら壊れた値を掴まされている
 *        とみなす
 *     全部 OK なら "uptime ok" と出してステータス 0 で
 *     終わり、駄目なら "uptime FAIL: 理由" を出して
 *     ステータス 9x で終わる。QEMU の時間は揺れるので、
 *     絶対値の厳密な一致は求めず、単調性と進み幅と
 *     大まかな範囲だけで判定する (test.sh のシナリオ R、
 *     test-k210.py の K12 がこの出力を見る)。
 *
 *     実分解能は 100ms (タイマ割り込みの間隔) なので、
 *     2 回の読みが同じ値になることは普通にある。単調の
 *     判定は「減らないこと」で、増えたことの確認は
 *     500ms の待ちを挟んだ t1 でする。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_UPTIME 31

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

static long uptime_(void)
{
    return syscall3(SYS_UPTIME, 0, 0, 0);
}

static void fail(const char *reason, long status)
{
    puts_("uptime FAIL: ");
    puts_(reason);
    putc_('\n');
    exit_(status);
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    long t0, t0b, t1, i;

    (void)argc;
    (void)argv;

    /* 1. 2 回読んで単調 (減らない) こと */
    t0 = uptime_();
    t0b = uptime_();
    puts_("uptime t0=");
    put_dec_(t0);
    putc_('\n');
    if (t0 < 0) {
        fail("negative", 91);
    }
    if (t0b < t0) {
        fail("went backwards", 92);
    }

    /* 2. 500ms 進むまで待つ。値が固まったままなら回数の
     * 上限で諦める (壊れた syscall で無限ループにしない) */
    t1 = t0;
    for (i = 0; i < 5000000L; i++) {
        t1 = uptime_();
        if (t1 < t0) {
            fail("went backwards", 92);
        }
        if (t1 - t0 >= 500) {
            break;
        }
    }
    if (t1 - t0 < 500) {
        fail("stuck", 93);
    }
    puts_("uptime t1=");
    put_dec_(t1);
    putc_('\n');

    /* 3. 起動直後の値として大まかに小さいこと */
    if (t0 >= 120000) {
        fail("t0 too big", 94);
    }

    puts_("uptime ok\n");
    exit_(0);
}
