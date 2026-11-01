/* kernel.c -- KosabaOS カーネル本体 (riscv64)
 *
 * start.S がスタックと .bss の準備をしてから呼ぶ、C 側の入口。
 *   1. UART を初期化する
 *   2. トラップ (例外・割り込み) の受け口を有効にし、
 *      タイマ割り込みで 1 秒ごとに tick と出す (ステップ 2)
 *   3. hello を出す
 *   4. 打たれたキーをそのまま打ち返す (echo) を繰り返す
 * echo のループはタイマ割り込みに何度も割り込まれるが、
 * レジスタの退避と復元のおかげで何事もなく続きが動く。
 */

#include "uart.h"
#include "trap.h"

void kmain(void)
{
    uart_init();
    trap_init();  /* タイマ割り込みを開始 (1 秒ごとに tick と表示) */
    uart_puts("hello from KosabaOS (S-mode)\n");

    for (;;) {
        char c = uart_getc();  /* 1 文字届くまで待って受け取る */
        uart_putc(c);          /* そのまま打ち返す */
    }
}
