/* clock.c -- 帯の時計 (ユーザープログラム、常駐ジョブ)。
 * doc/userland-design.md 決定 1・3・4。
 *
 * 起動すると gfx_layout (51) で帯の窓 (画面下部 24px)
 * を要求し、uptime (31) を毎秒読んで "TICK <秒>" を
 * 帯に描く。かつてカーネルの時計タスクが帯を描いていた
 * 時代の置き換えで、カーネルはもう時刻表示を持たない。
 * フォントは当面 gfx_text (46) のカーネル側 5x7 を使う
 * (libdashi のフォントマネージャが来たら乗り換える)。
 * 壁時計 (HH:MM:SS) は syscall 32 が入ってから。
 *
 * Nish (段4 以後は getty) がスタートアップでジョブと
 * して起こす。止めるのは Nish の kill。帯の窓は終了時に
 * カーネルが自動で解放する。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_UPTIME 31
#define SYS_SLEEP_MS 34
#define SYS_GFX_INFO 43
#define SYS_GFX_CLEAR 44
#define SYS_GFX_TEXT 46
#define SYS_GFX_LAYOUT 51

#define COL(r, g, b) \
    ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define BG COL(24, 24, 24)      /* 帯の地色 (旧 tick と同じ) */
#define FG COL(0, 255, 255)     /* シアン (旧 tick と同じ) */

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

/* "TICK " の後ろに秒数を 10 進で続けた文字列を作る */
static void make_label(char *buf, unsigned long sec)
{
    char rev[24];
    int n = 0, m = 0;

    buf[n++] = 'T';
    buf[n++] = 'I';
    buf[n++] = 'C';
    buf[n++] = 'K';
    buf[n++] = ' ';
    if (sec == 0) {
        rev[m++] = '0';
    }
    while (sec > 0) {
        rev[m++] = (char)('0' + (sec % 10));
        sec /= 10;
    }
    while (m > 0) {
        buf[n++] = rev[--m];
    }
    buf[n] = '\0';
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    unsigned int info[4];
    long last = -1;
    char label[32];

    (void)argc;
    (void)argv;
    if (syscall3(SYS_GFX_LAYOUT, 1, 0, 0) != 0) {
        puts_("clock: strip busy\n");
        exit_(1);
    }
    if (syscall3(SYS_GFX_INFO, (long)info, 0, 0) != 0) {
        puts_("clock: info failed\n");
        exit_(1);
    }
    puts_("clock: start\n");
    for (;;) {
        long sec = syscall3(SYS_UPTIME, 0, 0, 0) / 1000;

        if (sec != last) {
            last = sec;
            make_label(label, (unsigned long)sec);
            syscall3(SYS_GFX_CLEAR, BG, 0, 0);
            syscall3(SYS_GFX_TEXT, 0, (long)label, FG | (3 << 16));
        }
        syscall3(SYS_SLEEP_MS, 200, 0, 0);
    }
}
