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
 *
 * スケジューラ段1 (sleep、doc/scheduler-plan.md):
 *   タスクが「◯ミリ秒寝る」と言える口 (task_sleep_ms) を足した。
 *   寝ている間は TASK_SLEEPING で、起床時刻 (wake_at、time CSR
 *   の生カウントで持つ) が来たら schedule() の入口の起床判定
 *   (sched_wake_due) が READY に戻す。起こす側は trap.c の
 *   タイマ割り込みと、入力待ちのポーリングからの schedule()
 *   呼び出し。段2 で sched_start() が時計タスクを常駐させて
 *   からは、スケジューラはずっと生きたままになる。
 *
 * スケジューラ段2 (時計タスクの常駐):
 *   ファイル末尾の clock_body / sched_start を参照。表示の
 *   更新は割り込み文脈ではなく、このタスクの仕事。
 */

#include <stdint.h>
#include "proc.h"
#include "uart.h"
#include "board.h"  /* BOARD_TIMER_HZ (sleep の時刻換算) */
#include "klog.h"   /* 時計タスクの起床記録 (lkputs、段2) */
#if BOARD_FEATURE_LCD
#include "lcd.h"    /* 時計タスクの表示先 (k210 のみ、段2) */
#endif


#define NTASK 4          /* 0 = main (kmain)、1 = A、2 = B、3 = 時計 */
#define TASK_FREE 0
#define TASK_READY 1
#define TASK_DONE 2
#define TASK_SLEEPING 3  /* 起床時刻まで寝ている (段1) */

struct task {
    struct context ctx;      /* 文脈 (switch.S で読み書きする) */
    int state;
    void (*fn)(void);        /* タスクの本体 */
    char letter;             /* デモで出す文字 */
    uint64_t trap_sepc;      /* トラップをまたぐ切り替え用 */
    uint64_t trap_sstatus;
    uint64_t wake_at;        /* SLEEPING の起床時刻 (time CSR) */
    uint64_t wakes;          /* sleep から起きた回数 (段1) */
    char cwd[PROC_CWD_MAX];  /* 正規化済み絶対パス (段4) */
};

static struct task tasks[NTASK];
static int current = 0;      /* 実行中のタスク番号 */
int sched_active = 0;        /* スケジューラ稼働中か (trap.c が見る) */
int preempt_enabled = 0;     /* タイマで強制的に切り替えるか */

static uint64_t stacks[3][4096] __attribute__((aligned(16)));

extern void switch_context(struct context *old, struct context *new);

/* --- スケジューラ ------------------------------------------------ */

/* 生の時刻 (time CSR)。ボードによらず常に進み続けるので、
 * sleep の物差しは trap.c の tick (割り込みが滞ると止まる)
 * ではなくこちらで測る。K210 では M シムが rdtime を
 * エミュレーションする (mshim.c) */
static uint64_t r_time(void)
{
    uint64_t x;
    asm volatile("csrr %0, time" : "=r"(x));
    return x;
}

/* 起床判定: 寝ていて起床時刻が来たタスクを READY に戻す。
 * 戻り値は起こした数。schedule() の入口と、trap.c のタイマ
 * 割り込み (起床駆動の切り替えの判断) から呼ばれる */
int sched_wake_due(void)
{
    uint64_t now = r_time();
    int woke = 0;
    int i;

    for (i = 0; i < NTASK; i++) {
        if (tasks[i].state == TASK_SLEEPING &&
            tasks[i].wake_at <= now) {
            tasks[i].state = TASK_READY;
            woke++;
        }
    }
    return woke;
}

/* 次に動かすタスクを選んで切り替える (ラウンドロビン) */
void schedule(void)
{
    int next = current;
    int i;

    sched_wake_due();  /* 寝ているタスクの起床を先に反映する */

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

extern void trap_entry(void);  /* trap.S: カーネル側のトラップ入口 */

/* 通常の文脈では割り込み許可 (sstatus.SIE) が前提。
 * トラップ処理の最中から切り替わって戻ってきた場合、CPU の
 * SIE は落ちたままなので、通常文脈に戻る yield の出口と
 * タスクの入口 (トランポリン) で立て直す。
 *
 * ただし立て直してよいのは、トラップの入口がカーネル側
 * (trap_entry) のときだけ。ユーザープログラムの実行中は
 * stvec が trap_entry_user で、sscratch はトラップ専用
 * スタックではなくユーザーの sp を保持している。そういう
 * 文脈 (SYS_GETC の中から初めて切り替わったタスクなど) で
 * 割り込みを入れると、次の割り込みが入口で sp を取り違え、
 * ユーザーのスタックへ退避を書いて壊れる (段2 で実踏み。
 * test-w の初期版で Nish が落ちた)。入口がユーザー側の間は
 * 割り込みを入れず、ポーリングで回す */
static inline void enable_interrupts(void)
{
    uint64_t vec;
    asm volatile("csrr %0, stvec" : "=r"(vec));
    if (vec == (uint64_t)trap_entry) {
        asm volatile("csrs sstatus, 0x2");
    }
}

void yield(void)
{
    if (sched_active) {
        schedule();
        enable_interrupts();
    }
}

/* 待ちのポーリングからスケジューラを 1 回回す。yield() と違い
 * 割り込み状態には触らないので、トラップ文脈 (SYS_GETC の中
 * など、割り込みを入れてはいけない場所) からも呼べる */
void sched_poll(void)
{
    if (sched_active) {
        schedule();
    }
}

/* 現在のタスクを ms ミリ秒寝かせる (段1 の sleep)。
 * 寝ている間は TASK_SLEEPING で、他の READY なタスクが進む。
 * 誰も動けないときは、起床時刻が来るまで schedule() を回し
 * 続ける (この OS の待ちはポーリングが流儀なので、wfi で
 * CPU を止めることはしない)。起きたら wakes を 1 つ増やす */
void task_sleep_ms(uint64_t ms)
{
    uint64_t wake = r_time() + ms * (BOARD_TIMER_HZ / 1000);

    tasks[current].wake_at = wake;
    tasks[current].state = TASK_SLEEPING;
    while (r_time() < wake) {
        schedule();
    }
    tasks[current].state = TASK_READY;
    tasks[current].wakes++;
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

/* --- cwd (タスクごと) ---------------------------------------------- */

/* 未設定 (BSS のゼロ) ならルートとして扱う */
const char *proc_cwd(void)
{
    if (tasks[current].cwd[0] == '\0') {
        return "/";
    }
    return tasks[current].cwd;
}

void proc_set_cwd(const char *abs)
{
    int i = 0;
    while (abs[i] != '\0' && i < PROC_CWD_MAX - 1) {
        tasks[current].cwd[i] = abs[i];
        i++;
    }
    tasks[current].cwd[i] = '\0';
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

/* 全タスクが DONE になるまで、自分も yield し続けて待つ。
 * スケジューラは sched_start() (段2) 以後ずっと生きている
 * ので、ここでは出し入れしない */
static void run_until_done(void)
{
    tasks[0].state = TASK_READY;
    current = 0;
    while (tasks[1].state != TASK_DONE || tasks[2].state != TASK_DONE) {
        yield();
    }
}

/* --- デモのタスク本体 ---------------------------------------------- */

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
    static const char *names[] = { "free", "ready", "done", "sleeping" };
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
        /* sleep から起きた回数 (段1)。寝たことのあるタスク
         * だけに付ける (デモのタスクの行は従来どおり) */
        if (tasks[i].wakes > 0) {
            const char *w = " wakes=";
            uint64_t v = tasks[i].wakes;
            char rev[20];
            int m = 0;
            while (*w != '\0' && n < max) {
                buf[n++] = *w++;
            }
            while (v > 0) {
                rev[m++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (m > 0 && n < max) {
                buf[n++] = rev[--m];
            }
        }
        if (n < max) {
            buf[n++] = '\n';
        }
    }
    return (int)n;
}

/* --- 時計タスク (段2: 部分プロセス第 1 号) --------------------------
 *
 * スケジューラ段2 (doc/scheduler-plan.md)。OS の一部 (時刻
 * 表示) を、独立した常駐カーネルタスクとして動かす。
 * 置き時計は最初カーネルの描画経路 (K210 では M シムの
 * 割り込み文脈) で達成したが、その形では「入力待ちで
 * 割り込みが戻らない間は表示が止まる」弱点があった。
 * ここでは表示の更新を、sleep して起きるタスクの仕事に
 * 移す。入力待ちの側 (SYS_GETC とカーネルコンソール) が
 * 待ちの間にスケジューラを回すので、待ちの間も秒が進む。 */

#define CLOCK_TASK 3   /* タスク表の時計の席 */

static void clock_body(void)
{
    uint64_t start = r_time();

    for (;;) {
        task_sleep_ms(1000);
        /* 起きたことはログにだけ残す (コンソールには出さない)。
         * virt ではこれが観測手段になり、実機でも dmesg と
         * ps (wakes) で「待ちの間も起きていた」ことが追える */
        lkputs("clock: wake ");
        lkput_dec(tasks[current].wakes);
        lkputc('\n');
#if BOARD_FEATURE_LCD
        /* 表示する秒数は生の時刻 (time CSR) から数える。
         * trap.c の tick は入力待ちで割り込みが滞ると一緒に
         * 滞るが、こちらは滞らない。壁時計が有効な間は
         * lcd 側が RTC を読んで HH:MM:SS を描くので、この
         * 秒数は RTC 未設定のときの表示にだけ使われる */
        lcd_tick_draw((r_time() - start) / BOARD_TIMER_HZ);
#endif
    }
}

/* スケジューラの常駐化 (段2): 時計タスクを作り、以後ずっと
 * スケジューラを生かしておく。kmain が初期化の途中で
 * 1 回だけ呼ぶ */
void sched_start(void)
{
    tasks[0].state = TASK_READY;
    current = 0;
    task_create(CLOCK_TASK, clock_body, 0);
    sched_active = 1;
}
