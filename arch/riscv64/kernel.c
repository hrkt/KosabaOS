/* kernel.c -- KosabaOS カーネル本体 (riscv64)
 *
 * start.S がスタックと .bss の準備をしてから呼ぶ、C 側の入口。
 * 今やっていることは、アセンブラ時代とまったく同じ:
 *   1. UART を初期化する
 *   2. hello を出す
 *   3. 打たれたキーをそのまま打ち返す (echo) を繰り返す
 * 中身は同じでも、書く言語が C になったのがステップ 1 の要点。
 */

#include "uart.h"

void kmain(void)
{
    uart_init();
    uart_puts("hello from KosabaOS (S-mode)\n");

    for (;;) {
        char c = uart_getc();  /* 1 文字届くまで待って受け取る */
        uart_putc(c);          /* そのまま打ち返す */
    }
}
