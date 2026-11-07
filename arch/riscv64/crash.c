/* crash.c -- ジョブ制御 (スケジューラ段3) の検証用ユーザープログラム
 *
 * フォルトする役。開始を出してから、割り当ての無い住所
 * (0x60000000、vm_fault_demo と同じ場所) へ書いて落ちる。
 * バックグラウンドで走らせたときに、このプログラムだけが
 * 終わってシェル (Nish) と他のジョブが無事なこと
 * (障害の分離) を見るための道具 (test.sh シナリオ X)。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3

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

__attribute__((section(".text.start")))
void _start(void)
{
    puts_("crash start\n");
    /* 未割り当ての住所への書き込み = store のフォルト */
    *(volatile unsigned char *)0x60000000UL = 1;
    puts_("crash end\n");  /* ここへは届かないはず */
    syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) {
    }
}
