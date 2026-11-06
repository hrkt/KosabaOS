/* nish.c -- KosabaOS のシェル "Nish" (ユーザー空間のプログラム)
 *
 * U モードで動く対話プログラム。文字の出し入れもファイルの
 * 読み書きもプログラムの実行も、全部システムコール経由
 * (仕様は doc/syscalls.md)。
 *
 * 起動すると最初に立ち上がる「主役」のプログラム。
 * exit するとカーネルのコンソールに落ちる。
 *
 * コマンド:
 *   help            この一覧
 *   ls [ディレクトリ]  ファイル一覧 (省略時は cwd)
 *   cat <名前>      ファイルの中身を表示
 *   write <名前> <本文>  ファイルを作って本文を書く
 *   rm <名前>       ファイルを消す
 *   mkdir <名前>    ディレクトリを作る
 *   rmdir <名前>    空のディレクトリを消す
 *   cd <名前>       ディレクトリを移る (省略時は /)
 *   pwd             現在のディレクトリを表示
 *   run <名前>      プログラムを実行 (終わると戻ってくる)
 *   echo <本文>     本文をそのまま表示
 *   ps              カーネルタスクの一覧
 *   dmesg           カーネルログを表示
 *   clear           画面を消す
 *   exit            シェルを終わってカーネルに帰る
 *
 * 行編集: ←→ でカーソル移動、↑↓ で履歴 (最大 8 件)、
 * バックスペースで削除ができる。
 */

#define SYS_PUTC 1
#define SYS_GETC 2
#define SYS_EXIT 3
#define SYS_OPEN 4
#define SYS_READ 5
#define SYS_CLOSE 6
#define SYS_READDIR 7
#define SYS_EXEC 8
#define SYS_CREATE 9
#define SYS_WRITE 10
#define SYS_TASKS 11
#define SYS_UNLINK 12
#define SYS_MKDIR 13
#define SYS_RMDIR 14
#define SYS_CHDIR 15
#define SYS_GETCWD 16
#define SYS_KLOG_READ 30

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

/* 0 以上の整数を10進で出す (終了ステータスの表示用) */
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

static int strlen_(const char *s)
{
    int n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void strcpy_(char *dst, const char *src)
{
    int i = 0;
    while (src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* --- 行編集つきの 1 行入力 ------------------------------------------
 * buf の中身を画面と一致させながら編集する。カーソル位置 pos、
 * 長さ len。矢印キーは ESC [ A/B/C/D の並びで届く。 */

#define HIST_MAX 8
#define LINE_MAX 128
static char history[HIST_MAX][LINE_MAX];
static int hist_count = 0;

static const char *PROMPT = "nish> ";

/* 画面上の行を消して、プロンプトと buf を描き直す */
static void redraw(const char *buf)
{
    putc_('\r');
    puts_(PROMPT);
    puts_(buf);
    puts_("\033[K");  /* 行末までの残りを消す */
}

static void readline(char *buf)
{
    int len = 0;
    int pos = 0;
    int nav = -1;             /* 履歴をたどっている位置 (-1 = 新規) */
    char saved[LINE_MAX];     /* たどり始める前の入力 */

    buf[0] = '\0';
    saved[0] = '\0';
    for (;;) {
        long c = getc_();

        if (c == 0x1B) {      /* 矢印キーなどの ESC シーケンス */
            long c2 = getc_();
            long c3;
            if (c2 != '[') {
                continue;
            }
            c3 = getc_();
            if (c3 == 'D') {           /* ← */
                if (pos > 0) {
                    pos--;
                    puts_("\033[D");
                }
            } else if (c3 == 'C') {    /* → */
                if (pos < len) {
                    pos++;
                    puts_("\033[C");
                }
            } else if (c3 == 'A' || c3 == 'B') {  /* ↑ / ↓: 履歴 */
                int target;
                if (c3 == 'A') {
                    if (hist_count == 0) {
                        continue;
                    }
                    if (nav == -1) {
                        strcpy_(saved, buf);
                        target = hist_count - 1;
                    } else if (nav > 0) {
                        target = nav - 1;
                    } else {
                        continue;
                    }
                } else {
                    if (nav == -1) {
                        continue;
                    }
                    target = nav + 1;
                }
                if (target >= hist_count) {
                    strcpy_(buf, saved);
                    nav = -1;
                } else {
                    strcpy_(buf, history[target]);
                    nav = target;
                }
                len = strlen_(buf);
                pos = len;
                redraw(buf);
            }
            continue;
        }

        if (c == '\r' || c == '\n') {
            putc_('\n');
            buf[len] = '\0';
            if (len > 0) {
                /* 履歴へ追加 (満杯なら古い順にずらす) */
                if (hist_count == HIST_MAX) {
                    int i;
                    for (i = 1; i < HIST_MAX; i++) {
                        strcpy_(history[i - 1], history[i]);
                    }
                    hist_count = HIST_MAX - 1;
                }
                strcpy_(history[hist_count], buf);
                hist_count++;
            }
            return;
        }

        if (c == 0x7F || c == 0x08) {  /* バックスペース */
            if (pos > 0) {
                int i;
                for (i = pos - 1; i < len - 1; i++) {
                    buf[i] = buf[i + 1];
                }
                len--;
                pos--;
                buf[len] = '\0';
                putc_('\b');
                puts_(buf + pos);
                putc_(' ');
                puts_("\033[K");
                /* カーソルを pos の位置へ戻す */
                for (i = pos; i <= len; i++) {
                    putc_('\b');
                }
            }
            continue;
        }

        if (c >= 0x20 && c < 0x7F && len < LINE_MAX - 1) {
            int i;
            for (i = len; i > pos; i--) {
                buf[i] = buf[i - 1];
            }
            buf[pos] = (char)c;
            len++;
            pos++;
            buf[len] = '\0';
            putc_((char)c);
            if (pos < len) {  /* 途中への挿入: 後ろを描き直す */
                puts_(buf + pos);
                for (i = pos; i < len; i++) {
                    putc_('\b');
                }
            }
        }
    }
}

/* --- コマンド ------------------------------------------------------- */

/* ls [dir]: dir の中身を 1 行 1 件で出す。ディレクトリには
 * 末尾に '/' を付ける。dir を省略したら cwd を見る */
static void cmd_ls(const char *arg)
{
    char name[32];
    long i;
    int shown = 0;
    for (i = 0;; i++) {
        long r = syscall3(SYS_READDIR, i, (long)name,
                          (*arg != '\0') ? (long)arg : 0);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            if (!shown) {
                puts_("ls: no such directory\n");
            }
            break;
        }
        puts_("  ");
        puts_(name);
        if (r == 2) {
            putc_('/');
        }
        putc_('\n');
        shown = 1;
    }
}

static void cmd_mkdir(const char *arg)
{
    if (syscall3(SYS_MKDIR, (long)arg, 0, 0) < 0) {
        puts_("mkdir: cannot create\n");
        return;
    }
    puts_("created ");
    puts_(arg);
    putc_('\n');
}

static void cmd_rmdir(const char *arg)
{
    if (syscall3(SYS_RMDIR, (long)arg, 0, 0) < 0) {
        puts_("rmdir: cannot remove\n");
        return;
    }
    puts_("removed ");
    puts_(arg);
    putc_('\n');
}

static void cmd_cd(const char *arg)
{
    if (*arg == '\0') {
        arg = "/";
    }
    if (syscall3(SYS_CHDIR, (long)arg, 0, 0) < 0) {
        puts_("cd: no such directory\n");
    }
}

static void cmd_pwd(void)
{
    char buf[128];
    if (syscall3(SYS_GETCWD, (long)buf, sizeof(buf), 0) < 0) {
        puts_("pwd: unavailable\n");
        return;
    }
    puts_(buf);
    putc_('\n');
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

static void cmd_rm(const char *name)
{
    if (syscall3(SYS_UNLINK, (long)name, 0, 0) < 0) {
        puts_("rm: cannot remove\n");
        return;
    }
    puts_("removed ");
    puts_(name);
    putc_('\n');
}

static void cmd_write(char *arg)
{
    char *text = arg;
    long fd, len;

    while (*text != '\0' && *text != ' ') {
        text++;
    }
    if (*text != ' ') {
        puts_("usage: write <name> <text>\n");
        return;
    }
    *text = '\0';
    text++;
    len = strlen_(text);
    fd = syscall3(SYS_CREATE, (long)arg, 0, 0);
    if (fd < 0) {
        puts_("write: cannot create\n");
        return;
    }
    syscall3(SYS_WRITE, fd, (long)text, len);
    syscall3(SYS_CLOSE, fd, 0, 0);
    puts_("wrote ");
    puts_(arg);
    putc_('\n');
}

/* run の引数は「プログラム名＋渡したい引数」の並び全体。
 * 名前だけを切り出して a0 に、並び全体を a1 に渡すと、
 * 子は argv として受け取る (入口の約束、doc/syscalls.md) */
static void cmd_run(char *arg)
{
    char name[128];
    int i = 0;
    long r;

    while (arg[i] != '\0' && arg[i] != ' ' && i < 127) {
        name[i] = arg[i];
        i++;
    }
    name[i] = '\0';
    r = syscall3(SYS_EXEC, (long)name, (long)arg, 0);
    if (r == -1) {
        puts_("run: cannot start\n");
    } else if (r == -2) {
        puts_("run: program faulted\n");
    } else if (r != 0) {
        /* 子の終了ステータス (exit の引数) がそのまま戻る */
        puts_("run: exit status ");
        put_dec_(r);
        putc_('\n');
    }
}

/* dmesg: カーネルログを古い順に全部出す。klog_read (30) で
 * オフセットを進めながら読み切る (0 が返ったら最新まで
 * 読んだということ) */
static void cmd_dmesg(void)
{
    char buf[256];
    long off = 0;
    for (;;) {
        long n = syscall3(SYS_KLOG_READ, (long)buf, sizeof(buf), off);
        long i;
        if (n <= 0) {
            break;
        }
        for (i = 0; i < n; i++) {
            putc_(buf[i]);
        }
        off += n;
        if (n < (long)sizeof(buf)) {
            break;
        }
    }
}

static void cmd_ps(void)
{
    char buf[256];
    long n = syscall3(SYS_TASKS, (long)buf, 0, 0);
    long i;
    if (n < 0) {
        puts_("ps: unavailable\n");
        return;
    }
    for (i = 0; i < n; i++) {
        putc_(buf[i]);
    }
    puts_("(Nish itself is a user program, not a kernel task)\n");
}

__attribute__((section(".text.start")))
void _start(void)
{
    char line[LINE_MAX];

    puts_("Nish, the KosabaOS shell. Type 'help'.\n");
    for (;;) {
        char *arg;

        puts_(PROMPT);
        readline(line);

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
            puts_("help            this list\n");
            puts_("ls [dir]        list files (default: cwd)\n");
            puts_("cat <name>      show a file\n");
            puts_("write <n> <t>   create a file with text\n");
            puts_("rm <name>       remove a file\n");
            puts_("mkdir <name>    make a directory\n");
            puts_("rmdir <name>    remove an empty directory\n");
            puts_("cd <name>       change directory (default: /)\n");
            puts_("pwd             print working directory\n");
            puts_("run <name> [..] run a program (args after the name)\n");
            puts_("echo <text>     print text\n");
            puts_("ps              kernel tasks\n");
            puts_("dmesg           show the kernel log\n");
            puts_("clear           clear the screen\n");
            puts_("exit            leave Nish (to the kernel console)\n");
        } else if (streq(line, "ls")) {
            cmd_ls(arg);
        } else if (streq(line, "mkdir")) {
            cmd_mkdir(arg);
        } else if (streq(line, "rmdir")) {
            cmd_rmdir(arg);
        } else if (streq(line, "cd")) {
            cmd_cd(arg);
        } else if (streq(line, "pwd")) {
            cmd_pwd();
        } else if (streq(line, "cat")) {
            cmd_cat(arg);
        } else if (streq(line, "write")) {
            cmd_write(arg);
        } else if (streq(line, "rm")) {
            cmd_rm(arg);
        } else if (streq(line, "run")) {
            cmd_run(arg);
        } else if (streq(line, "echo")) {
            puts_(arg);
            putc_('\n');
        } else if (streq(line, "ps")) {
            cmd_ps();
        } else if (streq(line, "dmesg")) {
            cmd_dmesg();
        } else if (streq(line, "clear")) {
            puts_("\033[2J\033[H");
        } else if (streq(line, "exit")) {
            syscall3(SYS_EXIT, 0, 0, 0);
        } else {
            puts_("unknown command (try 'help')\n");
        }
    }
}
