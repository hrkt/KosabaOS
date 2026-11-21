/* top.c -- Unix の top 風モニタ (doc/top-design.md 段3)
 *
 *   run top.elf [回数]
 *     1 秒ごとに SYS_TASKS (11) を読み直し、前回との
 *     cpu= の差分から %CPU を出して描き直す。q で終了。
 *     回数を渡すと、その回数だけ描いて終わる (テスト用)。
 *
 * 待ちは getc のタイムアウト (段2) で受けるので、キー
 * が無ければ 1 秒ごとに進み、q が来たらすぐ終わる。
 * 対象は task (カーネルタスク) と run (ユーザープロ
 * グラム) の両方。task 0 の cpu は土台の数字 (走者の
 * syscall やポーリング待ちの回転も含む) なので、% が
 * 大きめに出ることがある (段1 の既知の注意)。
 */

#define SYS_PUTC 1
#define SYS_GETC 2
#define SYS_EXIT 3
#define SYS_TASKS 11
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
    if (v < 0) {
        putc_('-');
        v = -v;
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

/* 前回の cpu= (ミリ秒)。task と run で別に持つ */
static long prev_cpu[2][8];
static int have_prev;

static void put_pct(long delta_cpu, long delta_wall)
{
    long pct10;

    if (!have_prev || delta_wall <= 0) {
        puts_("--.-%");
        return;
    }
    pct10 = delta_cpu * 1000 / delta_wall;
    if (pct10 > 1000) {
        pct10 = 1000;
    }
    if (pct10 < 0) {
        pct10 = 0;
    }
    put_dec_(pct10 / 10);
    putc_('.');
    put_dec_(pct10 % 10);
    putc_('%');
}

/* 1 行 (改行を含まない) を出し、cpu= があれば % を足す。
 * 行の先頭の "task N:" / "run N:" から前回値を引く */
static void draw_line(const char *s, int len, long delta_wall)
{
    int i;
    int kind = -1;
    int id = 0;
    long cpu = -1;

    if (len > 5 && s[0] == 't' && s[1] == 'a' && s[2] == 's' &&
        s[3] == 'k' && s[4] == ' ') {
        kind = 0;
        id = atoi_(s + 5);
    }
    if (len > 4 && s[0] == 'r' && s[1] == 'u' && s[2] == 'n' &&
        s[3] == ' ') {
        kind = 1;
        id = atoi_(s + 4);
    }
    for (i = 0; i + 4 < len; i++) {
        if (s[i] == 'c' && s[i + 1] == 'p' && s[i + 2] == 'u' &&
            s[i + 3] == '=') {
            cpu = atoi_(s + i + 4);
            break;
        }
    }
    for (i = 0; i < len; i++) {
        putc_(s[i]);
    }
    if (kind >= 0 && cpu >= 0 && id >= 0 && id < 8) {
        puts_("  ");
        put_pct(cpu - prev_cpu[kind][id], delta_wall);
        prev_cpu[kind][id] = cpu;
    }
    putc_('\n');
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    static char buf[1024];
    long count = argc >= 2 ? atoi_(argv[1]) : 0;
    long frame_no = 0;
    long t_prev = 0;

    for (;;) {
        long now = syscall3(SYS_UPTIME, 0, 0, 0);
        long delta_wall = now - t_prev;
        long n;
        long i, start;
        long c;

        t_prev = now;
        n = syscall3(SYS_TASKS, (long)buf, 0, 0);
        if (n < 0) {
            puts_("top: tasks unavailable\n");
            exit_(1);
        }
        /* 画面を消して先頭から描く */
        puts_("\x1b[H\x1b[2J");
        puts_("top - up ");
        put_dec_(now / 1000);
        puts_(" s   (q: quit)\n");
        start = 0;
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                draw_line(buf + start, (int)(i - start),
                          delta_wall);
                start = i + 1;
            }
        }
        if (start < n) {
            draw_line(buf + start, (int)(n - start), delta_wall);
        }
        have_prev = 1;
        frame_no++;
        if (count > 0 && frame_no >= count) {
            exit_(0);
        }
        c = syscall3(SYS_GETC, 0, 1000, 0);
        if (c == 'q') {
            exit_(0);
        }
    }
}
