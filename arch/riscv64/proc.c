/* proc.c -- タスクとスケジューラ (ステップ 5)
 *
 * 「タスク」は実行の流れひとつ分で、それぞれ自分のスタックと
 * 文脈 (レジスタの組) を持つ。スケジューラはタスクを順番に
 * 選び、switch_context (switch.S) で文脈を交換して実行を移す。
 *
 * 切り替えのきっかけは 2 種類:
 *   協調的:     タスクが自分から yield() する
 *   プリエンプティブ: タイマ割り込み (100ms) が trap.c から
 *               schedule() を呼ぶ
 * どちらも行き着く先は schedule() で、仕組みは同じ。
 *
 * デモ (proc_demo、'm' キーで起動):
 *   第 1 幕 (協調): タスク A と B が 1 文字出すごとに yield
 *               → 出力は ABABAB... ときれいに交互になる
 *   第 2 幕 (プリエンプティブ): yield せず出し続ける
 *               → 100ms ごとに強制的に交代し、AAA...BBB... と
 *                 まとまりで交互になる
 * どちらの幕もタスクが有限回で終わり、全部終わると kmain の
 * echo に戻る。
 *
 * なおこの段階のタスクはカーネル内 (S モード) の流れで、
 * ページテーブルは全タスクで共有する。タスクごとの
 * ページテーブルはこの先の広がり (roadmap 参照)。
 */

#include <stdint.h>
#include "proc.h"
#include "uart.h"


#define NTASK 3          /* 0 = main (kmain)、1 = A、2 = B */
#define TASK_FREE 0
#define TASK_READY 1
#define TASK_DONE 2

struct task {
    struct context ctx;      /* 文脈 (switch.S で読み書きする) */
    int state;
    void (*fn)(void);        /* タスクの本体 */
    char letter;             /* デモで出す文字 */
    uint64_t trap_sepc;      /* トラップをまたぐ切り替え用 */
    uint64_t trap_sstatus;
};

static struct task tasks[NTASK];
static int current = 0;      /* 実行中のタスク番号 */
int sched_active = 0;        /* スケジューラ稼働中か (trap.c が見る) */
int preempt_enabled = 0;     /* タイマで強制的に切り替えるか */

static uint64_t stacks[2][4096] __attribute__((aligned(16)));

extern void switch_context(struct context *old, struct context *new);

/* --- スケジューラ ------------------------------------------------ */

/* 次に動かすタスクを選んで切り替える (ラウンドロビン) */
void schedule(void)
{
    int next = current;
    int i;

    for (i = 0; i < NTASK; i++) {
        next = (next + 1) % NTASK;
        if (next == current) {
            continue;
        }
        if (tasks[next].state == TASK_READY) {
            struct task *old = &tasks[current];
            current = next;
            switch_context(&old->ctx, &tasks[next].ctx);
            return;
        }
    }
    /* 切り替え相手がいない: そのまま戻る */
}

/* 通常の文脈では割り込み許可 (sstatus.SIE) が前提。
 * トラップ処理の最中から切り替わって戻ってきた場合、CPU の
 * SIE は落ちたままなので、通常文脈に戻る yield の出口と
 * タスクの入口 (トランポリン) で立て直す */
static inline void enable_interrupts(void)
{
    asm volatile("csrs sstatus, 0x2");
}

void yield(void)
{
    if (sched_active) {
        schedule();
        enable_interrupts();
    }
}

/* --- トラップをまたぐ状態 (trap.c から) --------------------------- */

void sched_save_trapstate(uint64_t sepc, uint64_t sstatus)
{
    tasks[current].trap_sepc = sepc;
    tasks[current].trap_sstatus = sstatus;
}

void sched_restore_trapstate(uint64_t *sepc, uint64_t *sstatus)
{
    *sepc = tasks[current].trap_sepc;
    *sstatus = tasks[current].trap_sstatus;
}

/* --- タスクの生成と起動 ------------------------------------------- */

/* 初めてのタスクは switch_context の ret でここへ着地する */
static void task_trampoline(void)
{
    void (*fn)(void) = tasks[current].fn;

    enable_interrupts();  /* 最初の入口: 割り込み許可で始める */

    fn();
    tasks[current].state = TASK_DONE;
    for (;;) {
        yield();  /* DONE なので二度と選ばれない。他に任せる */
    }
}

static void task_create(int idx, void (*fn)(void), char letter)
{
    uint64_t top = (uint64_t)&stacks[idx - 1][4096];

    tasks[idx].state = TASK_READY;
    tasks[idx].fn = fn;
    tasks[idx].letter = letter;
    tasks[idx].ctx.ra = (uint64_t)task_trampoline;
    tasks[idx].ctx.sp = top;
}

/* 全タスクが DONE になるまで、自分も yield し続けて待つ */
static void run_until_done(void)
{
    tasks[0].state = TASK_READY;
    current = 0;
    sched_active = 1;
    while (tasks[1].state != TASK_DONE || tasks[2].state != TASK_DONE) {
        yield();
    }
    sched_active = 0;
}

/* --- デモのタスク本体 ---------------------------------------------- */

static uint64_t r_time(void)
{
    uint64_t x;
    asm volatile("csrr %0, time" : "=r"(x));
    return x;
}

/* QEMU virt のタイマは 10MHz: 1ms = 10000 カウント */
static void delay_ms(int ms)
{
    uint64_t until = r_time() + (uint64_t)ms * 10000;
    while (r_time() < until) {
    }
}

static void body_cooperative(void)
{
    char me = tasks[current].letter;
    int i;

    for (i = 0; i < 16; i++) {
        uart_putc(me);
        delay_ms(40);
        yield();
    }
}

static void body_preemptive(void)
{
    char me = tasks[current].letter;
    int i;

    for (i = 0; i < 30; i++) {
        uart_putc(me);
        delay_ms(12);
    }
}

void proc_demo(void)
{
    uart_puts("\n[proc] cooperative (yield after each letter):\n");
    preempt_enabled = 0;
    task_create(1, body_cooperative, 'A');
    task_create(2, body_cooperative, 'B');
    run_until_done();
    uart_puts("\n[proc] preemptive (timer switches every 100ms):\n");
    preempt_enabled = 1;
    task_create(1, body_preemptive, 'A');
    task_create(2, body_preemptive, 'B');
    run_until_done();
    preempt_enabled = 0;
    uart_puts("\n[proc] all tasks done\n");
}

/* タスク一覧をテキストで書く (ps コマンド / SYS_TASKS 用)。
 * 戻り値は書いたバイト数 */
int proc_list(char *buf, unsigned long max)
{
    static const char *names[] = { "free", "ready", "done" };
    unsigned long n = 0;
    int i;

    for (i = 0; i < NTASK; i++) {
        const char *s = "task ";
        const char *st = names[tasks[i].state];
        while (*s != '\0' && n < max) {
            buf[n++] = *s++;
        }
        if (n < max) {
            buf[n++] = (char)('0' + i);
        }
        if (n < max) {
            buf[n++] = ':';
        }
        if (n < max) {
            buf[n++] = ' ';
        }
        while (*st != '\0' && n < max) {
            buf[n++] = *st++;
        }
        if (n < max) {
            buf[n++] = '\n';
        }
    }
    return (int)n;
}
