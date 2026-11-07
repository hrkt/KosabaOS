/* gdemo.c -- 画面表示 (描画 syscall 43〜47) の検証用
 * ユーザープログラム (doc/gfx-design.md 段A)
 *
 * 自分の窓の情報を取り、消去 → 矩形 2 つ → 文字列 → 直線
 * の順に描いて、コンソールにも経過を出す。virt では実描画
 * が無いので、検証は klog の要約行 (Nish の dmesg) と
 * ここが出す行で見る (test.sh シナリオ Y)。実機では窓に
 * 絵が出る。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_GFX_INFO 43
#define SYS_GFX_CLEAR 44
#define SYS_GFX_RECT 45
#define SYS_GFX_TEXT 46
#define SYS_GFX_LINE 47

/* RGB565 の色 (lcd.h の LCD_RGB と同じ計算) */
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

static void exit_(long status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    unsigned int info[4];
    long rc;

    (void)argc;
    (void)argv;

    rc = syscall3(SYS_GFX_INFO, (long)info, 0, 0);
    if (rc != 0) {
        puts_("gdemo: info failed\n");
        exit_(1);
    }
    puts_("gdemo: screen ");
    put_dec_((long)info[0]);
    putc_('x');
    put_dec_((long)info[1]);
    puts_(" win ");
    put_dec_((long)info[2]);
    putc_('x');
    put_dec_((long)info[3]);
    putc_('\n');

    syscall3(SYS_GFX_CLEAR, COL(0, 0, 128), 0, 0);
    syscall3(SYS_GFX_RECT, 10 | (10 << 16), 120 | (60 << 16),
             COL(255, 0, 0));
    syscall3(SYS_GFX_RECT, 30 | (30 << 16), 60 | (30 << 16),
             COL(0, 255, 0));
    syscall3(SYS_GFX_TEXT, 10 | (80 << 16), (long)"GDEMO",
             COL(255, 255, 255) | (2 << 16));
    syscall3(SYS_GFX_LINE, 0 | (100 << 16), 200 | (130 << 16),
             COL(0, 255, 255));

    puts_("gdemo: done\n");
    exit_(0);
}
