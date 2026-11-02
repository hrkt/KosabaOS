/* mtrap.c -- M モードのトラップとタイマ割り込み (k210)
 *
 * virt (S モード) ではタイマ設定を SBI に頼むが、K210 では
 * カーネルが M モードにいるので、CLINT を直接触れる:
 *   CLINT ベース 0x02000000
 *   mtime    (0x0200BFF8)  常に進み続けるカウンタ (読み専用扱い)
 *   mtimecmp (0x02004000)  hart0 用の「期限」。mtime がここに
 *                          達すると M タイマ割り込みが上がる
 * mtime の進む速さは実測で CPU クロックの 1/50 だった
 * (CPU 390MHz のとき約 7.8MHz。spike-k210/step3-timer で、
 *  115200 baud で 11520 文字を送る約 1 秒間に 7,799,270 進んだ)。
 *
 * 割り込みの間隔と考え方は virt の trap.c と揃える: 100ms ごとに
 * 割り込み、10 回に 1 回だけ "tick N" (N は秒数) と出す。
 */

#include <stdint.h>
#include "board.h"
#include "uart.h"
#include "lcd.h"

#define REG64(addr) (*(volatile uint64_t *)(addr))
#define CLINT_MTIME    REG64(0x0200BFF8)
#define CLINT_MTIMECMP REG64(0x02004000)

static uint64_t tick_interval;  /* 100ms 分の mtime の進み */
static int tick_divider;        /* 10 回に 1 回出すための数え */
static uint64_t tick_seconds;   /* 表示用の秒数 */

extern void mtrap_entry(void);  /* mtrap.S の入口 */

static inline uint64_t r_mcause(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, mcause" : "=r"(v));
    return v;
}

static inline uint64_t r_mepc(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, mepc" : "=r"(v));
    return v;
}

static void put_dec(uint64_t v)
{
    char buf[24];
    int n = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v > 0) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        uart_putc(buf[--n]);
    }
}

static void put_hex(uint64_t v)
{
    uart_puts("0x");
    for (int i = 15; i >= 0; i--) {
        int d = (int)((v >> (i * 4)) & 0xF);
        uart_putc(d < 10 ? (char)('0' + d) : (char)('a' + (d - 10)));
    }
}

/* mtrap.S から呼ばれる。割り込みなら期限を延ばして tick を数え、
 * 例外 (想定外) なら原因を出して止める (事故止めは start.S の
 * mtvec から、初期化後はこちらの入口に差し替わる)。 */
void mtrap_handler(void)
{
    uint64_t cause = r_mcause();
    if ((int64_t)cause < 0 && (cause & 0xFF) == 7) {
        /* M タイマ割り込み: 次の期限を予約し直す */
        CLINT_MTIMECMP = CLINT_MTIMECMP + tick_interval;
        if (++tick_divider >= 10) {
            tick_divider = 0;
            tick_seconds++;
            uart_puts("tick ");
            put_dec(tick_seconds);
            uart_putc('\n');
            lcd_tick_draw(tick_seconds);  /* LCD の tick 表示も進める */
        }
        return;
    }
    uart_puts("\nmtrap: unexpected trap mcause=");
    put_hex(cause);
    uart_puts(" mepc=");
    put_hex(r_mepc());
    uart_putc('\n');
    for (;;) {
        __asm__ volatile("wfi");
    }
}

void mtrap_init(void)
{
    /* mtime は CPU の 1/50 で進む (実測)。100ms = その 1/10 */
    tick_interval = board_cpu_freq() / 50 / 10;
    tick_divider = 0;
    tick_seconds = 0;

    __asm__ volatile("csrw mtvec, %0" :: "r"((uint64_t)mtrap_entry));
    CLINT_MTIMECMP = CLINT_MTIME + tick_interval;
    __asm__ volatile("csrs mie, %0" :: "r"(0x80));     /* MTIE: タイマ許可 */
    __asm__ volatile("csrs mstatus, %0" :: "r"(0x8));  /* MIE: 全体許可 */
}
