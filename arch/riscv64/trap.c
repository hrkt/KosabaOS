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
#include "fat.h"

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

/* fd テーブル (下の方で定義) の初期化: 根の段の表を指させる */
static void fd_table_init(void);

void trap_init(void)
{
    extern void trap_entry(void);  /* trap.S の入口 */

    w_stvec((uint64_t)trap_entry); /* トラップの飛び先を登録
                                    * (住所の下 2 ビット 0 = 全てここへ) */
    s_sie(SIE_STIE);               /* タイマ割り込みを許可 (sie) */
    s_sstatus(SSTATUS_SIE);        /* 全体の割り込み許可 (sstatus) */

    fd_table_init();  /* fd_table を根のプログラムの表に */

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
#define SYS_UNLINK 12
#define SYS_MKDIR 13
#define SYS_RMDIR 14
#define SYS_CHDIR 15
#define SYS_GETCWD 16

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
 * fd テーブルは exec の段ごとに持つ (exec 多段化、doc/
 * exec-multilevel.md)。子は空のテーブルで始まり、親のテーブルは
 * 親の段にそのまま残るので、子が終わっても親が開いたファイルは
 * 生きている。fd_table は「今の段」のテーブルを指すポインタで、
 * exec と子の終了 (resume_parent) のたびに切り替わる。 */

#define MAX_FD 8
struct fd_entry {
    struct fs_file *file;  /* RAM FS のファイル (is_sd でないとき) */
    struct fat_file fat;   /* SD 側のファイル (is_sd のとき) */
    uint64_t pos;
    int used;
    int writable;  /* 1 = create で開いた書き込み用 */
    int is_sd;     /* 1 = /sd の向こう (FAT) のファイル */
};

static struct fd_entry fd_tables[EXEC_MAX_DEPTH + 1][MAX_FD];
static struct fd_entry *fd_table;  /* trap_init で fd_tables[0] を指す */

static void fd_table_init(void)
{
    fd_table = fd_tables[0];
}

/* パスが固定マウント /sd の配下かどうか (doc/sd-design.md) */
static int is_sd_path(const char *name)
{
    return name[0] == '/' && name[1] == 's' && name[2] == 'd' &&
           name[3] == '/';
}

/* --- パスの解決と正規化 (FS 再設計 段4) -------------------------------
 * ユーザーから渡されたパス (相対でも絶対でも) を、現在の
 * タスクの cwd から解決して正規化済みの絶対パスにする。
 * '//' と '.' は畳み、'..' は親へ (ルートの親はルート)。
 * 要素 1 つの上限は 31 文字、パス全体の上限は PATH_MAX。
 * 失敗したら -1 (長すぎる、要素が長すぎる)。 */

#define PATH_MAX 128

static int str_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == b[i];
}

static int resolve_path(const char *in, char *out)
{
    char combined[PATH_MAX * 2 + 2];
    const char *comp[PATH_MAX / 2];
    int lens[PATH_MAX / 2];
    int ncomp = 0;
    int i = 0, n = 0;
    const char *cwd = proc_cwd();

    /* 相対パスは cwd を前に付けて 1 本にする */
    if (in[0] != '/') {
        while (cwd[i] != '\0' && i < (int)sizeof(combined) - 1) {
            combined[i] = cwd[i];
            i++;
        }
        if (i < (int)sizeof(combined) - 1) {
            combined[i++] = '/';
        }
    }
    {
        int j = 0;
        while (in[j] != '\0' && i < (int)sizeof(combined) - 1) {
            combined[i++] = in[j++];
        }
    }
    combined[i] = '\0';

    /* 要素に割りながら '.' と '..' を処理する */
    i = 0;
    while (combined[i] != '\0') {
        int start, len;
        while (combined[i] == '/') {
            i++;
        }
        if (combined[i] == '\0') {
            break;
        }
        start = i;
        while (combined[i] != '\0' && combined[i] != '/') {
            i++;
        }
        len = i - start;
        if (len == 1 && combined[start] == '.') {
            continue;
        }
        if (len == 2 && combined[start] == '.' &&
            combined[start + 1] == '.') {
            if (ncomp > 0) {
                ncomp--;  /* 親へ (ルートではそのまま) */
            }
            continue;
        }
        if (len > 31 || ncomp >= PATH_MAX / 2) {
            return -1;
        }
        comp[ncomp] = combined + start;
        lens[ncomp] = len;
        ncomp++;
    }

    /* '/a/b/c' の形に組み直す (要素が無ければ "/") */
    out[n++] = '/';
    for (i = 0; i < ncomp; i++) {
        int j;
        if (i > 0) {
            out[n++] = '/';
        }
        if (n + lens[i] >= PATH_MAX) {
            return -1;
        }
        for (j = 0; j < lens[i]; j++) {
            out[n++] = comp[i][j];
        }
    }
    out[n] = '\0';
    return 0;
}

/* 解決済み絶対パスが /sd の配下か ("/sd" 自身を含む) */
static int is_sd_abs(const char *abs)
{
    return str_eq(abs, "/sd") || is_sd_path(abs);
}

/* /sd を外した FAT 側の相対パス ("/sd" → ""、"/sd/X" → "X") */
static const char *sd_rel(const char *abs)
{
    return abs[3] == '\0' ? abs + 3 : abs + 4;
}

static void clear_fds(struct fd_entry *t)
{
    int i;
    for (i = 0; i < MAX_FD; i++) {
        t[i].used = 0;
    }
}

/* --- exec の積み重ね (ステップ 6 → exec 多段化) -----------------------
 * 親が exec を呼んだら、親の trapframe・sepc・sp・領域の割り当て・
 * cwd を exec_stack へ退避し、同じ仮想アドレスに子の領域を
 * 差し替えて子を起動する。子が exit / 保護違反で終わったら、
 * 退避した親の状態を現在の trapframe に書き戻し、親は exec から
 * 戻った形で再開する。exec_depth が「今動いているプログラムの段」
 * で、0 がカーネルから直接起動された根のプログラム。
 * 段A (現段階) では退避を積み重ねる構造と段ごとの fd テーブルだけ
 * を入れ、領域は従来どおり親子 2 セットなので深さ 1 で打ち止める。
 * 領域のプール化は段B で行う (doc/exec-multilevel.md)。 */

static struct {
    uint64_t frame[32];
    uint64_t sepc;
    uint64_t sp;
    uint64_t code_pa;
    uint64_t data_pa;
    uint64_t stack_pa;
    char cwd[PATH_MAX];  /* 親の cwd (子が chdir しても戻せるように) */
} exec_stack[EXEC_MAX_DEPTH];

static int exec_depth = 0;  /* 今動いているプログラムの段 */

static void resume_parent(uint64_t *frame, uint64_t result)
{
    int i;
    exec_depth--;
    for (i = 0; i < 32; i++) {
        frame[i] = exec_stack[exec_depth].frame[i];
    }
    frame[FRAME_A0] = result;
    w_sepc(exec_stack[exec_depth].sepc + 4);
    w_sscratch(exec_stack[exec_depth].sp);
    vm_switch_user_region(exec_stack[exec_depth].code_pa,
                          exec_stack[exec_depth].data_pa,
                          exec_stack[exec_depth].stack_pa);
    proc_set_cwd(exec_stack[exec_depth].cwd);
    fd_table = fd_tables[exec_depth];  /* 親の fd テーブルに戻す */
}

/* exec の引数 (a1 で渡される空白区切りの文字列) の上限 */
#define EXEC_ARGS_MAX 512
#define EXEC_ARGC_MAX 16

/* 戻り値: 1 = sepc は処理済みなので進めない、0 = 通常どおり進める */
static int do_exec(uint64_t *frame)
{
    char name[PATH_MAX];
    char abs[PATH_MAX];
    char argsbuf[EXEC_ARGS_MAX];
    const char *tokens[EXEC_ARGC_MAX];
    int argc = 0;
    struct fs_file *f;
    uint64_t entry;
    int i;

    if (exec_depth >= 1) {
        /* 段A では領域セットが親子 2 組だけなので深さ 1 で
         * 打ち止める。段B でプール化したら EXEC_MAX_DEPTH
         * (sanity 上限) まで開ける */
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }
    if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0 ||
        resolve_path(name, abs) != 0 || is_sd_abs(abs)) {
        /* SD からの exec は v1 の対象外 (sd-design.md) */
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }
    /* 引数文字列 (任意、a1) を親のメモリから読み、空白で割る。
     * 引用符やエスケープは v1 では扱わない (割り切り) */
    argsbuf[0] = '\0';
    if (frame[FRAME_A1] != 0 &&
        copy_user_string(frame[FRAME_A1], argsbuf, sizeof(argsbuf)) != 0) {
        argsbuf[0] = '\0';
    }
    {
        char *p = argsbuf;
        while (*p != '\0' && argc < EXEC_ARGC_MAX) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '\0') {
                break;
            }
            tokens[argc++] = p;
            while (*p != '\0' && *p != ' ' && *p != '\t') {
                p++;
            }
            if (*p != '\0') {
                *p = '\0';
                p++;
            }
        }
    }
    if (argc == 0) {
        tokens[0] = name;  /* 引数が無ければ argv[0] = プログラム名 */
        argc = 1;
    }
    f = fs_find(abs + 1);
    if (f == 0 || f->is_dir) {
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }
    entry = elf_load_to(f, CHILD_CODE_PA, CHILD_DATA_PA);
    if (entry == 0) {
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }

    /* 親の状態を退避スタックへ積む */
    for (i = 0; i < 32; i++) {
        exec_stack[exec_depth].frame[i] = frame[i];
    }
    exec_stack[exec_depth].sepc = r_sepc();
    exec_stack[exec_depth].sp = r_sscratch();
    exec_stack[exec_depth].code_pa = vm_current_code_pa();
    exec_stack[exec_depth].data_pa = vm_current_data_pa();
    exec_stack[exec_depth].stack_pa = vm_current_stack_pa();
    /* cwd は子へ引き継ぐ (そのまま)。親の分は退避しておき、
     * 子が chdir しても resume_parent で戻す */
    {
        const char *cwd = proc_cwd();
        int i2 = 0;
        while (cwd[i2] != '\0' && i2 < PATH_MAX - 1) {
            exec_stack[exec_depth].cwd[i2] = cwd[i2];
            i2++;
        }
        exec_stack[exec_depth].cwd[i2] = '\0';
    }
    exec_depth++;
    /* 子は空の fd テーブルで始める。親のテーブルは親の段に
     * 残るので、子の終了後に親の fd がそのまま使える */
    fd_table = fd_tables[exec_depth];
    clear_fds(fd_table);

    /* 子の領域に差し替えて、子の入口から開始させる。
     * レジスタは汚れたまま渡さないようゼロにしておく。
     * 引数は子のスタック末尾に置き、入口の約束どおり
     * a0 = argc、a1 = argv で渡す */
    vm_switch_user_region(CHILD_CODE_PA, CHILD_DATA_PA,
                          CHILD_STACK_PA);
    for (i = 0; i < 32; i++) {
        frame[i] = 0;
    }
    {
        uint64_t argv_va;
        uint64_t sp = user_stack_setup_argv(CHILD_STACK_PA, argc,
                                            tokens, &argv_va);
        frame[FRAME_A0] = (uint64_t)argc;
        frame[FRAME_A1] = argv_va;
        w_sepc(entry);
        w_sscratch(sp);
    }
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
        if (exec_depth > 0) {
            /* a0 = 終了ステータス。そのまま親の exec の戻り値に
             * なる (0 が正常。-1 / -2 は exec 自身の失敗と
             * 保護違反用に予約されているので、ステータスは
             * 0 以上を使う約束) */
            uint64_t status = frame[FRAME_A0];
            resume_parent(frame, status);  /* 親の exec へ戻る */
            return 1;
        }
        user_exit_jump();  /* ここからは戻ってこない */
        break;
    case SYS_OPEN: {
        char name[PATH_MAX];
        char abs[PATH_MAX];
        int i;
        struct fs_file *f = 0;
        struct fat_file fat;
        int is_sd = 0;
        fat.first_cluster = 0;
        fat.size = 0;
        if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0 ||
            resolve_path(name, abs) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (is_sd_abs(abs)) {
            /* /sd は固定マウント。mount 点を外して FAT 側へ */
            if (fat_ensure_mounted() != 0 ||
                fat_find(sd_rel(abs), &fat) != 0) {
                frame[FRAME_A0] = (uint64_t)-1;
                break;
            }
            is_sd = 1;
        } else {
            f = fs_find(abs + 1);
            if (f == 0 || f->is_dir) {
                frame[FRAME_A0] = (uint64_t)-1;
                break;
            }
        }
        for (i = 0; i < MAX_FD; i++) {
            if (!fd_table[i].used) {
                fd_table[i].file = f;
                fd_table[i].fat = fat;
                fd_table[i].pos = 0;
                fd_table[i].used = 1;
                fd_table[i].writable = 0;
                fd_table[i].is_sd = is_sd;
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
        unsigned char buf[256];
        uint64_t remain, n, done = 0;
        if (fd >= MAX_FD || !fd_table[fd].used || fd_table[fd].writable) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        remain = (fd_table[fd].is_sd ? fd_table[fd].fat.size
                                     : fd_table[fd].file->size) -
                 fd_table[fd].pos;
        n = max < remain ? max : remain;
        /* ファイルからは位置指定 (fs_read_at / fat_read_at) で
         * 小分けに読み、ユーザーメモリへ運ぶ
         * (データは連続とは限らない) */
        while (done < n) {
            uint64_t chunk = n - done;
            uint64_t got;
            if (chunk > sizeof(buf)) {
                chunk = sizeof(buf);
            }
            got = fd_table[fd].is_sd
                      ? fat_read_at(&fd_table[fd].fat,
                                    fd_table[fd].pos + done, buf, chunk)
                      : fs_read_at(fd_table[fd].file,
                                   fd_table[fd].pos + done, buf, chunk);
            if (got == 0) {
                break;
            }
            if (copy_to_user(frame[FRAME_A1] + done, buf, got) != 0) {
                frame[FRAME_A0] = (uint64_t)-1;
                done = (uint64_t)-1;
                break;
            }
            done += got;
        }
        if (done != (uint64_t)-1) {
            fd_table[fd].pos += done;
            frame[FRAME_A0] = done;
        }
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
        /* a0 = 番号、a1 = 名前バッファ (32 バイト)、
         * a2 = ディレクトリのパス (NULL なら cwd)。
         * 戻り値: 1 = ファイル、2 = ディレクトリ、0 = 終わり、
         * -1 = そのディレクトリは無い (段4 で拡張)。
         * バッファには要素名だけが入る */
        char dirbuf[PATH_MAX];
        char abs[PATH_MAX];
        char namebuf[32];
        uint64_t len = 0;
        int is_dir = 0;
        if (frame[FRAME_A2] != 0) {
            if (copy_user_string(frame[FRAME_A2], dirbuf,
                                 sizeof(dirbuf)) != 0 ||
                resolve_path(dirbuf, abs) != 0) {
                frame[FRAME_A0] = (uint64_t)-1;
                break;
            }
        } else if (resolve_path(".", abs) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (is_sd_abs(abs)) {
            uint64_t sz;
            int d = 0;
            int r = fat_list(sd_rel(abs), (int)frame[FRAME_A0],
                             namebuf, &sz, &d);
            if (r < 0) {
                frame[FRAME_A0] = (uint64_t)-1;
                break;
            }
            if (r == 0) {
                frame[FRAME_A0] = 0;
                break;
            }
            is_dir = d;
        } else {
            if (!fs_is_dir(abs + 1) ||
                fs_child(abs + 1, (int)frame[FRAME_A0], namebuf,
                         sizeof(namebuf), &is_dir) != 1) {
                /* ディレクトリが無ければ -1、子が尽きたら 0。
                 * fs_is_dir で区別する */
                frame[FRAME_A0] = fs_is_dir(abs + 1)
                                      ? 0
                                      : (uint64_t)-1;
                break;
            }
        }
        while (namebuf[len] != '\0') {
            len++;
        }
        if (copy_to_user(frame[FRAME_A1],
                         (const unsigned char *)namebuf,
                         len + 1) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        frame[FRAME_A0] = is_dir ? 2 : 1;
        break;
    }
    case SYS_EXEC:
        if (do_exec(frame)) {
            return 1;  /* sepc は do_exec が子の入口に設定済み */
        }
        break;  /* 失敗時は通常どおり sepc を進めて戻る */
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
    case SYS_UNLINK: {
        char name[PATH_MAX];
        char abs[PATH_MAX];
        int i;
        struct fs_file *f;
        if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0 ||
            resolve_path(name, abs) != 0 || is_sd_abs(abs)) {
            /* SD は読み取り専用なので unlink は常に失敗 */
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        f = fs_find(abs + 1);
        if (f == 0 || f->is_dir) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        /* 開いているファイルは消せない (段2 の割り切り)。
         * 消すと fd の先が宙に浮くため。fd テーブルは段ごとに
         * あるので、どの段で開いていても対象にする (SD の fd は
         * file が 0 なので一致しない) */
        {
            int l, open_found = 0;
            for (l = 0; l <= exec_depth && !open_found; l++) {
                for (i = 0; i < MAX_FD; i++) {
                    if (fd_tables[l][i].used &&
                        fd_tables[l][i].file == f) {
                        open_found = 1;
                        break;
                    }
                }
            }
            if (open_found) {
                frame[FRAME_A0] = (uint64_t)-1;
                break;
            }
        }
        frame[FRAME_A0] = (uint64_t)fs_unlink(abs + 1);
        break;
    }
    case SYS_MKDIR: {
        char name[PATH_MAX];
        char abs[PATH_MAX];
        if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0 ||
            resolve_path(name, abs) != 0 || is_sd_abs(abs)) {
            frame[FRAME_A0] = (uint64_t)-1;  /* SD は読み取り専用 */
            break;
        }
        frame[FRAME_A0] = (uint64_t)fs_mkdir(abs + 1);
        break;
    }
    case SYS_RMDIR: {
        char name[PATH_MAX];
        char abs[PATH_MAX];
        if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0 ||
            resolve_path(name, abs) != 0 || is_sd_abs(abs)) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (str_eq(abs, proc_cwd())) {
            frame[FRAME_A0] = (uint64_t)-1;  /* 自分の cwd は消せない */
            break;
        }
        frame[FRAME_A0] = (uint64_t)fs_rmdir(abs + 1);
        break;
    }
    case SYS_CHDIR: {
        char name[PATH_MAX];
        char abs[PATH_MAX];
        if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0 ||
            resolve_path(name, abs) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (is_sd_abs(abs)) {
            /* /sd の中へも入れる (読み取り専用でも居場所には
             * なれる)。実在するディレクトリかを FAT 側で見る */
            char nm[16];
            uint64_t sz;
            int d;
            if (fat_list(sd_rel(abs), 0, nm, &sz, &d) < 0) {
                frame[FRAME_A0] = (uint64_t)-1;
                break;
            }
        } else if (!fs_is_dir(abs + 1)) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        proc_set_cwd(abs);
        frame[FRAME_A0] = 0;
        break;
    }
    case SYS_GETCWD: {
        const char *cwd = proc_cwd();
        uint64_t len = 0;
        while (cwd[len] != '\0') {
            len++;
        }
        len++;  /* NUL まで含めた長さ */
        if (frame[FRAME_A1] < len ||
            copy_to_user(frame[FRAME_A0],
                         (const unsigned char *)cwd, len) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        frame[FRAME_A0] = len;
        break;
    }
    case SYS_CREATE: {
        char name[PATH_MAX];
        char abs[PATH_MAX];
        int i;
        struct fs_file *f;
        if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0 ||
            resolve_path(name, abs) != 0 || is_sd_abs(abs)) {
            frame[FRAME_A0] = (uint64_t)-1;  /* SD は読み取り専用 */
            break;
        }
        f = fs_create(abs + 1);
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
                fd_table[i].is_sd = 0;
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
        if (exec_depth > 0) {
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
