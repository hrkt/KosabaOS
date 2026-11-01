/* shell.c -- ユーザー空間のシェル (ステップ 6)
 *
 * U モードで動く対話プログラム。文字の出し入れもファイルの
 * 読みもプログラムの実行も、全部システムコール経由
 * (仕様は doc/syscalls.md v1)。
 *
 * コマンド:
 *   help          この一覧
 *   ls            ファイル一覧
 *   cat <名前>    ファイルの中身を表示
 *   run <名前>    プログラムを実行 (終わると戻ってくる)
 *   exit          シェルを終わってカーネルに帰る
 */

#define SYS_PUTC 1
#define SYS_GETC 2
#define SYS_EXIT 3
#define SYS_OPEN 4
#define SYS_READ 5
#define SYS_CLOSE 6
#define SYS_READDIR 7
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
static long getc_(void) { return syscall3(SYS_GETC, 0, 0, 0); }

static void puts_(const char *s)
{
    while (*s != '\0') {
        putc_(*s);
        s++;
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

static void cmd_ls(void)
{
    char name[32];
    long i;
    for (i = 0;; i++) {
        if (syscall3(SYS_READDIR, i, (long)name, 0) != 1) {
            break;
        }
        puts_("  ");
        puts_(name);
        putc_('\n');
    }
}

static void cmd_cat(const char *name)
{
    char buf[64];
    long fd, n;

    fd = syscall3(SYS_OPEN, (long)name, 0, 0);
    if (fd < 0) {
        puts_("cat: no such file\n");
        return;
    }
    while ((n = syscall3(SYS_READ, fd, (long)buf, sizeof(buf) - 1)) > 0) {
        long i;
        for (i = 0; i < n; i++) {
            putc_(buf[i]);
        }
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
}

static void cmd_run(const char *name)
{
    long r = syscall3(SYS_EXEC, (long)name, 0, 0);
    if (r == -1) {
        puts_("run: cannot start\n");
    } else if (r == -2) {
        puts_("run: program faulted\n");
    }
}

/* 1 行読み取る (画面への echo とバックスペース処理つき)。
 * 戻り値は行の長さ */
static int readline(char *buf, int max)
{
    int n = 0;
    for (;;) {
        long c = getc_();
        if (c == '\r' || c == '\n') {
            putc_('\n');
            buf[n] = '\0';
            return n;
        }
        if (c == 0x7F || c == 0x08) {  /* DEL / BS */
            if (n > 0) {
                n--;
                puts_("\b \b");
            }
            continue;
        }
        if (n < max - 1) {
            buf[n++] = (char)c;
            putc_((char)c);
        }
    }
}

__attribute__((section(".text.start")))
void _start(void)
{
    char line[128];

    puts_("KosabaOS shell. Type 'help'.\n");
    for (;;) {
        char *arg;

        puts_("kosaba> ");
        readline(line, sizeof(line));

        /* コマンドと引数を最初の空白で分ける */
        arg = line;
        while (*arg != '\0' && *arg != ' ') {
            arg++;
        }
        if (*arg == ' ') {
            *arg = '\0';
            arg++;
            while (*arg == ' ') {
                arg++;
            }
        }

        if (line[0] == '\0') {
            continue;
        } else if (streq(line, "help")) {
            puts_("help        this list\n");
            puts_("ls          list files\n");
            puts_("cat <name>  show a file\n");
            puts_("run <name>  run a program\n");
            puts_("exit        leave the shell\n");
        } else if (streq(line, "ls")) {
            cmd_ls();
        } else if (streq(line, "cat")) {
            cmd_cat(arg);
        } else if (streq(line, "run")) {
            cmd_run(arg);
        } else if (streq(line, "exit")) {
            syscall3(SYS_EXIT, 0, 0, 0);
        } else {
            puts_("unknown command (try 'help')\n");
        }
    }
}
