/* keywait.c -- getc タイムアウト (a1) の検証用プログラム
 *
 *   run keywait.elf [回数]
 *     400ms のタイムアウト付き getc を回数分くり返し、
 *     タイムアウトするたびに "keywait timeout i/N"、
 *     文字が来たら "keywait got <c>" と出して終わる。
 *     全部タイムアウトしたら "keywait done"。
 *
 * top.elf が「1 秒ごとに描き直し、q で終わる」ために
 * 必要な待ち方 (test.sh シナリオ AN、doc/top-design.md)。
 */

#define SYS_PUTC 1
#define SYS_GETC 2
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
    long count = argc >= 2 ? atoi_(argv[1]) : 3;
    long i;

    if (count < 1) {
        count = 1;
    }
    for (i = 1; i <= count; i++) {
        long c = syscall3(SYS_GETC, 0, 400, 0);
        if (c < 0) {
            puts_("keywait timeout ");
            put_dec_(i);
            putc_('/');
            put_dec_(count);
            putc_('\n');
        } else {
            puts_("keywait got ");
            putc_((char)c);
            putc_('\n');
            exit_(0);
        }
    }
    puts_("keywait done\n");
    exit_(0);
}
