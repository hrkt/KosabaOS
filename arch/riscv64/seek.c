/* seek.c -- lseek syscall (17) と位置指定 write の検証用
 * ユーザープログラム (FS 再設計 段3)
 *
 * RAM FS と SD (FAT) の両方で、同じ手順を試す:
 *
 *   run seek.elf
 *     1. create して "ABCDEFGHIJ" (10 バイト) を書く。
 *        書いた直後の位置は 10
 *     2. lseek で 3 へ戻して "xyz" を上書きする。ファイルの
 *        大きさは 10 のまま (末尾は伸びない)
 *     3. lseek で 20 (終端より後ろ) へ動かして "Q" を書く。
 *        大きさは 21 になり、間の 10 バイトは 0 で埋まる
 *     4. open し直して全部読み戻し、内容が
 *        "ABCxyzGHIJ" + 0 x10 + "Q" であることを見る
 *     5. 読み側のランダムアクセス (seek して途中から読む、
 *        末尾基準の負オフセット) と、エラー系 (負になる
 *        seek は -1 で位置が動かない、不正な基準は -1)
 *     全部 OK なら対象ごとに "seek ram ok" / "seek sd ok"、
 *     最後に "seek ok" と出してステータス 0 で終わる。駄目
 *     なら "seek FAIL: 理由" でステータス 9x ではなく
 *     10 番台 (RAM) / 40 番台 (SD) のコードで終わる
 *     (test.sh のシナリオ AF、test-k210.py の K25 が見る)。
 *     SD 側のテストファイルは最後に消す
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_OPEN 4
#define SYS_READ 5
#define SYS_CLOSE 6
#define SYS_CREATE 9
#define SYS_WRITE 10
#define SYS_UNLINK 12
#define SYS_LSEEK 17

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

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

static long open_(const char *path)
{
    return syscall3(SYS_OPEN, (long)path, 0, 0);
}

static long create_(const char *path)
{
    return syscall3(SYS_CREATE, (long)path, 0, 0);
}

static long read_(long fd, char *buf, long n)
{
    return syscall3(SYS_READ, fd, (long)buf, n);
}

static long write_(long fd, const char *buf, long n)
{
    return syscall3(SYS_WRITE, fd, (long)buf, n);
}

static long close_(long fd)
{
    return syscall3(SYS_CLOSE, fd, 0, 0);
}

static long lseek_(long fd, long off, long whence)
{
    return syscall3(SYS_LSEEK, fd, off, whence);
}

static void fail(const char *reason, long status)
{
    puts_("seek FAIL: ");
    puts_(reason);
    putc_('\n');
    exit_(status);
}

static void test_file(const char *path, const char *tag, long st0)
{
    long fd, n, i;
    char buf[32];

    fd = create_(path);
    if (fd < 0) {
        fail("create", st0);
    }
    n = write_(fd, "ABCDEFGHIJ", 10);
    if (n != 10) {
        fail("write", st0 + 1);
    }
    if (lseek_(fd, 0, SEEK_CUR) != 10) {
        fail("pos after write", st0 + 2);
    }
    /* 途中への上書き: 大きさは変わらない */
    if (lseek_(fd, 3, SEEK_SET) != 3) {
        fail("seek set", st0 + 3);
    }
    n = write_(fd, "xyz", 3);
    if (n != 3) {
        fail("overwrite", st0 + 4);
    }
    if (lseek_(fd, 0, SEEK_END) != 10) {
        fail("size after overwrite", st0 + 5);
    }
    /* 終端より後ろへの書き込み: 間は 0 で埋まる */
    if (lseek_(fd, 20, SEEK_SET) != 20) {
        fail("seek past end", st0 + 6);
    }
    n = write_(fd, "Q", 1);
    if (n != 1) {
        fail("gap write", st0 + 7);
    }
    if (lseek_(fd, 0, SEEK_END) != 21) {
        fail("size after gap", st0 + 8);
    }
    close_(fd);

    fd = open_(path);
    if (fd < 0) {
        fail("open", st0 + 9);
    }
    n = read_(fd, buf, 21);
    if (n != 21) {
        fail("read back", st0 + 10);
    }
    if (!(buf[0] == 'A' && buf[1] == 'B' && buf[2] == 'C' &&
          buf[3] == 'x' && buf[4] == 'y' && buf[5] == 'z' &&
          buf[6] == 'G' && buf[7] == 'H' && buf[8] == 'I' &&
          buf[9] == 'J')) {
        fail("content head", st0 + 11);
    }
    for (i = 10; i < 20; i++) {
        if (buf[i] != 0) {
            fail("gap not zero", st0 + 12);
        }
    }
    if (buf[20] != 'Q') {
        fail("content tail", st0 + 13);
    }
    /* 読み側のランダムアクセス */
    if (lseek_(fd, 4, SEEK_SET) != 4) {
        fail("rseek", st0 + 14);
    }
    n = read_(fd, buf, 3);
    if (n != 3 || !(buf[0] == 'y' && buf[1] == 'z' && buf[2] == 'G')) {
        fail("random read", st0 + 15);
    }
    if (lseek_(fd, -1, SEEK_END) != 20) {
        fail("seek end-1", st0 + 16);
    }
    n = read_(fd, buf, 1);
    if (n != 1 || buf[0] != 'Q') {
        fail("read last", st0 + 17);
    }
    /* エラー系: 失敗した seek では位置は動かない */
    if (lseek_(fd, -1, SEEK_SET) != -1) {
        fail("negative seek", st0 + 18);
    }
    if (lseek_(fd, 0, SEEK_CUR) != 21) {
        fail("pos moved on error", st0 + 19);
    }
    if (lseek_(fd, 0, 99) != -1) {
        fail("bad whence", st0 + 20);
    }
    close_(fd);

    puts_("seek ");
    puts_(tag);
    puts_(" ok\n");
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_file("seektest.txt", "ram", 10);
    test_file("/sd/SEEKTEST.TXT", "sd", 40);
    syscall3(SYS_UNLINK, (long)"/sd/SEEKTEST.TXT", 0, 0);

    puts_("seek ok\n");
    exit_(0);
}
