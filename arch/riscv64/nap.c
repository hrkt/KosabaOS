/* nap.c -- sleep_ms (34) の検証用ユーザープログラム
 *
 *   run nap.elf [回数]
 *     1 回 500ms を sleep_ms で寝て、起きるたびに
 *     "nap i/N" と出す。最後に開始からの経過 (uptime の
 *     差) を "nap: elapsed <ms>" と出して終わる。
 *
 * ポーリングで待つ slow.elf と違い、こちらは syscall で
 * 寝るので CPU 積算がほぼ増えないはず (test.sh シナリオ
 * AN、doc/top-design.md 段2)。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_UPTIME 31
#define SYS_SLEEP_MS 34

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

static int atoi_(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static void exit_(long status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    long count = argc >= 2 ? atoi_(argv[1]) : 2;
    long t0;
    long i;

    if (count < 1) {
        count = 1;
    }
    t0 = syscall3(SYS_UPTIME, 0, 0, 0);
    for (i = 1; i <= count; i++) {
        syscall3(SYS_SLEEP_MS, 500, 0, 0);
        puts_("nap ");
        put_dec_(i);
        putc_('/');
        put_dec_(count);
        putc_('\n');
    }
    puts_("nap: elapsed ");
    put_dec_(syscall3(SYS_UPTIME, 0, 0, 0) - t0);
    putc_('\n');
    puts_("nap done\n");
    exit_(0);
}
