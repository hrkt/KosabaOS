/* trap.c -- トラップ (例外・割り込み) の処理 (riscv64)
 *
 * 流れ: 割り込みや例外が起きる → CPU が stvec の trap_entry
 * (trap.S) に飛ぶ → レジスタを退避して trap_handler (ここ) を
 * 呼ぶ → 戻ると復元されて sret で元の処理に帰る。
 *
 * QEMU virt のタイマは 10MHz (1 秒 = 10,000,000 カウント) で
 * 進む。S モードからはタイマの予約を OpenSBI の Timer 拡張
 * (sbi_set_timer) に頼む: 指定した時刻になると、S モードへ
 * タイマ割り込みが届く。時刻の読み取りは time レジスタ (CSR)。
 *
 * 扱うもの: タイマ割り込み (100ms ごと。tick 表示と、ステップ 5
 * のプリエンプティブなタスク切り替え)、ユーザーモードからの
 * システムコール (ステップ 4)、例外 (カーネルのものは原因を
 * 表示して停止、ユーザーのものはそのプログラムだけを終了)。
 */

#include <stdint.h>
#include "trap.h"
#include "uart.h"
#include "proc.h"

#define TIMER_HZ        10000000ULL  /* QEMU virt のタイマ周波数 */
/* 割り込み自体は 100ms ごと (ステップ 5 のスケジューラの切り替え
 * 間隔)。tick の表示は 10 回に 1 回にして 1 秒に 1 行を保つ */
#define TIMER_INTERVAL  (TIMER_HZ / 10)
static int tick_divider = 0;

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

static inline void w_sepc(uint64_t x)
{
    asm volatile("csrw sepc, %0" : : "r"(x));
}

static inline uint64_t r_sstatus(void)
{
    uint64_t x;
    asm volatile("csrr %0, sstatus" : "=r"(x));
    return x;
}

static inline void w_sstatus(uint64_t x)
{
    asm volatile("csrw sstatus, %0" : : "r"(x));
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

/* --- システムコール (ステップ 4) -------------------------------------
 * 仕様は doc/syscalls.md。frame は trap.S が退避したレジスタの
 * 配列で、並びは退避の順番: [8] = a0 (引数・戻り値)、[16] = a7
 * (番号)。戻り値を返したいときは frame[8] を書き換えると、
 * 復元されてユーザープログラムの a0 に入る。 */

/* 退避順は ra, gp, tp, t0, t1, t2, s0, s1, a0, a1, ..., a7, s2, ...
 * なので a0 が 8 番目、a7 が 15 番目 */
#define FRAME_A0 8
#define FRAME_A7 15

#define SYS_PUTC 1
#define SYS_GETC 2
#define SYS_EXIT 3

extern void user_exit_jump(void);  /* user.S: カーネルの文脈へ戻る */

static void do_syscall(uint64_t *frame)
{
    uint64_t num = frame[FRAME_A7];

    switch (num) {
    case SYS_PUTC:
        uart_putc((char)frame[FRAME_A0]);
        frame[FRAME_A0] = 0;
        break;
    case SYS_GETC:
        frame[FRAME_A0] = (uint64_t)uart_getc();
        break;
    case SYS_EXIT:
        user_exit_jump();  /* ここからは戻ってこない */
        break;
    default:
        uart_puts("trap: unknown syscall ");
        put_dec(num);
        uart_putc('\n');
        frame[FRAME_A0] = (uint64_t)-1;
        break;
    }

    /* ecall の次の命令から再開させる (sepc を 1 命令分進める) */
    w_sepc(r_sepc() + 4);
}

/* --- ハンドラ本体 (trap.S から呼ばれる) -------------------------------
 * frame: 退避したレジスタ一式。from_user: ユーザーモードからの
 * トラップなら 1、カーネル自身なら 0。 */

void trap_handler(uint64_t *frame, int from_user)
{
    uint64_t scause = r_scause();
    int is_interrupt = (int)(scause >> 63);
    uint64_t code = scause & 0xFF;

    /* スケジューリング中に割り込まれたら、このタスクの
     * sepc / sstatus を退避しておく (CSR は CPU に 1 組だけ
     * なので、切り替え先のタスクのトラップで上書きされる前に) */
    if (sched_active) {
        sched_save_trapstate(r_sepc(), r_sstatus());
    }

    if (is_interrupt && code == 5) {
        /* スーパーバイザ・タイマ割り込み (100ms ごと) */
        next_deadline += TIMER_INTERVAL;
        sbi_set_timer(next_deadline);
        if (++tick_divider >= 10) {
            tick_divider = 0;
            ticks++;
            uart_puts("tick ");
            put_dec(ticks);
            uart_putc('\n');
        }
        if (sched_active && preempt_enabled) {
            /* プリエンプティブな切り替え (ステップ 5)。
             * 切り替わった先のタスクの文脈でこのハンドラに
             * 戻ってくることがあるので、帰る前に「今の」タスクの
             * sepc / sstatus を CSR に戻す */
            uint64_t e, s;
            schedule();
            sched_restore_trapstate(&e, &s);
            w_sepc(e);
            w_sstatus(s);
        }
        return;
    }

    if (is_interrupt) {
        uart_puts("trap: unexpected interrupt, code=");
        put_dec(code);
        uart_putc('\n');
        return;
    }

    /* ここから例外 */
    if (from_user && code == 8) {
        /* ユーザーモードからの ecall = システムコール */
        do_syscall(frame);
        return;
    }

    if (from_user) {
        /* ユーザープログラムの例外 (メモリ保護違反など):
         * そのプログラムだけを終わらせてカーネルに帰る */
        uart_puts("trap: user program fault scause=");
        put_hex(scause);
        uart_puts(" sepc=");
        put_hex(r_sepc());
        uart_puts(" stval=");
        put_hex(r_stval());
        uart_putc('\n');
        user_exit_jump();  /* ここからは戻ってこない */
    }

    /* カーネル自身の例外: 原因と現場を表示して止める */
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
