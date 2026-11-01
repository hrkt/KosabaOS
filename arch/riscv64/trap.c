/* trap.c -- トラップ (例外・割り込み) の処理 (riscv64)
 *
 * 流れ: 割り込みや例外が起きる → CPU が stvec の trap_entry
 * (trap.S) に飛ぶ → レジスタを退避して trap_handler (ここ) を
 * 呼ぶ → 戻ると復元されて sret で元の処理に帰る。
 *
 * 今回扱うのはタイマ割り込みだけ。QEMU virt のタイマは
 * 10MHz (1 秒 = 10,000,000 カウント) で進む。S モードからは
 * タイマの予約を OpenSBI の Timer 拡張 (sbi_set_timer) に頼む:
 * 指定した時刻になると、S モードへタイマ割り込みが届く。
 * 時刻の読み取りは time レジスタ (CSR) から。
 *
 * 例外 (想定外のトラップ) は、原因を表示して止める。次の
 * ステップ (ページング) ではページフォルトがここに届く。
 */

#include <stdint.h>
#include "trap.h"
#include "uart.h"

#define TIMER_HZ        10000000ULL  /* QEMU virt のタイマ周波数 */
#define TIMER_INTERVAL  TIMER_HZ     /* 1 秒ごとに割り込む */

#define SIE_STIE   (1ULL << 5)  /* sie のタイマ割り込み許可ビット */
#define SSTATUS_SIE (1ULL << 1) /* sstatus の割り込み許可ビット */

/* --- CSR の読み書き (インラインアセンブラ) --------------------------
 * CSR は汎用レジスタとは別枠の、CPU の状態・設定レジスタ群。
 * C からは直接触れないので、csrr/csrw/csrs 命令を 1 行だけ
 * アセンブラで書く。 */

static inline uint64_t r_scause(void)
{
    uint64_t x;
    asm volatile("csrr %0, scause" : "=r"(x));
    return x;
}

static inline uint64_t r_sepc(void)
{
    uint64_t x;
    asm volatile("csrr %0, sepc" : "=r"(x));
    return x;
}

static inline uint64_t r_stval(void)
{
    uint64_t x;
    asm volatile("csrr %0, stval" : "=r"(x));
    return x;
}

static inline uint64_t r_time(void)
{
    uint64_t x;
    asm volatile("csrr %0, time" : "=r"(x));
    return x;
}

static inline void w_stvec(uint64_t x)
{
    asm volatile("csrw stvec, %0" : : "r"(x));
}

static inline void s_sie(uint64_t bits)
{
    asm volatile("csrs sie, %0" : : "r"(bits));
}

static inline void s_sstatus(uint64_t bits)
{
    asm volatile("csrs sstatus, %0" : : "r"(bits));
}

/* OpenSBI の Timer 拡張: 「この時刻になったらタイマ割り込みを
 * ください」とファームウェアに頼む。ecall の約束は a7 = 拡張の
 * ID、a6 = その中の機能の番号 (set_timer は 0)、a0 = 引数。
 * 結果は a0 にエラーコードが返る (0 = 成功)。
 * ID 0x54494D45 は "TIME" の文字コード。 */
#define SBI_EID_TIME 0x54494D45ULL

static long sbi_set_timer(uint64_t when)
{
    register uint64_t a0 asm("a0") = when;
    register uint64_t a6 asm("a6") = 0;
    register uint64_t a7 asm("a7") = SBI_EID_TIME;
    asm volatile("ecall" : "+r"(a0) : "r"(a6), "r"(a7) : "memory");
    return (long)a0;
}

/* --- 小さな表示部品 ------------------------------------------------ */

static void put_hex(uint64_t x)
{
    int i;
    uart_puts("0x");
    for (i = 60; i >= 0; i -= 4) {
        int d = (int)((x >> i) & 0xF);
        uart_putc((char)(d < 10 ? '0' + d : 'a' + (d - 10)));
    }
}

static void put_dec(uint64_t x)
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

/* --- タイマ -------------------------------------------------------- */

static uint64_t next_deadline;  /* 次の割り込みの時刻 */
static uint64_t ticks;          /* 割り込みの回数 */

void trap_init(void)
{
    extern void trap_entry(void);  /* trap.S の入口 */

    w_stvec((uint64_t)trap_entry); /* トラップの飛び先を登録
                                    * (住所の下 2 ビット 0 = 全てここへ) */
    s_sie(SIE_STIE);               /* タイマ割り込みを許可 (sie) */
    s_sstatus(SSTATUS_SIE);        /* 全体の割り込み許可 (sstatus) */

    next_deadline = r_time() + TIMER_INTERVAL;
    if (sbi_set_timer(next_deadline) != 0) {  /* 最初の 1 回を予約 */
        uart_puts("trap: sbi_set_timer failed\n");
    }
}

/* --- ハンドラ本体 (trap.S から呼ばれる) ----------------------------- */

void trap_handler(void)
{
    uint64_t scause = r_scause();
    int is_interrupt = (int)(scause >> 63);
    uint64_t code = scause & 0xFF;

    if (is_interrupt && code == 5) {
        /* スーパーバイザ・タイマ割り込み */
        ticks++;
        uart_puts("tick ");
        put_dec(ticks);
        uart_putc('\n');
        next_deadline += TIMER_INTERVAL;
        sbi_set_timer(next_deadline);
        return;
    }

    if (is_interrupt) {
        uart_puts("trap: unexpected interrupt, code=");
        put_dec(code);
        uart_putc('\n');
        return;
    }

    /* 例外: 原因と現場を表示して止める */
    if (code == 12 || code == 13 || code == 15) {
        uart_puts("trap: page fault (code=");
        put_dec(code);
        uart_puts(") ");
    }
    uart_puts("trap: exception scause=");
    put_hex(scause);
    uart_puts(" sepc=");
    put_hex(r_sepc());
    uart_puts(" stval=");
    put_hex(r_stval());
    uart_putc('\n');
    for (;;) {
        asm volatile("wfi");
    }
}
