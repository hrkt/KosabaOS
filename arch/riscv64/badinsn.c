/* badinsn.c -- エミュレーションできないトラップの検証プログラム
 *
 *   run badinsn.elf     違法命令 (0x00000000) を実行する
 *   run badinsn.elf fp  浮動小数点の misaligned アクセス (fld)
 *
 * どちらも M シムでは処理できず、段2 の転送でカーネルへ
 * 届いて、このプログラムだけが終了する (基板は死なない)。
 * "badinsn: before" の後に "badinsn: SURVIVED" が出たら失敗
 * (落ちずに通り抜けてしまった)。virt では違法命令は最初から
 * S へ届くので、同じ「プログラムだけ終了」になる (fp の方は
 * QEMU が misaligned を許容するので通ってしまう。実機専用)。
 * 詳細は doc/trap-recovery-design.md。
 */

#include <stdint.h>

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

static void print(const char *s)
{
    while (*s)
        syscall3(SYS_PUTC, *s++, 0, 0);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static char buf[32];

int _start(int argc, char **argv)
{
    print("badinsn: before\n");
    if (argc >= 2 && streq(argv[1], "fp")) {
        /* fld をオフセット 1 で (浮動小数点の misaligned) */
        double d;
        __asm__ volatile("fld %0, 1(%1)" : "=f"(d) : "r"(buf));
        (void)d;
    } else {
        /* 違法命令 (全 0 は unimp) */
        __asm__ volatile(".word 0x00000000");
    }
    print("badinsn: SURVIVED\n");
    syscall3(SYS_EXIT, 0, 0, 0);
    return 0;
}
