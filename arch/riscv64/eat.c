/* eat.c -- ページプール枯渇の検証用ユーザープログラム
 *
 * 大きな .bss (約 1000KB) を持つので、exec されるたびに
 * データ領域のページを約 250 枚食べる (ELF ローダは
 * セグメントが触るページを全部プールから取るため)。
 * 自分自身を exec して段を重ねていくと、K210 の小さい
 * プール (1280 枚) では数段で枯渇し、次の段のページが
 * 取れなくなって exec が -1 になる。そのときカーネル
 * ログに枯渇の行 ("out of pages" + "pool free 0"。
 * 枯渇する箇所はページ数の端数で ELF のセグメント
 * 確保かスタック準備か変わる) が残ることを確かめるのが
 * 目的 (test-k210.py の K11)。virt のプール (24064 枚)
 * では 12 段くらいでは枯渇しない。
 *
 *   run eat.elf 12
 *     底 (0) まで届けば "eat bottom" と出てステータス 0。
 *     途中で枯渇した段は "eat: exec failed" と出して
 *     ステータス 33 で終わり、上の段はそれをそのまま
 *     伝える (深さ上限の 16 段より先に枯渇する想定)。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_EXEC 8

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

/* 1 桁または 2 桁の10進文字列を数に直す (テスト用なので雑でよい) */
static int atoi_(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/* 数を文字列にする ("eat.elf 11" のような exec 用引数を作る) */
static void itoa_(int v, char *buf)
{
    char tmp[12];
    int n = 0, i = 0;

    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        buf[i++] = tmp[--n];
    }
    buf[i] = '\0';
}

/* ページを食べる本体: 約 1000KB の .bss (250 ページ分)。
 * 先頭と末尾を実際に触って、自分のページが生きている
 * ことも確かめる */
static char big[1000 * 1024];

static void run_eat(int n)
{
    big[0] = (char)n;
    big[sizeof(big) - 1] = (char)n;

    if (n == 0) {
        puts_("eat bottom\n");
        exit_(0);
    }
    {
        char num[12];
        char args[24];
        int i = 0;
        const char *p = "eat.elf ";
        long r;

        while (*p != '\0') {
            args[i++] = *p++;
        }
        itoa_(n - 1, num);
        {
            int j = 0;
            while (num[j] != '\0') {
                args[i++] = num[j++];
            }
        }
        args[i] = '\0';

        r = syscall3(SYS_EXEC, (long)"eat.elf", (long)args, 0);
        if (r < 0) {
            /* プール枯渇で子のページが取れなかった段 */
            puts_("eat: exec failed\n");
            exit_(33);
        }
        /* 子が終わったあとも自分のページが無事なこと */
        if (big[0] != (char)n || big[sizeof(big) - 1] != (char)n) {
            puts_("eat: memory broken\n");
            exit_(34);
        }
        exit_(r);  /* ステータスはそのまま上へ伝える */
    }
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    run_eat(argc >= 2 ? atoi_(argv[1]) : 0);
    exit_(94);  /* ここへは来ないはず */
}
