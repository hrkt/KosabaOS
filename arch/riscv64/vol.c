/* vol.c -- 再生音量 (カーネルの音量シフト) を見たり
 * 変えたりするユーザープログラム
 * (doc/audio-driver-design.md、syscall 50)
 *
 *   run vol.elf        現在のシフトを出す
 *   run vol.elf N      シフトを N (0〜5) に変える
 *   run vol.elf mute   ミュートにする (N = 5 と同じ)
 *
 * シフト n は振幅 1/2^n (0 = 等倍、1 = 半分、2 = 1/4、
 * 3 = 1/8、4 = 1/16)。5 はミュート (全サンプル 0) で、
 * 音を出さずに実機計測するためのもの (経路と帳簿は
 * 通常どおり動く)。値はカーネル側に残るので、次に
 * 鳴らすプログラムにもそのまま効く。引数が不正なら
 * 使い方を出してステータス 2 で終わる
 * (test.sh のシナリオ AQ が見る)。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_AUDIO_SET_VOLUME 50

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

/* 0 以上の小さな数を出す (シフト値と分母専用) */
static void put_num(long v)
{
    char buf[12];
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

static void usage(void)
{
    puts_("usage: vol [0-5]\n");
    exit_(2);
}

static int is_mute_word(const char *p)
{
    return p[0] == 'm' && p[1] == 'u' && p[2] == 't' &&
           p[3] == 'e' && p[4] == '\0';
}

void _start(int argc, char **argv)
{
    long cur;
    if (argc == 1) {
        /* 引数なし: 問い合わせ (a0 = -1) */
        cur = syscall3(SYS_AUDIO_SET_VOLUME, -1, 0, 0);
    } else if (argc == 2) {
        /* 10 進 1 つ (0〜5) か、mute */
        const char *p = argv[1];
        long v = 0;
        if (is_mute_word(p)) {
            cur = syscall3(SYS_AUDIO_SET_VOLUME, 5, 0, 0);
            puts_("vol: shift=5 (mute)\n");
            exit_(0);
        }
        if (*p == '\0') {
            usage();
        }
        while (*p != '\0') {
            if (*p < '0' || *p > '9') {
                usage();
            }
            v = v * 10 + (*p - '0');
            p++;
        }
        if (v > 5) {
            usage();
        }
        cur = syscall3(SYS_AUDIO_SET_VOLUME, v, 0, 0);
    } else {
        usage();
        cur = 0;  /* usage() は戻らない */
    }
    if (cur == 5) {
        puts_("vol: shift=5 (mute)\n");
    } else {
        puts_("vol: shift=");
        put_num(cur);
        puts_(" (1/");
        put_num(1L << cur);
        puts_(")\n");
    }
    exit_(0);
}
