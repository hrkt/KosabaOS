/* copy.c -- ファイルをバイト列のままコピーする小さな道具
 *
 *   run copy.elf SRC DST
 *
 * Nish の write はテキスト前提 (引数の文字列を書く) なので、
 * ELF のようなバイナリは化けて書けない。SD からの exec
 * (doc/sd-exec-design.md) の実機確認で「RAM FS 上の ELF を
 * /sd へ運ぶ」ために用意したもので、使うのは既存 syscall
 * (open / read / create / write / close) だけ。
 * 成功したら "copied N bytes" を出してステータス 0、
 * 失敗したら理由を出してステータス 1 で終わる。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_OPEN 4
#define SYS_READ 5
#define SYS_CLOSE 6
#define SYS_CREATE 9
#define SYS_WRITE 10

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

static void fail(const char *what, const char *path)
{
    puts_("copy: ");
    puts_(what);
    putc_(' ');
    puts_(path);
    putc_('\n');
    exit_(1);
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    static unsigned char buf[512];
    long in, out, n;
    long total = 0;

    if (argc < 3) {
        puts_("usage: copy.elf SRC DST\n");
        exit_(2);
    }
    in = syscall3(SYS_OPEN, (long)argv[1], 0, 0);
    if (in < 0) {
        fail("cannot open", argv[1]);
    }
    out = syscall3(SYS_CREATE, (long)argv[2], 0, 0);
    if (out < 0) {
        fail("cannot create", argv[2]);
    }
    for (;;) {
        long done = 0;
        n = syscall3(SYS_READ, in, (long)buf, sizeof(buf));
        if (n < 0) {
            fail("read failed", argv[1]);
        }
        if (n == 0) {
            break;  /* 終端 */
        }
        while (done < n) {
            long w = syscall3(SYS_WRITE, out, (long)(buf + done),
                              n - done);
            if (w <= 0) {
                fail("write failed", argv[2]);
            }
            done += w;
        }
        total += n;
    }
    syscall3(SYS_CLOSE, in, 0, 0);
    syscall3(SYS_CLOSE, out, 0, 0);
    puts_("copied ");
    put_dec_(total);
    puts_(" bytes\n");
    exit_(0);
}
