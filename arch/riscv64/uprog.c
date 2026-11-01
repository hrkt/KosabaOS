/* uprog.c -- 最初のユーザープログラム
 *
 * ユーザーモード (U モード) で動く側のプログラム。UART には
 * 直接触れない (メモリ保護で届かない) ので、文字の出し入れは
 * すべてシステムコール (ecall) でカーネルに頼む。
 * 仕様は doc/syscalls.md (KosabaOS システムコール仕様 v0)。
 *
 * 動き: 挨拶を出して、その後はカーネル版と同じ echo。
 * 'q' を打つと exit を呼んでカーネルに帰る。
 */

#define SYS_PUTC 1
#define SYS_GETC 2
#define SYS_EXIT 3

/* ecall の約束: a7 = 番号、a0 = 引数と戻り値 */
static long syscall1(long num, long arg)
{
    register long a0 asm("a0") = arg;
    register long a7 asm("a7") = num;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static void puts(const char *s)
{
    while (*s != '\0') {
        syscall1(SYS_PUTC, *s);
        s++;
    }
}

/* 書き換える静的変数 (.data と .bss)。データ専用領域が
 * できたことの確認用 probe: コード領域だけだった頃は、
 * ここへの書き込みが store page fault になっていた */
static int data_probe = 7;
static char bss_probe;

/* 入口はバイナリの先頭に置く (user.ld が .text.start を先頭に
 * 配置する) */
__attribute__((section(".text.start")))
void _start(void)
{
    puts("hello from user mode!\n");
    data_probe++;
    bss_probe = 'x';
    puts("data probe: ");
    syscall1(SYS_PUTC, '0' + data_probe);
    syscall1(SYS_PUTC, ' ');
    syscall1(SYS_PUTC, bss_probe);
    syscall1(SYS_PUTC, '\n');

    for (;;) {
        long c = syscall1(SYS_GETC, 0);
        if (c == 'q') {
            syscall1(SYS_EXIT, 0);  /* ここからは戻ってこない */
        }
        syscall1(SYS_PUTC, c);
    }
}
