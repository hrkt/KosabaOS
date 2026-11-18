/* misalign.c -- misaligned アクセスの検証プログラム
 *
 * 境界にそろっていないアドレスへ ld / sd などを行い、
 * 値が正しく読み書きできるかを確認する。virt (QEMU) は
 * misaligned を許容するので素通りする。K210 では M シムが
 * エミュレーションして続行する (doc/trap-recovery-design.md)。
 * 失敗したら段階番号つきで知らせる (終了コード 10 + 段)。
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

static void fail(int step)
{
    print("misalign FAIL ");
    syscall3(SYS_PUTC, '0' + step, 0, 0);
    syscall3(SYS_PUTC, '\n', 0, 0);
    syscall3(SYS_EXIT, 10 + step, 0, 0);
}

/* テスト用の台 (自然な配置に任せず、オフセットでずらす) */
static char buf[64];

int _start(int argc, char **argv)
{
    uint64_t v64;
    uint32_t v32;
    uint16_t v16;
    volatile char *p;

    (void)argc;
    (void)argv;
    p = buf;

    /* 段1: 8 バイトの書き/読みをオフセット 1 で */
    __asm__ volatile("sd %0, 1(%1)"
                     :: "r"(0x0123456789ABCDEFULL), "r"(p));
    __asm__ volatile("ld %0, 1(%1)" : "=r"(v64) : "r"(p));
    if (v64 != 0x0123456789ABCDEFULL)
        fail(1);

    /* 段2: 4 バイトをオフセット 2 で */
    __asm__ volatile("sw %0, 2(%1)" :: "r"(0xDEADBEEF), "r"(p + 8));
    __asm__ volatile("lw %0, 2(%1)" : "=r"(v32) : "r"(p + 8));
    if (v32 != 0xDEADBEEFU)
        fail(2);

    /* 段3: 2 バイトをオフセット 1 で */
    __asm__ volatile("sh %0, 1(%1)" :: "r"(0xBEEF), "r"(p + 16));
    __asm__ volatile("lh %0, 1(%1)" : "=r"(v16) : "r"(p + 16));
    if (v16 != 0xBEEF)
        fail(3);

    /* 段4: 符号拡張つき 4 バイト読み (負数) をオフセット 3 で */
    __asm__ volatile("sw %0, 3(%1)" :: "r"(-12345), "r"(p + 24));
    __asm__ volatile("lw %0, 3(%1)" : "=r"(v32) : "r"(p + 24));
    if ((int32_t)v32 != -12345)
        fail(4);

    /* 段5: ページ境界をまたぐ 8 バイト (バッファ末尾付近ではなく
     * 配置はコンパイラ任せだが、オフセット 5 の 8 バイトで
     * 8 バイト境界をまたぐ形) */
    __asm__ volatile("sd %0, 5(%1)"
                     :: "r"(0xFEEDFACE12345678ULL), "r"(p + 32));
    __asm__ volatile("ld %0, 5(%1)" : "=r"(v64) : "r"(p + 32));
    if (v64 != 0xFEEDFACE12345678ULL)
        fail(5);

    print("misalign ok\n");
    syscall3(SYS_EXIT, 0, 0, 0);
    return 0;
}
