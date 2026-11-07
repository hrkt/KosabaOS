/* uart.c (k210) -- UARTHS ドライバ (Kendryte K210)
 *
 * 共通の uart.h に対する K210 側の実装。QEMU virt の 16550 と違い、
 * K210 の UARTHS は SiFive 系のレジスタ配置:
 *   ベース 0x38000000
 *   +0x00 txdata  bit31 = 送信 FIFO が満杯
 *   +0x04 rxdata  bit31 = 受信 FIFO が空
 *   +0x08 txctrl  bit0 = 送信有効
 *   +0x0C rxctrl  bit0 = 受信有効
 *   +0x18 div     ボーレート分周 (下位 16 ビット) = CPU / baud - 1
 *
 * CH340 (USB シリアル) は IO4/IO5 に繋がっているので、FPIOA で
 * UARTHS を割り当ててから使う。分周の元になる CPU クロックは
 * ROM ブートローダが残した状態に依存するため、sysctl を読み戻して
 * 実周波数を計算する (step1 の実機検証で確定した方法:
 *  ROM が残すのは PLL0=780MHz、CPU=390MHz)。
 */

#include <stdint.h>
#include "uart.h"

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* FPIOA: ベース 0x502B0000、ピン n の設定レジスタは +4n。
 * 設定値の内訳 (SDK の fpioa 既定表が正本):
 *   RX = 機能18 + ie_en (bit20: 入力有効) + st (bit23: シュミット)
 *   TX = 機能19 + ds=0xF (bits[11:8]) + oe_en (bit12: 出力有効)
 * 注意: RX で ie_en を立て忘れると入力が常時 0 に見え、受信 FIFO に
 * 0x00 が湧き続けて getc が永遠にブロックしない (step2 で実踏み。
 * 0x00810012 は pu (bit16) との取り違えで ie_en が落ちていた)。 */
#define FPIOA_IO4 REG32(0x502B0010)  /* IO4 = UARTHS_RX */
#define FPIOA_IO5 REG32(0x502B0014)  /* IO5 = UARTHS_TX */
#define FPIOA_VAL_UARTHS_RX 0x00900012u
#define FPIOA_VAL_UARTHS_TX 0x00001F13u

/* UARTHS: ベース 0x38000000 */
#define UARTHS_TXDATA REG32(0x38000000)
#define UARTHS_RXDATA REG32(0x38000004)
#define UARTHS_TXCTRL REG32(0x38000008)
#define UARTHS_RXCTRL REG32(0x3800000C)
#define UARTHS_DIV    REG32(0x38000018)

/* SYSCTL: ベース 0x50440000 */
#define SYSCTL_PLL0     REG32(0x50440008)
#define SYSCTL_CLK_SEL0 REG32(0x50440020)

#define IN0_FREQ 26000000u  /* 入力水晶 26MHz */

/* sysctl の読み戻し値から CPU クロック (Hz) を計算する。
 * clk_sel0 の bit0 が 0 なら水晶そのまま、1 なら PLL0 を
 * 分周セレクタ (bits[2:1]) で割ったもの (step1 で実測確認)。
 * mtrap.c (タイマの分周計算) からも使うため board.h で公開。 */
unsigned long board_cpu_freq(void)
{
    uint32_t pll0 = SYSCTL_PLL0;
    uint32_t clk_sel0 = SYSCTL_CLK_SEL0;
    if ((clk_sel0 & 1u) == 0) {
        return IN0_FREQ;
    }
    uint32_t clkr = pll0 & 0xF;
    uint32_t clkf = (pll0 >> 4) & 0x3F;
    uint32_t clkod = (pll0 >> 10) & 0xF;
    uint32_t pll = IN0_FREQ / (clkr + 1);
    pll = pll * (clkf + 1) / (clkod + 1);
    uint32_t div_sel = (clk_sel0 >> 1) & 0x3;
    return pll / (2u << div_sel);
}

void uart_init(void)
{
    FPIOA_IO4 = FPIOA_VAL_UARTHS_RX;
    FPIOA_IO5 = FPIOA_VAL_UARTHS_TX;
    UARTHS_DIV = board_cpu_freq() / 115200 - 1;
    UARTHS_TXCTRL = 1;
    UARTHS_RXCTRL = 1;
}

void uart_putc(char c)
{
    while (UARTHS_TXDATA & 0x80000000u) {
        /* 送信 FIFO に空きができるまで待つ */
    }
    UARTHS_TXDATA = (uint32_t)(unsigned char)c;
}

char uart_getc(void)
{
    uint32_t v;
    do {
        v = UARTHS_RXDATA;
    } while (v & 0x80000000u);  /* 空の間は待つ */
    return (char)(v & 0xFFu);
}

/* 待たずに 1 文字受け取る。受信 FIFO が空なら -1。
 * 待ちの間に他のタスクを回したい場所 (スケジューラ段2) が
 * ポーリングで使う */
int uart_try_getc(void)
{
    uint32_t v = UARTHS_RXDATA;
    if (v & 0x80000000u) {
        return -1;
    }
    return (int)(v & 0xFFu);
}

void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s);
        s++;
    }
}
