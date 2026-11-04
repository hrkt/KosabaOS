/* page.c -- 物理ページのアロケータ
 *
 * プール (board.h が決める物理アドレスの範囲) の各ページに
 * ついて「使用中かどうか」をビットマップで持つだけの、
 * 小さい部品。1 ビット = 1 ページ (4KB) で、1 = 使用中、
 * 0 = 空き。確保は先頭から空きを探す素直な線形探索
 * (プールが数万ページでも、exec のような低頻度の場面では
 * 十分)。解放は範囲と整列を確かめてからビットを戻す。
 *
 * 返すページはゼロで埋める: ページテーブルは「全部 0 =
 * 全部無効」が前提だし、プログラムのデータ領域も BSS の
 * ゼロ初期化と相性がよい。埋めるのは 8 バイト単位
 * (ページ先頭は 4KB 整列なので境界の問題は出ない)。
 */

#include <stdint.h>
#include "page.h"
#include "board.h"
#include "uart.h"

#define PAGE_SIZE 4096ULL
#define POOL_PAGES \
    ((BOARD_PAGE_POOL_END - BOARD_PAGE_POOL_BASE) / PAGE_SIZE)
/* プールが空のボードでも配列が 1 ワードはあるように +64 する */
#define BITMAP_WORDS ((POOL_PAGES + 64) / 64)

static uint64_t bitmap[BITMAP_WORDS];
static uint64_t free_pages = 0;

void page_init(void)
{
    uint64_t i;
    for (i = 0; i < BITMAP_WORDS; i++) {
        bitmap[i] = 0;
    }
    free_pages = POOL_PAGES;
}

uint64_t page_alloc(void)
{
    uint64_t i, j;

    if (free_pages == 0) {
        return 0;
    }
    for (i = 0; i < BITMAP_WORDS; i++) {
        uint64_t word = bitmap[i];
        if (word == 0xFFFFFFFFFFFFFFFFULL) {
            continue;
        }
        for (j = 0; j < 64; j++) {
            uint64_t idx = i * 64 + j;
            uint64_t pa;
            uint64_t *p;
            uint64_t k;

            if (idx >= POOL_PAGES) {
                return 0;  /* 範囲の外は貸さない */
            }
            if (word & (1ULL << j)) {
                continue;
            }
            bitmap[i] = word | (1ULL << j);
            free_pages--;
            pa = BOARD_PAGE_POOL_BASE + idx * PAGE_SIZE;
            p = (uint64_t *)pa;
            for (k = 0; k < PAGE_SIZE / 8; k++) {
                p[k] = 0;
            }
            return pa;
        }
    }
    return 0;
}

void page_free(uint64_t pa)
{
    uint64_t idx;

    if (pa < BOARD_PAGE_POOL_BASE || pa >= BOARD_PAGE_POOL_END) {
        return;  /* プールの外は相手にしない */
    }
    if ((pa & (PAGE_SIZE - 1)) != 0) {
        return;  /* ページ先頭でなければおかしい */
    }
    idx = (pa - BOARD_PAGE_POOL_BASE) / PAGE_SIZE;
    if ((bitmap[idx / 64] & (1ULL << (idx % 64))) == 0) {
        return;  /* 二重解放は無視する */
    }
    bitmap[idx / 64] &= ~(1ULL << (idx % 64));
    free_pages++;
}

uint64_t page_free_count(void)
{
    return free_pages;
}

uint64_t page_total_count(void)
{
    return POOL_PAGES;
}

/* --- カーネルコンソールのデモ ('p' キー、kernel.c から) --------------
 * アロケータの動きを目で確かめるためのもの (vm_fault_demo と
 * 同じ位置づけ): 空き数 → 3 枚借りる → 1 枚返して借り直す
 * (返したページが再利用されること) → 全部返して空き数が
 * 元に戻ること、までを表示する。プールが空のボードでは
 * 「借りられない」が正しい動きとして表示される */

static void put_u64(uint64_t x)
{
    char buf[20];
    int n = 0;

    if (x == 0) {
        uart_putc('0');
        return;
    }
    while (x > 0) {
        buf[n++] = (char)('0' + (x % 10));
        x /= 10;
    }
    while (n > 0) {
        uart_putc(buf[--n]);
    }
}

static void put_hex(uint64_t x)
{
    int shift;
    uart_puts("0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        int d = (int)((x >> shift) & 0xF);
        uart_putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
}

void page_demo(void)
{
    uint64_t before = page_free_count();
    uint64_t a, b, c, d;

    uart_puts("\npage: pool total ");
    put_u64(page_total_count());
    uart_puts(", free ");
    put_u64(before);
    uart_putc('\n');

    a = page_alloc();
    b = page_alloc();
    c = page_alloc();
    uart_puts("page: alloc ");
    put_hex(a);
    uart_putc(' ');
    put_hex(b);
    uart_putc(' ');
    put_hex(c);
    uart_puts(", free ");
    put_u64(page_free_count());
    uart_putc('\n');
    if (a == 0) {
        uart_puts("page: pool is empty on this board\n");
        return;
    }
    page_free(b);
    d = page_alloc();  /* 返した b が再利用されるはず */
    uart_puts("page: freed one, realloc ");
    put_hex(d);
    uart_puts(d == b ? " (reused)\n" : " (NOT reused)\n");
    page_free(a);
    page_free(d);
    page_free(c);
    uart_puts("page: free all, free ");
    put_u64(page_free_count());
    uart_puts(page_free_count() == before ? " (back to start)\n"
                                          : " (LEAK)\n");
}
