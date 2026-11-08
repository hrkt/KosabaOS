/* gjob.c -- 画面表示の窓 (doc/gfx-design.md 段B) の検証用
 * ユーザープログラム
 *
 * slow.elf の描画版。約 400ms ごとに、自分の窓へ伸びる
 * バーとカウンタを描き、コンソールにも "gjob i/N" と出す。
 * バックグラウンドで走らせると、自分の窓 (ジョブのグリッド
 * のマス) にだけ絵が育ち、前面の窓には触らないはず
 * (test.sh シナリオ Z、test-k210.py K18)。
 *
 *   run gjob.elf [回数] [終了ステータス]
 *     回数の既定は 8、ステータスの既定は 0。slow.elf と
 *     同じ引数の流儀。
 *
 * 待ちは uptime (31) のポーリング (この OS の流儀)。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_UPTIME 31
#define SYS_GFX_CLEAR 44
#define SYS_GFX_RECT 45
#define SYS_GFX_TEXT 46

#define COL(r, g, b) \
    ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

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

static void wait_ms(long ms)
{
    long t0 = syscall3(SYS_UPTIME, 0, 0, 0);
    long i;
    for (i = 0; i < 20000000L; i++) {
        if (syscall3(SYS_UPTIME, 0, 0, 0) - t0 >= ms) {
            return;
        }
    }
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    long count = argc >= 2 ? atoi_(argv[1]) : 8;
    long status = argc >= 3 ? atoi_(argv[2]) : 0;
    long i;

    if (count < 1) {
        count = 1;
    }
    syscall3(SYS_GFX_CLEAR, COL(0, 64, 0), 0, 0);
    for (i = 1; i <= count; i++) {
        char label[12];
        int n = 0;
        const char *p = "GJOB ";

        while (*p != '\0') {
            label[n++] = *p++;
        }
        if (i >= 10) {
            label[n++] = (char)('0' + (i / 10) % 10);
        }
        label[n++] = (char)('0' + i % 10);
        label[n] = '\0';

        /* バーは回数に応じて伸びる (幅は窓でクリップされる) */
        syscall3(SYS_GFX_RECT, 4 | (24 << 16),
                 (i * 14) | (14 << 16), COL(0, 255, 255));
        syscall3(SYS_GFX_TEXT, 4 | (4 << 16), (long)label,
                 COL(255, 255, 255) | (2 << 16));

        puts_("gjob ");
        put_dec_(i);
        putc_('/');
        put_dec_(count);
        putc_('\n');
        wait_ms(400);
    }
    puts_("gjob done\n");
    exit_(status);
}
