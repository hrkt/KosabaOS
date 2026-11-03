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
#include "vm.h"
#include "elf.h"
#include "fs.h"

#define TIMER_HZ        BOARD_TIMER_HZ  /* CLINT mtime の周波数 (ボード依存) */
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

#define FRAME_A1 9
#define FRAME_A2 10

#define SYS_PUTC 1
#define SYS_GETC 2
#define SYS_EXIT 3
#define SYS_OPEN 4
#define SYS_READ 5
#define SYS_CLOSE 6
#define SYS_READDIR 7
#define SYS_EXEC 8
#define SYS_CREATE 9
#define SYS_WRITE 10
#define SYS_TASKS 11

extern void user_exit_jump(void);  /* user.S: カーネルの文脈へ戻る */

static inline uint64_t r_sscratch(void)
{
    uint64_t x;
    asm volatile("csrr %0, sscratch" : "=r"(x));
    return x;
}

static inline void w_sscratch(uint64_t x)
{
    asm volatile("csrw sscratch, %0" : : "r"(x));
}

/* --- ユーザーメモリの読み書き ---------------------------------------
 * ポインタはユーザーの仮想アドレスで渡される。現在の割り当て
 * (vm.c の user_va_to_pa) で訳せる範囲だけを触る。 */

static int copy_user_string(uint64_t va, char *buf, int max)
{
    int i;
    for (i = 0; i < max - 1; i++) {
        uint64_t pa = user_va_to_pa(va + (uint64_t)i);
        char c;
        if (pa == 0) {
            return -1;
        }
        c = *(char *)pa;
        buf[i] = c;
        if (c == '\0') {
            return 0;
        }
    }
    buf[max - 1] = '\0';
    return 0;
}

static int copy_to_user(uint64_t va, const unsigned char *src, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t pa = user_va_to_pa(va + i);
        if (pa == 0) {
            return -1;
        }
        *(unsigned char *)pa = src[i];
    }
    return 0;
}

static int copy_from_user(unsigned char *dst, uint64_t va, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t pa = user_va_to_pa(va + i);
        if (pa == 0) {
            return -1;
        }
        dst[i] = *(unsigned char *)pa;
    }
    return 0;
}

/* --- ファイルディスクリプタ (ステップ 6) ------------------------------
 * 同時に動くユーザープログラムは 1 つなので、表は全体で 1 つ。
 * exec を呼ぶと全部閉じられる (仕様 v1 の決まり)。 */

#define MAX_FD 8
static struct {
    struct fs_file *file;
    uint64_t pos;
    int used;
    int writable;  /* 1 = create で開いた書き込み用 */
} fd_table[MAX_FD];

static void close_all_fds(void)
{
    int i;
    for (i = 0; i < MAX_FD; i++) {
        fd_table[i].used = 0;
    }
}

/* --- exec の親子 (ステップ 6) -----------------------------------------
 * 親が exec を呼んだら、親の trapframe・sepc・sp・領域の割り当てを
 * ここへ退避し、同じ仮想アドレスに子の領域を差し替えて子を
 * 起動する。子が exit / 保護違反で終わったら、退避した親の状態を
 * 現在の trapframe に書き戻し、親は exec から戻った形で再開する。 */

static struct {
    int active;
    uint64_t frame[32];
    uint64_t sepc;
    uint64_t sp;
    uint64_t code_pa;
    uint64_t data_pa;
    uint64_t stack_pa;
} exec_parent;

static void resume_parent(uint64_t *frame, uint64_t result)
{
    int i;
    for (i = 0; i < 32; i++) {
        frame[i] = exec_parent.frame[i];
    }
    frame[FRAME_A0] = result;
    w_sepc(exec_parent.sepc + 4);
    w_sscratch(exec_parent.sp);
    vm_switch_user_region(exec_parent.code_pa, exec_parent.data_pa,
                          exec_parent.stack_pa);
    exec_parent.active = 0;
    close_all_fds();
}

/* 戻り値: 1 = sepc は処理済みなので進めない、0 = 通常どおり進める */
static int do_exec(uint64_t *frame)
{
    char name[32];
    struct fs_file *f;
    uint64_t entry;
    int i;

    if (exec_parent.active) {
        frame[FRAME_A0] = (uint64_t)-1;  /* exec は 1 段まで */
        return 0;
    }
    if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0) {
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }
    f = fs_find(name);
    if (f == 0) {
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }
    entry = elf_load_to(f->data, CHILD_CODE_PA, CHILD_DATA_PA);
    if (entry == 0) {
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }

    /* 親の状態を退避する */
    for (i = 0; i < 32; i++) {
        exec_parent.frame[i] = frame[i];
    }
    exec_parent.sepc = r_sepc();
    exec_parent.sp = r_sscratch();
    exec_parent.code_pa = vm_current_code_pa();
    exec_parent.data_pa = vm_current_data_pa();
    exec_parent.stack_pa = vm_current_stack_pa();
    exec_parent.active = 1;
    close_all_fds();

    /* 子の領域に差し替えて、子の入口から開始させる。
     * レジスタは汚れたまま渡さないようゼロにしておく */
    vm_switch_user_region(CHILD_CODE_PA, CHILD_DATA_PA,
                          CHILD_STACK_PA);
    for (i = 0; i < 32; i++) {
        frame[i] = 0;
    }
    w_sepc(entry);
    w_sscratch(USER_STACK_TOP_VA);
    return 1;
}

static int do_syscall(uint64_t *frame)
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
        if (exec_parent.active) {
            resume_parent(frame, 0);  /* 親の exec へ戻る */
            return 1;
        }
        user_exit_jump();  /* ここからは戻ってこない */
        break;
    case SYS_OPEN: {
        char name[32];
        int i;
        struct fs_file *f;
        if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        f = fs_find(name);
        if (f == 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        for (i = 0; i < MAX_FD; i++) {
            if (!fd_table[i].used) {
                fd_table[i].file = f;
                fd_table[i].pos = 0;
                fd_table[i].used = 1;
                fd_table[i].writable = 0;
                frame[FRAME_A0] = (uint64_t)i;
                break;
            }
        }
        if (i == MAX_FD) {
            frame[FRAME_A0] = (uint64_t)-1;
        }
        break;
    }
    case SYS_READ: {
        uint64_t fd = frame[FRAME_A0];
        uint64_t max = frame[FRAME_A2];
        uint64_t remain, n;
        if (fd >= MAX_FD || !fd_table[fd].used || fd_table[fd].writable) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        remain = fd_table[fd].file->size - fd_table[fd].pos;
        n = max < remain ? max : remain;
        if (copy_to_user(frame[FRAME_A1],
                         fd_table[fd].file->data + fd_table[fd].pos,
                         n) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        fd_table[fd].pos += n;
        frame[FRAME_A0] = n;
        break;
    }
    case SYS_CLOSE: {
        uint64_t fd = frame[FRAME_A0];
        if (fd >= MAX_FD || !fd_table[fd].used) {
            frame[FRAME_A0] = (uint64_t)-1;
        } else {
            fd_table[fd].used = 0;
            frame[FRAME_A0] = 0;
        }
        break;
    }
    case SYS_READDIR: {
        struct fs_file *f = fs_at((int)frame[FRAME_A0]);
        char namebuf[32];
        int i;
        if (f == 0) {
            frame[FRAME_A0] = 0;
            break;
        }
        for (i = 0; i < 31 && f->name[i] != '\0'; i++) {
            namebuf[i] = f->name[i];
        }
        namebuf[i] = '\0';
        if (copy_to_user(frame[FRAME_A1],
                         (const unsigned char *)namebuf,
                         (uint64_t)i + 1) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        frame[FRAME_A0] = 1;
        break;
    }
    case SYS_EXEC:
        return do_exec(frame);
    case SYS_TASKS: {
        char kbuf[256];
        int n = proc_list(kbuf, sizeof(kbuf));
        if (copy_to_user(frame[FRAME_A0],
                         (const unsigned char *)kbuf,
                         (uint64_t)n) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
        } else {
            frame[FRAME_A0] = (uint64_t)n;
        }
        break;
    }
    case SYS_CREATE: {
        char name[32];
        int i;
        struct fs_file *f;
        if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        f = fs_create(name);
        if (f == 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        for (i = 0; i < MAX_FD; i++) {
            if (!fd_table[i].used) {
                fd_table[i].file = f;
                fd_table[i].pos = 0;
                fd_table[i].used = 1;
                fd_table[i].writable = 1;
                frame[FRAME_A0] = (uint64_t)i;
                break;
            }
        }
        if (i == MAX_FD) {
            frame[FRAME_A0] = (uint64_t)-1;
        }
        break;
    }
    case SYS_WRITE: {
        uint64_t fd = frame[FRAME_A0];
        uint64_t len = frame[FRAME_A2];
        unsigned char buf[256];
        uint64_t done = 0;
        if (fd >= MAX_FD || !fd_table[fd].used || !fd_table[fd].writable) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        /* ユーザーメモリから小分けに運んでファイルへ書く */
        while (done < len) {
            uint64_t chunk = len - done;
            uint64_t w;
            if (chunk > sizeof(buf)) {
                chunk = sizeof(buf);
            }
            if (copy_from_user(buf, frame[FRAME_A1] + done, chunk) != 0) {
                frame[FRAME_A0] = (uint64_t)-1;
                done = (uint64_t)-1;
                break;
            }
            w = fs_write_data(fd_table[fd].file, buf, chunk);
            done += w;
            if (w < chunk) {
                break;  /* 容量いっぱい */
            }
        }
        if (done != (uint64_t)-1) {
            frame[FRAME_A0] = done;
        }
        break;
    }
    default:
        uart_puts("trap: unknown syscall ");
        put_dec(num);
        uart_putc('\n');
        frame[FRAME_A0] = (uint64_t)-1;
        break;
    }

    /* ecall の次の命令から再開させる (sepc を 1 命令分進める) */
    w_sepc(r_sepc() + 4);
    return 0;
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
         * そのプログラムだけを終わらせる。exec の子なら
         * 親に戻して exec の結果として -2 を返す */
        uart_puts("trap: user program fault scause=");
        put_hex(scause);
        uart_puts(" sepc=");
        put_hex(r_sepc());
        uart_puts(" stval=");
        put_hex(r_stval());
        uart_putc('\n');
        if (exec_parent.active) {
            resume_parent(frame, (uint64_t)-2);
            return;
        }
        user_exit_jump();  /* ここからは戻ってこない */
    }

    /* カーネル自身の例外: 原因と現場を表示して止める。
     * K210 のドラフト仕様ではページフォルトが独立番号を持たず、
     * 一般のアクセスフォルト (1/5/7) として届く */
    if (code == 12 || code == 13 || code == 15
#if BOARD_DRAFT_FAULT_CODES
        || code == 1 || code == 5 || code == 7
#endif
        ) {
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
