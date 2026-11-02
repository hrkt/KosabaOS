/* uart.c -- NS16550A UART ドライバ (riscv64 / QEMU virt)
 *
 * デバイスはメモリと同じ住所空間に住んでいる (MMIO)。QEMU virt では
 * UART の先頭アドレス (ベース) が 0x10000000 で、機能ごとの窓口
 * (レジスタ) がそこからの距離 (オフセット) で並んでいる:
 *   +0  THR  送信用 (書くと 1 文字送られる) ※受信 RBR と同じ住所
 *   +1  IER  割り込みを有効にする設定
 *   +2  FCR  FIFO (送受信の待ち行列) の設定 ※書く専用
 *   +3  LCR  通信の形式設定。「分周比を書くモード」への切替ビットもここ
 *   +5  LSR  状態表示。bit5 (0x20) = 送信窓口が空いた、bit0 = 受信あり
 * RAM と違い、値を置くこと自体がデバイスへの命令になる。
 * ベースアドレスはボードごとに違う (これは QEMU virt の値)。
 */

#include "uart.h"

#define UART_BASE 0x10000000UL

/* REG(オフセット) で UART の窓口 1 つを読み書きできる。
 * volatile が肝心: 「この読み書きは毎回本当にやること。最適化で
 * 省いたりまとめたりしないで」とコンパイラに伝える指定。
 * これが無いと、状態待ちのループ読みが「どうせ変わらない」と
 * 判断されて消え、永遠に待つ (または待たない) コードになりうる。 */
#define REG(off) (*(volatile unsigned char *)(UART_BASE + (off)))

#define LSR_DATA_READY 0x01  /* LSR bit0: 受信データあり */
#define LSR_THR_EMPTY  0x20  /* LSR bit5: 送信窓口が空いた */

/* 電源投入直後の UART は設定が定まっていないので、通信の約束を
 * 書き込んでから使う。順番は 16550 系の定石どおり。 */
void uart_init(void)
{
    REG(1) = 0x00;  /* IER = 0: 割り込みは使わない (今は自力で待つ) */
    REG(3) = 0x80;  /* LCR: 分周比を書くモードに切り替える */
    REG(0) = 0x03;  /* 分周比の下半分 = 3 (実機の標準クロックでは */
    REG(1) = 0x00;  /*   38400 bps になる値。上半分は 0) */
    REG(3) = 0x03;  /* LCR = 8 ビット・パリティなし・ストップ 1 (8N1)
                     * に戻すと分周比はロックされる */
    REG(2) = 0x07;  /* FCR: FIFO を有効にして中身を空にする */
}

void uart_putc(char c)
{
    while ((REG(5) & LSR_THR_EMPTY) == 0) {
        /* 送信窓口が空くまで待つ */
    }
    REG(0) = (unsigned char)c;  /* THR へ 1 文字置く = 送信される */
}

char uart_getc(void)
{
    while ((REG(5) & LSR_DATA_READY) == 0) {
        /* 1 文字届くまで待つ */
    }
    return (char)REG(0);  /* RBR を読む = 1 文字受け取る。
                           * 読むと「受信データあり」は自動で消える */
}

void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s);
        s++;
    }
}
