/* oled.c -- SSD1315 OLED に文字列を出すユーザープログラム
 * (doc/oled-design.md、syscall 48 / 49)
 *
 *   run oled <単語> [単語...]   引数を空白でつないで表示
 *   run oled clear              消去する
 *
 * 折り返しと大文字化はカーネル側 (ssd1315.c) が行う。
 * 単語 "|" は改行の合図 (ima と同じ)。表示できたら
 * "oled: done" と出してステータス 0、OLED が無い環境では
 * "oled: no display" と出してステータス 3、引数が無ければ
 * 使い方を出してステータス 2 で終わる
 * (test.sh のシナリオ AI、test-k210.py の K27 が見る)。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_OLED_TEXT 48
#define SYS_OLED_CLEAR 49

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

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void _start(int argc, char **argv)
{
    char buf[176];
    int n = 0;
    int i, k;

    if (argc < 2) {
        puts_("usage: run oled <words...> | clear\n");
        exit_(2);
    }

    if (argc == 2 && str_eq(argv[1], "clear")) {
        if (syscall3(SYS_OLED_CLEAR, 0, 0, 0) != 0) {
            puts_("oled: no display\n");
            exit_(3);
        }
        puts_("oled: cleared\n");
        exit_(0);
    }

    /* 引数を空白 1 つでつなぐ (入りきらない分は切る) */
    for (i = 1; i < argc; i++) {
        if (i > 1 && n < (int)sizeof(buf) - 1)
            buf[n++] = ' ';
        for (k = 0; argv[i][k] && n < (int)sizeof(buf) - 1; k++)
            buf[n++] = argv[i][k];
    }
    buf[n] = '\0';

    if (syscall3(SYS_OLED_TEXT, (long)buf, 0, 0) != 0) {
        puts_("oled: no display\n");
        exit_(3);
    }
    puts_("oled: done\n");
    exit_(0);
}
