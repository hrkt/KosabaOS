/* chain.c -- exec 多段化の検証用ユーザープログラム
 *
 * 自分自身を exec して段を重ねることで、次を確認するための
 * テストの道具 (通常の利用のためのプログラムではない):
 *
 *   数え上げモード: run chain.elf 3
 *     chain 3 → chain 2 → chain 1 → chain 0 と 3 段の exec が
 *     重なり、終了ステータスが 1 段ずつ +1 されながら戻る
 *     (0 番が 3 で終わり、最終的に Nish には 6 が返る)。
 *     一番上 (3 番) は hello.txt を開いて 5 バイト読み、fd を
 *     開いたまま子を exec する。子が終わったあとも同じ fd から
 *     続きが読め (fdok)、前に読んだ中身も壊れていない (mem)
 *     ことで、「親の fd とメモリが子の後も生きている」を見る。
 *
 *   深さモード: run chain.elf deep 25
 *     sanity 上限 (16 段) を超える深さまで exec を重ねようとし、
 *     上限の段で exec が -1 になって "deep denied" と出る。
 *     その段はステータス 9 で終わり、上限より下の段はそれを
 *     そのまま伝えるので、Nish には 9 が返る (底まで届けば
 *     "deep bottom" が出るが、上限があるので出ないはず)。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_OPEN 4
#define SYS_READ 5
#define SYS_CLOSE 6
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

static int streq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
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

/* 数を文字列にする ("chain.elf 2" のような exec 用引数を作る) */
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

static void exit_(long status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

/* スタックを食う再帰: 1 段 256 バイトの配列を持ち、64 段で
 * 約 16KB を超える。起動時の初期分 (16KB) では足りないので、
 * fault でページが伸びることが前提になる。戻りながら各段の
 * 配列が無事か (合計が合うか) も見る */
static long burn_acc;

static void stack_burn(int n)
{
    char buf[256];

    buf[0] = (char)n;
    buf[255] = (char)n;
    if (n > 0) {
        stack_burn(n - 1);
    }
    burn_acc += buf[0] + buf[255];
}

/* --- 数え上げモード -------------------------------------------------- */

static void mode_count(int n)
{
    long fd = -1;
    char first[6] = "";  /* exec 前に読んだ 5 バイト + NUL */

    puts_("chain ");
    put_dec_(n);
    puts_(" start\n");

    if (n == 3) {
        /* スタック伸長の確認 (合計は 2 * (0+...+64) = 4160) */
        stack_burn(64);
        puts_("burn=");
        put_dec_(burn_acc);
        putc_('\n');
        if (burn_acc != 4160) {
            puts_("chain: burn mismatch\n");
            exit_(95);
        }
    }

    if (n == 3) {
        /* 一番上だけ: fd を開いて少し読み、開いたまま子へ行く */
        long got;
        fd = syscall3(SYS_OPEN, (long)"hello.txt", 0, 0);
        if (fd < 0) {
            puts_("chain: open failed\n");
            exit_(90);
        }
        got = syscall3(SYS_READ, fd, (long)first, 5);
        if (got != 5) {
            puts_("chain: read failed\n");
            exit_(91);
        }
        first[5] = '\0';
    }

    if (n == 0) {
        exit_(3);  /* 底: ステータス 3 で終わる */
    }

    {
        char num[12];
        char args[32];
        int i = 0;
        const char *p = "chain.elf ";
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

        r = syscall3(SYS_EXEC, (long)"chain.elf", (long)args, 0);
        if (r < 0) {
            puts_("chain ");
            put_dec_(n);
            puts_(" exec failed\n");
            exit_(92);
        }
        puts_("chain ");
        put_dec_(n);
        puts_(" child=");
        put_dec_(r);
        putc_('\n');

        if (n == 3) {
            /* 子が終わったあとの確認: fd の続きが読めることと、
             * exec 前に読んだ中身 (自分のデータ) が無事なこと */
            char next[5] = "";
            long got = syscall3(SYS_READ, fd, (long)next, 4);
            if (got != 4) {
                puts_("chain: fd lost\n");
                exit_(93);
            }
            next[4] = '\0';
            puts_("fdok=");
            puts_(next);
            putc_('\n');
            puts_("mem=");
            puts_(first);
            putc_('\n');
            syscall3(SYS_CLOSE, fd, 0, 0);
        }
        exit_(r + 1);  /* ステータスを 1 つ増やして親へ返す */
    }
}

/* --- 深さモード ------------------------------------------------------ */

static void mode_deep(int n)
{
    if (n == 0) {
        puts_("deep bottom\n");
        exit_(9);
    }
    {
        char num[12];
        char args[40];
        int i = 0;
        const char *p = "chain.elf deep ";
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

        r = syscall3(SYS_EXEC, (long)"chain.elf", (long)args, 0);
        if (r < 0) {
            /* sanity 上限に達した段: ここで打ち止めになる */
            puts_("deep denied\n");
            exit_(9);
        }
        exit_(r);  /* ステータスはそのまま上へ伝える */
    }
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    if (argc >= 2 && streq(argv[1], "deep")) {
        mode_deep(argc >= 3 ? atoi_(argv[2]) : 0);
    } else if (argc >= 2) {
        mode_count(atoi_(argv[1]));
    } else {
        mode_count(3);
    }
    exit_(94);  /* ここへは来ないはず */
}
