#ifndef KOSABAOS_UART_H
#define KOSABAOS_UART_H

/* UART (シリアル通信デバイス) のドライバの公開部分。
 * 実装は uart.c。OS 本体はこの 4 つだけを知っていればよい。 */

void uart_init(void);        /* 通信の約束事を UART に書き込む */
void uart_putc(char c);      /* 1 文字送る (送れるまで待つ) */
char uart_getc(void);        /* 1 文字受け取る (届くまで待つ) */
int uart_try_getc(void);     /* 待たずに受け取る (無ければ -1) */
void uart_puts(const char *s); /* 文字列を送る */

#endif
