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
#include "klog.h"
#include "page.h"
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

/* --- 浮動小数点の初期化 (プログラム開始時) --------------------------
 * sstatus.FS (bits 14:13) は FPU の状態: 0 = Off (FP 命令は
 * 違法命令)、1 = Initial、2 = Clean、3 = Dirty。ブートチェーン
 * の残した値に任せず、ユーザープログラムへ入る前に必ず
 * Initial にして「FPU が使える」ことを保証する (カーネルから
 * の直接起動は user.S の user_launch 側で同じことをする)。
 * あわせて fcsr (丸めモード等) を 0 に戻す: K210 では ROM が
 * frm に予約値 (111) を残すことが実測されており、そのままだと
 * 浮動小数点演算が違法命令になる。fcsr へのアクセス自体、
 * FS が Off だとできないので FS を先に立てる。
 * FP レジスタの中身は exec をまたいで保存しない (同時に走る
 * ユーザープログラムは 1 本という現行モデルの割り切り。
 * doc/syscalls.md の入口の約束に明記)。 */

#define SSTATUS_FS_MASK (3ULL << 13)
#define SSTATUS_FS_INITIAL (1ULL << 13)

static void user_fp_init(void)
{
    uint64_t s = r_sstatus();
    s = (s & ~SSTATUS_FS_MASK) | SSTATUS_FS_INITIAL;
    w_sstatus(s);
    asm volatile("csrw fcsr, zero");
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

/* --- 小さな表示部品 ------------------------------------------------
 * カーネルのメッセージは klog 経由 (klog.h) で出し、コンソール
 * に出ると同時にログにも残る。数値の表示は klog 側の部品を
 * そのまま使う (put_hex / put_dec はその別名)。 */

#define put_hex kput_hex
#define put_dec kput_dec

/* tick の表示専用 (10 進)。tick は毎秒出るのでログには残さず、
 * コンソールにだけ出す (klog.c の方針) */
static void con_dec(uint64_t x)
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
static uint64_t ticks;          /* 起動以来の秒数 (1 秒に 1 回増える) */

/* klog の接頭辞 [秒] 用: 起動以来の秒数を返す */
uint64_t trap_tick_count(void)
{
    return ticks;
}

/* uptime (31) 用: 起動以来のミリ秒。秒 (ticks) に、現在の
 * 秒の端数を足して組み立てる。端数は tick_divider が
 * 100ms 単位で数えている (割り込み 10 回で ticks が 1 増
 * える)。タイマ割り込みが 100ms 間隔なので実分解能は
 * 100ms で、値は 100ms 刻みに進む (仕様にも明記) */
uint64_t trap_uptime_ms(void)
{
    return ticks * 1000 + (uint64_t)tick_divider * 100;
}

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
        kputs("trap: sbi_set_timer failed\n");
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
#define SYS_JOB_RUN 20
#define SYS_JOB_LIST 21
#define SYS_JOB_WAIT 22
#define SYS_KLOG_READ 30
#define SYS_UPTIME 31
#define SYS_AUDIO_OPEN 40
#define SYS_AUDIO_WRITE 41
#define SYS_AUDIO_CLOSE 42

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
 * ポインタはユーザーの仮想アドレスで渡される。現在の段の
 * 割り当て (vm.c の user_va_to_pa) で訳せる範囲だけを触る。
 * スタックの未割り当てページは、カーネル経由のコピーでも
 * demand paging が効くよう、伸ばしてから訳し直す (プログラムが
 * まだ触っていないスタック上のバッファを渡されても動くように) */

static uint64_t user_pa(uint64_t va)
{
    uint64_t pa = user_va_to_pa(va);
    if (pa == 0 && vm_stack_grow(va)) {
        pa = user_va_to_pa(va);
    }
    return pa;
}

static int copy_user_string(uint64_t va, char *buf, int max)
{
    int i;
    for (i = 0; i < max - 1; i++) {
        uint64_t pa = user_pa(va + (uint64_t)i);
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
        uint64_t pa = user_pa(va + i);
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
        uint64_t pa = user_pa(va + i);
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

static struct fd_entry fd_tables[VM_LEVELS][MAX_FD];  /* exec の段 + ジョブの段 */
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

/* SD 書き込みの締め忘れを拾う: テーブルの持ち主が close
 * しないまま終わった書き込み用 fd があれば、目録への
 * 書き戻し (fat_close_write) を済ませる。RAM FS 側は
 * close で何も起きないので対象は SD だけ */
static void flush_sd_fds(struct fd_entry *t)
{
    int i;
    for (i = 0; i < MAX_FD; i++) {
        if (t[i].used && t[i].is_sd && t[i].writable) {
            fat_close_write(&t[i].fat);
        }
    }
}

static void clear_fds(struct fd_entry *t)
{
    int i;
    flush_sd_fds(t);  /* 使い回す前に前の持ち主の書き込みを締める */
    for (i = 0; i < MAX_FD; i++) {
        t[i].used = 0;
    }
}

/* --- exec の積み重ね (ステップ 6 → exec 多段化) -----------------------
 * 親が exec を呼んだら、親の trapframe・sepc・sp・cwd を
 * exec_stack へ退避し、子のページ台帳をページ表へ反映して子を
 * 起動する。物理メモリは段ごとにページ単位で割り当てるので
 * (vm.c の台帳)、親のメモリはプールに残ったまま凍結される。
 * 子が exit / 保護違反で終わったら、子のページをプールへ返し、
 * 退避した親の状態を現在の trapframe に書き戻して、親は exec
 * から戻った形で再開する。exec_depth が「今動いている
 * プログラムの段」で、0 がカーネルから直接起動された根の
 * プログラム。深さの上限は sanity の EXEC_MAX_DEPTH で、
 * それより先はプールの残量が実質の上限になる。 */

static struct {
    uint64_t frame[32];
    uint64_t sepc;
    uint64_t sp;
    char cwd[PATH_MAX];  /* 親の cwd (子が chdir しても戻せるように) */
} exec_stack[EXEC_MAX_DEPTH];

static int exec_depth = 0;  /* 今動いているプログラムの段 */

/* 子のプログラムから親へ戻る共通処理: 子のページを返し、
 * 1 段降りて親の状態 (レジスタ・写像・cwd・fd テーブル) を
 * 復元する。result は親の exec の戻り値になる */
static void resume_parent(uint64_t *frame, uint64_t result)
{
    int i;
    /* 子が閉じ忘れた SD 書き込みはここで締める (子の fd
     * テーブルはこの後しばらく残るが、目録の書き戻しは
     * プログラムの終了時点で済ませるのが約束) */
    flush_sd_fds(fd_tables[exec_depth]);
    vm_pages_release(exec_depth);  /* 子のページをプールへ返す */
    exec_depth--;
    for (i = 0; i < 32; i++) {
        frame[i] = exec_stack[exec_depth].frame[i];
    }
    frame[FRAME_A0] = result;
    w_sepc(exec_stack[exec_depth].sepc + 4);
    w_sscratch(exec_stack[exec_depth].sp);
    vm_apply_pages(exec_depth);  /* 親の台帳をページ表へ戻す */
    proc_set_cwd(exec_stack[exec_depth].cwd);
    fd_table = fd_tables[exec_depth];  /* 親の fd テーブルに戻す */
}

/* exec の引数 (a1 で渡される空白区切りの文字列) の上限 */
#define EXEC_ARGS_MAX 512
#define EXEC_ARGC_MAX 16

/* elf_load_src へ渡す FAT 側の読み口 (fs_read_at と同型に
 * そろえた橋渡し。doc/sd-exec-design.md 決め事2) */
static uint64_t fat_read_adapter(void *file, uint64_t pos,
                                 unsigned char *dst, uint64_t n)
{
    return fat_read_at((struct fat_file *)file, pos, dst, n);
}

/* SD のファイルがどこかの段かジョブで書き込み用に開かれて
 * いるか (exec の点検用)。実体はジョブの節に置く (ジョブの
 * fd テーブルも見るため) */
static int sd_open_for_write(const struct fat_file *target);

/* 戻り値: 1 = sepc は処理済みなので進めない、0 = 通常どおり進める */
static int do_exec(uint64_t *frame)
{
    char name[PATH_MAX];
    char abs[PATH_MAX];
    char argsbuf[EXEC_ARGS_MAX];
    const char *tokens[EXEC_ARGC_MAX];
    int argc = 0;
    struct fs_file *f;
    struct fat_file fat;
    int from_sd = 0;
    struct user_pages *up;
    uint64_t entry;
    int i;

    if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0) {
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }
    if (exec_depth >= EXEC_MAX_DEPTH) {
        /* sanity 上限。これ以上は積み重ねない (仕様として
         * syscalls.md に明記)。戻り値の -1 だけでは理由が
         * 分からないので、ログに理由を残す (klog) */
        kputs("exec: denied, depth limit: ");
        kputs(name);
        kputc('\n');
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }
    if (resolve_path(name, abs) != 0) {
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
    if (is_sd_abs(abs)) {
        /* /sd 上の ELF も実行できる (doc/sd-exec-design.md)。
         * ディレクトリは fat_find が -1 にする (RAM 側の
         * is_dir の弾き方と揃う)。書き込み用に開かれている
         * ファイルは、目録への書き戻しが close まで済んで
         * いないので実行しない (決め事4) */
        if (fat_find(sd_rel(abs), &fat) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            return 0;
        }
        if (sd_open_for_write(&fat)) {
            kputs("exec: sd file open for writing: ");
            kputs(name);
            kputc('\n');
            frame[FRAME_A0] = (uint64_t)-1;
            return 0;
        }
        from_sd = 1;
    } else {
        f = fs_find(abs + 1);
        if (f == 0 || f->is_dir) {
            frame[FRAME_A0] = (uint64_t)-1;
            return 0;
        }
    }
    /* 子の段の台帳へ ELF を読み、スタックの初期分 (16KB) を
     * 用意する。どちらか失敗したら、取ったページを返して
     * 起動失敗 (-1) にする */
    up = vm_pages(exec_depth + 1);
    vm_pages_release(exec_depth + 1);  /* 前の残りがあれば返す */
    entry = from_sd ? elf_load_src(&fat, fat_read_adapter, up)
                    : elf_load_to(f, up);
    if (entry == 0 || vm_stack_prepare(up, 16384) != 0) {
        /* 失敗の具体的な理由 (プール枯渇、ELF の形など) は
         * elf.c / vm.c 側がログに残す。ここでは対象だけ残す */
        kputs("exec: cannot load: ");
        kputs(name);
        kputc('\n');
        vm_pages_release(exec_depth + 1);
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }

    /* 親の状態を退避スタックへ積む */
    for (i = 0; i < 32; i++) {
        exec_stack[exec_depth].frame[i] = frame[i];
    }
    exec_stack[exec_depth].sepc = r_sepc();
    exec_stack[exec_depth].sp = r_sscratch();
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
    vm_apply_pages(exec_depth);  /* 子の台帳をページ表へ反映 */

    /* 引数は子のスタック末尾に置く (台帳経由の書き込みなので
     * ページ表の切り替え前後どちらでもよい)。置けなければ
     * 積んだものを巻き戻して起動失敗にする */
    {
        uint64_t argv_va;
        uint64_t sp = user_stack_setup_argv(up, argc, tokens, &argv_va);
        if (sp == 0) {
            /* 実質プール枯渇のときだけ到達する経路 */
            kputs("exec: cannot place argv: ");
            kputs(name);
            kputc('\n');
            resume_parent(frame, (uint64_t)-1);
            return 1;  /* sepc は resume_parent が親側に設定済み */
        }
        /* レジスタは汚れたまま渡さないようゼロにしておく。
         * 入口の約束どおり a0 = argc、a1 = argv で渡す */
        for (i = 0; i < 32; i++) {
            frame[i] = 0;
        }
        frame[FRAME_A0] = (uint64_t)argc;
        frame[FRAME_A1] = argv_va;
        user_fp_init();  /* 子も FPU が使える状態で始める */
        w_sepc(entry);
        w_sscratch(sp);
    }
    return 1;
}

/* --- ジョブ (スケジューラ段3、doc/jobs-design.md) -------------------
 * exec の「親が凍結する」モデルを広げ、ユーザープログラムを
 * バックグラウンドのジョブとして常駐・並行実行させる仕組み。
 *
 * 「走者 (runner)」が単位: 前面の走者 1 つ (根のプログラムと
 * exec の段の流れ。番号 0) と、ジョブの走者 最大 JOB_MAX 本
 * (番号 1〜JOB_MAX = ジョブ番号)。同時に CPU を使うのは
 * 1 つだけで、切り替えはトラップの区切りでだけ起きる:
 *   - タイマ割り込み (ユーザーモードから): ラウンドロビン
 *   - getc: 持ち主の待ちでジョブへ / bg ジョブは停止
 *   - ジョブの exit / フォルト: 次の走者へ (待つ前面がいれば
 *     ステータスを渡して起こす)
 *   - job_wait: 前面が待ちに入り、対象に番が回る
 * 各ジョブはページ台帳・fd テーブル・cwd を専用に持ち、
 * 切り替えでは相手のものを差し替える (resume_parent の
 * 一般化)。前面の走者にも、ジョブへ番を渡している間の
 * 文脈の退避場所 (fg_save) を 1 組持つ。
 * exec_depth は前面の走者の中だけの話で、ジョブが走って
 * いる間は値が残っているだけで参照されない (syscall の
 * 振り分けは cur_runner で行う)。 */

#define JOB_FREE 0
#define JOB_RUNNING 1  /* 実行可能 (走っている最中も含む) */
#define JOB_STOPPED 2  /* 入力待ちで停止 (bg の getc) */
#define JOB_DONE 3     /* 終わった (回収待ち、status が有効) */

struct job_slot {
    uint64_t frame[32];   /* 退避したレジスタ一式 */
    uint64_t sepc;
    uint64_t sp;          /* ユーザーの sp (sscratch の中身) */
    uint64_t status;      /* JOB_DONE のときの終了ステータス */
    int state;
    char name[32];        /* jobs 表示用 (起動時に渡された名前) */
    char cwd[PATH_MAX];
};

static struct job_slot jobs[JOB_MAX];

/* 前面の走者の退避場所。valid は「退避中」を表し、その間は
 * ジョブのどれかが走っている。blocked は job_wait で
 * 待っている状態で、wait_job のジョブが終わるまで前面は
 * 実行可能にならない */
static struct {
    uint64_t frame[32];
    uint64_t sepc;
    uint64_t sp;
    char cwd[PATH_MAX];
    int valid;
    int blocked;
    int wait_job;  /* 待ち対象のジョブ番号 (0 = なし) */
} fg_save;

static int cur_runner = 0;     /* 今走っている走者 (0 = 前面) */
static int console_owner = 0;  /* コンソールの持ち主 (0 = 前面) */

/* 文字列を固定長バッファへ写す (末尾は必ず NUL) */
static void copy_str(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] != '\0' && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* 今の走者の文脈 (レジスタ・sepc・ユーザー sp・cwd) を
 * その走者の退避場所へ保存する。sepc は呼び出し側が決める
 * (通常の切り替えは今の値、syscall の再実行をしたい場所
 * では ecall の位置 = 今の値のまま) */
static void save_runner(uint64_t *frame, uint64_t sepc)
{
    int i;
    if (cur_runner == 0) {
        for (i = 0; i < 32; i++) {
            fg_save.frame[i] = frame[i];
        }
        fg_save.sepc = sepc;
        fg_save.sp = r_sscratch();
        copy_str(fg_save.cwd, proc_cwd(), PATH_MAX);
        fg_save.valid = 1;
    } else {
        struct job_slot *j = &jobs[cur_runner - 1];
        for (i = 0; i < 32; i++) {
            j->frame[i] = frame[i];
        }
        j->sepc = sepc;
        j->sp = r_sscratch();
        copy_str(j->cwd, proc_cwd(), PATH_MAX);
    }
}

/* 走者 runner の退避済み文脈を今のトラップの frame へ戻し、
 * ページ台帳・fd テーブル・cwd をその走者のものに切り替える。
 * この後のトラップ復帰 (sret) で、その走者の続きが走る */
static void restore_runner(uint64_t *frame, int runner)
{
    int i;
    if (runner == 0) {
        for (i = 0; i < 32; i++) {
            frame[i] = fg_save.frame[i];
        }
        w_sepc(fg_save.sepc);
        w_sscratch(fg_save.sp);
        proc_set_cwd(fg_save.cwd);
        fg_save.valid = 0;
        fd_table = fd_tables[exec_depth];  /* 前面の今の段の表 */
        vm_apply_pages(exec_depth);
    } else {
        struct job_slot *j = &jobs[runner - 1];
        int level = JOB_LEVEL_BASE + (runner - 1);
        for (i = 0; i < 32; i++) {
            frame[i] = j->frame[i];
        }
        w_sepc(j->sepc);
        w_sscratch(j->sp);
        proc_set_cwd(j->cwd);
        fd_table = fd_tables[level];
        vm_apply_pages(level);
    }
    cur_runner = runner;
}

/* 走者が「実行可能」か。前面は job_wait で待っている間だけ
 * 実行可能でない。ジョブは RUNNING のときだけ */
static int runner_runnable(int r)
{
    if (r == 0) {
        return !fg_save.blocked;
    }
    return jobs[r - 1].state == JOB_RUNNING;
}

/* ラウンドロビン: 今の走者の次から順に実行可能な走者を探す。
 * 誰もいなければ今の走者を返す */
static int pick_next_runner(void)
{
    int k, r;
    for (k = 1; k <= JOB_MAX; k++) {
        r = (cur_runner + k) % (JOB_MAX + 1);
        if (runner_runnable(r)) {
            return r;
        }
    }
    return cur_runner;
}

/* 生きているジョブ (FREE でないスロット) があるか */
static int jobs_any_active(void)
{
    int i;
    for (i = 0; i < JOB_MAX; i++) {
        if (jobs[i].state != JOB_FREE) {
            return 1;
        }
    }
    return 0;
}

/* タイマ割り込みからのラウンドロビン (trap_handler が
 * ユーザーモードからの割り込みで呼ぶ)。ジョブが 1 つも
 * 無ければ従来の挙動と完全に同じ (何もしない) */
static void job_round_robin(uint64_t *frame)
{
    int next;
    if (!jobs_any_active()) {
        return;
    }
    next = pick_next_runner();
    if (next != cur_runner) {
        save_runner(frame, r_sepc());
        restore_runner(frame, next);
    }
}

/* 符号つき 10 進をログへ (終了ステータスは -2 もありうる) */
static void kput_sdec(uint64_t v)
{
    int64_t sv = (int64_t)v;
    if (sv < 0) {
        kputc('-');
        kput_dec((uint64_t)(-sv));
    } else {
        kput_dec(v);
    }
}

/* 今走っているジョブを終わらせる (exit / フォルト共通)。
 * ページを返し、fd を締めて、ログに 1 行残す。job_wait で
 * 待っている前面がいれば、ステータスを渡して前面を起こし、
 * スロットは回収済み (FREE) にする。いなければ done として
 * 残し、次の実行可能な走者へ移る。どちらの場合も、戻る
 * ときには frame に次の走者の文脈が入っている */
static void job_finish(uint64_t *frame, uint64_t status)
{
    int jobno = cur_runner;
    struct job_slot *j = &jobs[jobno - 1];
    int level = JOB_LEVEL_BASE + (jobno - 1);

    clear_fds(fd_tables[level]);  /* SD 書き込みの締めも含む */
    vm_pages_release(level);
    kputs("job: done ");
    kputs(j->name);
    kputs(" status=");
    kput_sdec(status);
    kputc('\n');

    if (fg_save.blocked && fg_save.wait_job == jobno) {
        j->state = JOB_FREE;
        fg_save.frame[FRAME_A0] = status;
        fg_save.sepc += 4;  /* job_wait の ecall を抜ける */
        fg_save.blocked = 0;
        fg_save.wait_job = 0;
        console_owner = 0;
        restore_runner(frame, 0);
        return;
    }
    j->state = JOB_DONE;
    j->status = status;
    {
        int next = pick_next_runner();
        if (next == jobno) {
            /* 起こせる相手が本当にいない場合の保険
             * (通常は待つ前面か対象ジョブが必ず実行可能) */
            next = 0;
        }
        restore_runner(frame, next);
    }
}

/* 全ジョブを終わらせる (根のプログラムの終了 / フォルト時)。
 * ジョブはシェルのセッションと運命を共にする */
static void jobs_kill_all(void)
{
    int i;
    for (i = 0; i < JOB_MAX; i++) {
        if (jobs[i].state != JOB_FREE) {
            int level = JOB_LEVEL_BASE + i;
            clear_fds(fd_tables[level]);
            vm_pages_release(level);
            jobs[i].state = JOB_FREE;
        }
    }
    fg_save.valid = 0;
    fg_save.blocked = 0;
    fg_save.wait_job = 0;
    console_owner = 0;
    cur_runner = 0;
}

/* SD のファイル (目録項の位置で照合) が、どこかの段か
 * ジョブで書き込み用に開かれているか。exec と job_run が
 * 「書き込み中の SD ファイルは実行しない」を点検する
 * ために使う (doc/sd-exec-design.md 決め事4)。照合の範囲は
 * unlink の点検と同じ (前面の全段 + 生きているジョブ)。
 * 読み用に開いているだけのものは対象にしない */
static int sd_open_for_write(const struct fat_file *target)
{
    int l, i;
    for (l = 0; l <= exec_depth; l++) {
        for (i = 0; i < MAX_FD; i++) {
            if (fd_tables[l][i].used &&
                fd_tables[l][i].is_sd &&
                fd_tables[l][i].writable &&
                fd_tables[l][i].fat.has_dir &&
                fd_tables[l][i].fat.dir_cluster ==
                    target->dir_cluster &&
                fd_tables[l][i].fat.dir_off == target->dir_off) {
                return 1;
            }
        }
    }
    for (l = 0; l < JOB_MAX; l++) {
        int t;
        if (jobs[l].state == JOB_FREE) {
            continue;
        }
        t = JOB_LEVEL_BASE + l;
        for (i = 0; i < MAX_FD; i++) {
            if (fd_tables[t][i].used &&
                fd_tables[t][i].is_sd &&
                fd_tables[t][i].writable &&
                fd_tables[t][i].fat.has_dir &&
                fd_tables[t][i].fat.dir_cluster ==
                    target->dir_cluster &&
                fd_tables[t][i].fat.dir_off == target->dir_off) {
                return 1;
            }
        }
    }
    return 0;
}

/* job_run (20): プログラムをジョブとして起動する。
 * do_exec と同じ約束 (名前 + 引数文字列) で、違うのは
 * 親を凍らせるのではなく、文脈を用意して RUNNING に
 * するだけなこと。実際の開始は次の切り替えのタイミング */
static void do_job_run(uint64_t *frame)
{
    char name[PATH_MAX];
    char abs[PATH_MAX];
    char argsbuf[EXEC_ARGS_MAX];
    const char *tokens[EXEC_ARGC_MAX];
    int argc = 0;
    struct fs_file *f;
    struct fat_file fat;
    int from_sd = 0;
    int i, slot = -1;

    if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0 ||
        resolve_path(name, abs) != 0) {
        frame[FRAME_A0] = (uint64_t)-1;
        return;
    }
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
        tokens[0] = name;
        argc = 1;
    }
    if (is_sd_abs(abs)) {
        /* exec と同じく /sd 上の ELF も対象 (ローダ共有。
         * doc/sd-exec-design.md 決め事5) */
        if (fat_find(sd_rel(abs), &fat) != 0 ||
            sd_open_for_write(&fat)) {
            frame[FRAME_A0] = (uint64_t)-1;
            return;
        }
        from_sd = 1;
    } else {
        f = fs_find(abs + 1);
        if (f == 0 || f->is_dir) {
            frame[FRAME_A0] = (uint64_t)-1;
            return;
        }
    }
    for (i = 0; i < JOB_MAX; i++) {
        if (jobs[i].state == JOB_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        kputs("job: table full: ");
        kputs(name);
        kputc('\n');
        frame[FRAME_A0] = (uint64_t)-1;
        return;
    }
    {
        int level = JOB_LEVEL_BASE + slot;
        struct user_pages *up = vm_pages(level);
        struct job_slot *j = &jobs[slot];
        uint64_t entry, argv_va, sp;

        vm_pages_release(level);  /* 前の残りがあれば返す */
        entry = from_sd ? elf_load_src(&fat, fat_read_adapter, up)
                        : elf_load_to(f, up);
        if (entry == 0 || vm_stack_prepare(up, 16384) != 0) {
            kputs("job: cannot load: ");
            kputs(name);
            kputc('\n');
            vm_pages_release(level);
            frame[FRAME_A0] = (uint64_t)-1;
            return;
        }
        sp = user_stack_setup_argv(up, argc, tokens, &argv_va);
        if (sp == 0) {
            vm_pages_release(level);
            frame[FRAME_A0] = (uint64_t)-1;
            return;
        }
        clear_fds(fd_tables[level]);  /* ジョブは空の表で始める */
        for (i = 0; i < 32; i++) {
            j->frame[i] = 0;
        }
        j->frame[FRAME_A0] = (uint64_t)argc;
        j->frame[FRAME_A1] = argv_va;
        j->sepc = entry;
        j->sp = sp;
        j->status = 0;
        copy_str(j->name, name, sizeof(j->name));
        copy_str(j->cwd, proc_cwd(), PATH_MAX);  /* cwd は引き継ぐ */
        j->state = JOB_RUNNING;
        frame[FRAME_A0] = (uint64_t)(slot + 1);
    }
}

/* job_list (21) の 1 行を作る部品 (proc_list と同じ流儀) */
static int job_line(char *buf, int max, int n, int idx)
{
    const struct job_slot *j = &jobs[idx];
    const char *s;
    int i;

    s = "job ";
    while (*s != '\0' && n < max) {
        buf[n++] = *s++;
    }
    if (n < max) {
        buf[n++] = (char)('1' + idx);
    }
    s = ": ";
    while (*s != '\0' && n < max) {
        buf[n++] = *s++;
    }
    for (i = 0; j->name[i] != '\0' && n < max; i++) {
        buf[n++] = j->name[i];
    }
    if (n < max) {
        buf[n++] = ' ';
    }
    s = j->state == JOB_RUNNING ? "running"
        : j->state == JOB_STOPPED ? "stopped"
                                  : "done status=";
    while (*s != '\0' && n < max) {
        buf[n++] = *s++;
    }
    if (j->state == JOB_DONE) {
        int64_t sv = (int64_t)j->status;
        uint64_t uv;
        char rev[20];
        int m = 0;
        if (sv < 0 && n < max) {
            buf[n++] = '-';
            uv = (uint64_t)(-sv);
        } else {
            uv = (uint64_t)sv;
        }
        if (uv == 0) {
            rev[m++] = '0';
        }
        while (uv > 0) {
            rev[m++] = (char)('0' + (uv % 10));
            uv /= 10;
        }
        while (m > 0 && n < max) {
            buf[n++] = rev[--m];
        }
    }
    if (n < max) {
        buf[n++] = '\n';
    }
    return n;
}

static void do_job_list(uint64_t *frame)
{
    char kbuf[256];
    int n = 0;
    int i;
    for (i = 0; i < JOB_MAX; i++) {
        if (jobs[i].state != JOB_FREE) {
            n = job_line(kbuf, (int)sizeof(kbuf), n, i);
        }
    }
    if (copy_to_user(frame[FRAME_A0],
                     (const unsigned char *)kbuf, (uint64_t)n) != 0) {
        frame[FRAME_A0] = (uint64_t)-1;
    } else {
        frame[FRAME_A0] = (uint64_t)n;
    }
}

/* job_wait (22): 対象が終わっていれば即座にステータスを
 * 返して回収する。実行中なら、そのジョブをコンソールの
 * 持ち主にして、前面は待ちに入る (戻り値 1 = frame に
 * 対象ジョブの文脈を入れて切り替え済み)。完了は
 * job_finish が待ち前面の保存済み frame へ書き込む */
static int do_job_wait(uint64_t *frame)
{
    uint64_t num = frame[FRAME_A0];
    struct job_slot *j;

    if (num < 1 || num > JOB_MAX) {
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }
    j = &jobs[num - 1];
    if (j->state == JOB_FREE) {
        frame[FRAME_A0] = (uint64_t)-1;
        return 0;
    }
    if (j->state == JOB_DONE) {
        frame[FRAME_A0] = j->status;
        j->state = JOB_FREE;  /* 回収 */
        return 0;
    }
    console_owner = (int)num;
    if (j->state == JOB_STOPPED) {
        j->state = JOB_RUNNING;  /* 持ち主になるので再開 */
    }
    save_runner(frame, r_sepc());  /* ecall の位置で退避 */
    fg_save.blocked = 1;
    fg_save.wait_job = (int)num;
    restore_runner(frame, (int)num);
    return 1;
}

/* --- オーディオ (デバイス、40〜42) ----------------------------------
 * 仕様は doc/syscalls.md (Ikesu 段2の確定仕様。Ikesu 側の
 * doc/audio-syscall.md が原案)。カーネルは「サンプルの流れを
 * デバイスへ渡す」ことだけを知り、形式は符号付き 16bit
 * リトルエンディアン、インターリーブ固定。同時に開ける
 * 流れは 1 本だけ。
 *
 * 実機のドライバ (K210 の I2S) はまだ無いので、今は両ボード
 * 共通のダミー実装: audio_write は待たずに全部受け付け、
 * 受けたバイト列の FNV-1a 64bit ハッシュと総サンプル数を
 * 貯めておき、audio_close のときにカーネルログへ 1 行残す。
 * テストは Nish の dmesg 経由でこの行を照合する (テスト専用
 * の syscall は作らない)。ダミーは rate の対応可否を検証
 * しない (対応レートは本物のドライバが決めるものなので、
 * 今は任意の rate を受ける)。 */

#define FNV1A64_OFFSET 14695981039346656037ULL /* 0xcbf29ce484222325 */
#define FNV1A64_PRIME 1099511628211ULL         /* 0x100000001b3 */

static int audio_open_flag;      /* 1 = 開いている */
static uint64_t audio_rate;      /* audio_open の a0 の保持 */
static uint64_t audio_channels;  /* 1 または 2 */
static uint64_t audio_fnv;       /* 受けたバイト列の FNV-1a 64bit */
static uint64_t audio_samples;   /* 受け付けた総サンプル数 */

/* 16 進 16 桁 (小文字、ゼロ埋め、0x なし) をログへ出す。
 * kput_hex は 0x 接頭辞が付くので fnv の表示には使えない */
static void kput_hex16(uint64_t x)
{
    int i;
    for (i = 60; i >= 0; i -= 4) {
        int d = (int)((x >> i) & 0xF);
        kputc((char)(d < 10 ? '0' + d : 'a' + (d - 10)));
    }
}

static int do_syscall(uint64_t *frame)
{
    uint64_t num = frame[FRAME_A7];

    switch (num) {
    case SYS_PUTC:
        uart_putc((char)frame[FRAME_A0]);
        frame[FRAME_A0] = 0;
        break;
    case SYS_GETC: {
        /* コンソール仲裁 (doc/jobs-design.md): 文字は持ち主
         * にだけ配る。ジョブが無ければ従来とまったく同じ
         * 待ちになる (ポーリングの流儀はそのまま) */
        if (cur_runner != console_owner) {
            /* bg ジョブの getc: 入力待ちで停止する。文脈は
             * ecall の位置で保存するので、持ち主になって
             * 再開されたときに getc がやり直される */
            int next;
            save_runner(frame, r_sepc());
            jobs[cur_runner - 1].state = JOB_STOPPED;
            if (runner_runnable(console_owner)) {
                next = console_owner;
            } else {
                next = pick_next_runner();
            }
            restore_runner(frame, next);
            return 1;
        }
        for (;;) {
            int c = uart_try_getc();
            if (c >= 0) {
                frame[FRAME_A0] = (uint64_t)(unsigned char)c;
                break;
            }
            /* 文字が無い間は、実行可能なジョブに番を回す。
             * 自分の文脈は ecall の位置で保存しておき、
             * 戻ってきたら getc を再実行する */
            {
                int k, n, target = 0;
                for (k = 0; k < JOB_MAX; k++) {
                    n = (cur_runner + k) % JOB_MAX + 1;
                    if (n != cur_runner &&
                        jobs[n - 1].state == JOB_RUNNING) {
                        target = n;
                        break;
                    }
                }
                if (target != 0) {
                    save_runner(frame, r_sepc());
                    restore_runner(frame, target);
                    return 1;
                }
            }
            /* ジョブが無い / 誰も走れない: 待ちの間も
             * スケジューラを回し、寝ているタスク (時計) が
             * 起きられるようにする (スケジューラ段2)。ここは
             * トラップ文脈で割り込みが入らない間なので、
             * タイマ割り込みはこの待ちを助けてくれない。
             * 代わりにポーリングの合間に sched_poll() で
             * 番を回す (割り込み状態に触る yield() は
             * ここでは使えない) */
            sched_poll();
        }
        break;
    }
    case SYS_EXIT:
        if (cur_runner != 0) {
            /* ジョブの終了: そのジョブだけを終わらせ、
             * 次の走者へ移る (job_finish が frame を差し替える) */
            job_finish(frame, frame[FRAME_A0]);
            return 1;
        }
        if (exec_depth > 0) {
            /* a0 = 終了ステータス。そのまま親の exec の戻り値に
             * なる (0 が正常。-1 / -2 は exec 自身の失敗と
             * 保護違反用に予約されているので、ステータスは
             * 0 以上を使う約束) */
            uint64_t status = frame[FRAME_A0];
            resume_parent(frame, status);  /* 親の exec へ戻る */
            return 1;
        }
        /* 根のプログラムの終了: ジョブはセッションと運命を
         * 共にするので全部終わらせ、閉じ忘れた SD 書き込みが
         * あれば締めてから戻る (fd 自体の後始末は従来どおり
         * テーブルに残す) */
        jobs_kill_all();
        flush_sd_fds(fd_table);
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
            /* SD の書き込み用 fd は、閉じるときに目録への
             * 書き戻しまで済ませる (書き込み対応 段2/3)。
             * 失敗したら -1 を返す */
            int r = 0;
            if (fd_table[fd].is_sd && fd_table[fd].writable) {
                r = fat_close_write(&fd_table[fd].fat);
            }
            fd_table[fd].used = 0;
            frame[FRAME_A0] = (uint64_t)r;
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
        if (cur_runner != 0) {
            /* bg ジョブからの exec は作らない (入れ子に
             * しない、doc/jobs-design.md の割り切り) */
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
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
    case SYS_JOB_RUN:
    case SYS_JOB_LIST:
    case SYS_JOB_WAIT:
        /* ジョブ管理は根のプログラム (段 0 で動くシェル)
         * だけの仕事。ジョブの中や exec の子からは使えない */
        if (cur_runner != 0 || exec_depth != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (num == SYS_JOB_RUN) {
            do_job_run(frame);
        } else if (num == SYS_JOB_LIST) {
            do_job_list(frame);
        } else if (do_job_wait(frame)) {
            return 1;  /* 待ちに入って対象へ切り替え済み */
        }
        break;
    case SYS_UNLINK: {
        char name[PATH_MAX];
        char abs[PATH_MAX];
        int i;
        struct fs_file *f;
        if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0 ||
            resolve_path(name, abs) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (is_sd_abs(abs)) {
            /* SD 側も消せる (書き込み対応、段3)。開いている
             * ファイルは消せない割り切りは RAM 側と同じで、
             * 照合は目録項の位置 (fat_find が埋める) で行う */
            struct fat_file target;
            int l, open_found = 0;
            if (fat_find(sd_rel(abs), &target) != 0) {
                frame[FRAME_A0] = (uint64_t)-1;
                break;
            }
            for (l = 0; l <= exec_depth && !open_found; l++) {
                for (i = 0; i < MAX_FD; i++) {
                    if (fd_tables[l][i].used &&
                        fd_tables[l][i].is_sd &&
                        fd_tables[l][i].fat.has_dir &&
                        fd_tables[l][i].fat.dir_cluster ==
                            target.dir_cluster &&
                        fd_tables[l][i].fat.dir_off ==
                            target.dir_off) {
                        open_found = 1;
                        break;
                    }
                }
            }
            /* ジョブが開いている場合も同様に消せない (段3) */
            for (l = 0; l < JOB_MAX && !open_found; l++) {
                int t;
                if (jobs[l].state == JOB_FREE) {
                    continue;
                }
                t = JOB_LEVEL_BASE + l;
                for (i = 0; i < MAX_FD; i++) {
                    if (fd_tables[t][i].used &&
                        fd_tables[t][i].is_sd &&
                        fd_tables[t][i].fat.has_dir &&
                        fd_tables[t][i].fat.dir_cluster ==
                            target.dir_cluster &&
                        fd_tables[t][i].fat.dir_off ==
                            target.dir_off) {
                        open_found = 1;
                        break;
                    }
                }
            }
            frame[FRAME_A0] = open_found
                                  ? (uint64_t)-1
                                  : (uint64_t)fat_unlink(sd_rel(abs));
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
            /* ジョブが開いている場合も同様に消せない (段3) */
            for (l = 0; l < JOB_MAX && !open_found; l++) {
                int t;
                if (jobs[l].state == JOB_FREE) {
                    continue;
                }
                t = JOB_LEVEL_BASE + l;
                for (i = 0; i < MAX_FD; i++) {
                    if (fd_tables[t][i].used &&
                        fd_tables[t][i].file == f) {
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
            resolve_path(name, abs) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (is_sd_abs(abs)) {
            /* SD 側にも作れる (書き込み対応、段3)。
             * 中身は "." と ".." だけの空ディレクトリ */
            frame[FRAME_A0] = (uint64_t)fat_mkdir(sd_rel(abs));
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
        struct fs_file *f = 0;
        struct fat_file fat;
        int is_sd = 0;
        fat.first_cluster = 0;
        fat.size = 0;
        fat.has_dir = 0;
        fat.dirty = 0;
        if (copy_user_string(frame[FRAME_A0], name, sizeof(name)) != 0 ||
            resolve_path(name, abs) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (is_sd_abs(abs)) {
            /* /sd にも作れる (書き込み対応、doc/sd-write-
             * design.md 段3)。意味づけは RAM 側と同じ
             * (同名は空に切り捨て、親が無いと -1) */
            if (fat_create(sd_rel(abs), &fat) != 0) {
                frame[FRAME_A0] = (uint64_t)-1;
                break;
            }
            is_sd = 1;
        } else {
            f = fs_create(abs + 1);
            if (f == 0) {
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
                fd_table[i].writable = 1;
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
            /* 書き先は RAM FS か SD (FAT) かで振り分ける。
             * どちらも「末尾へ書く」意味づけは同じ */
            w = fd_table[fd].is_sd
                    ? fat_write_data(&fd_table[fd].fat, buf, chunk)
                    : fs_write_data(fd_table[fd].file, buf, chunk);
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
    case SYS_KLOG_READ: {
        /* a0 = バッファ、a1 = 大きさ、a2 = 読み出し開始
         * オフセット (起動以来の通し位置)。fs_read_at 的な
         * 位置指定読みで、Nish の dmesg が 0 から順に読む。
         * 開始位置が最古の残存位置より前なら最古に丸めて
         * から読み始める (丸めた位置を起点に数える) */
        char kbuf[256];
        uint64_t off = frame[FRAME_A2];
        uint64_t max = frame[FRAME_A1];
        uint64_t done = 0;
        if (off < klog_oldest()) {
            off = klog_oldest();
        }
        while (done < max) {
            uint64_t chunk = max - done;
            uint64_t got;
            if (chunk > sizeof(kbuf)) {
                chunk = sizeof(kbuf);
            }
            got = klog_read(off + done, kbuf, chunk);
            if (got == 0) {
                break;  /* 最新まで読んだ */
            }
            if (copy_to_user(frame[FRAME_A0] + done,
                             (const unsigned char *)kbuf, got) != 0) {
                frame[FRAME_A0] = (uint64_t)-1;
                done = (uint64_t)-1;
                break;
            }
            done += got;
            if (got < chunk) {
                break;  /* 末尾まで読み切った */
            }
        }
        if (done != (uint64_t)-1) {
            frame[FRAME_A0] = done;
        }
        break;
    }
    case SYS_UPTIME:
        /* 引数なし。起動以来のミリ秒を a0 に返す。
         * 実分解能は 100ms (タイマ割り込みの間隔)。
         * 仕様は doc/syscalls.md の 31 番 */
        frame[FRAME_A0] = trap_uptime_ms();
        break;
    case SYS_AUDIO_OPEN: {
        /* a0 = サンプルレート (Hz)、a1 = チャンネル数
         * (1 または 2)。同時に開けるのは 1 本だけ */
        uint64_t channels = frame[FRAME_A1];
        if (audio_open_flag || (channels != 1 && channels != 2)) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        audio_open_flag = 1;
        audio_rate = frame[FRAME_A0];
        audio_channels = channels;
        audio_fnv = FNV1A64_OFFSET;
        audio_samples = 0;
        frame[FRAME_A0] = 0;
        break;
    }
    case SYS_AUDIO_WRITE: {
        /* a0 = サンプル列へのポインタ、a1 = サンプル数
         * (int16 の個数)。ダミーは待たずに全部受け付け、
         * 受けたバイト列で FNV-1a を更新する */
        uint64_t nsamples = frame[FRAME_A1];
        uint64_t nbytes;
        unsigned char buf[256];
        uint64_t done = 0;
        if (!audio_open_flag || nsamples > (uint64_t)-1 / 2) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (nsamples == 0) {
            frame[FRAME_A0] = 0;
            break;
        }
        nbytes = nsamples * 2;
        /* ポインタが範囲外なら -1 (仕様)。途中まで受けて
         * から失敗するとハッシュが中途半端な状態になる
         * ので、先に全範囲が触れることを確かめる */
        {
            uint64_t i;
            int ok = 1;
            for (i = 0; i < nbytes; i++) {
                if (user_pa(frame[FRAME_A0] + i) == 0) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) {
                frame[FRAME_A0] = (uint64_t)-1;
                break;
            }
        }
        /* ユーザーメモリから小分けに運びながらハッシュする */
        while (done < nbytes) {
            uint64_t chunk = nbytes - done;
            uint64_t i;
            if (chunk > sizeof(buf)) {
                chunk = sizeof(buf);
            }
            if (copy_from_user(buf, frame[FRAME_A0] + done,
                               chunk) != 0) {
                /* 全範囲を確認済みなので、ここへは来ないはず */
                frame[FRAME_A0] = (uint64_t)-1;
                done = (uint64_t)-1;
                break;
            }
            for (i = 0; i < chunk; i++) {
                audio_fnv ^= buf[i];
                audio_fnv *= FNV1A64_PRIME;
            }
            done += chunk;
        }
        if (done != (uint64_t)-1) {
            audio_samples += nsamples;
            frame[FRAME_A0] = nsamples;
        }
        break;
    }
    case SYS_AUDIO_CLOSE: {
        /* 閉じる。受けた流れの要約をカーネルログへ 1 行残す
         * (テストは dmesg 経由でこの行を照合する) */
        if (!audio_open_flag) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        audio_open_flag = 0;
        kputs("audio: close rate=");
        kput_dec(audio_rate);
        kputs(" channels=");
        kput_dec(audio_channels);
        kputs(" samples=");
        kput_dec(audio_samples);
        kputs(" fnv=");
        kput_hex16(audio_fnv);
        kputc('\n');
        frame[FRAME_A0] = 0;
        break;
    }
    default:
        kputs("trap: unknown syscall ");
        put_dec(num);
        kputc('\n');
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
            /* tick はコンソールだけ (ログには残さない) */
            uart_puts("tick ");
            con_dec(ticks);
            uart_putc('\n');
        }
        if (sched_active) {
            /* 切り替えの判断 (スケジューラ段1):
             *   - プリエンプティブ有効 (デモの第 2 幕) なら毎回
             *   - そうでなくても、寝ていて起床時刻が来たタスクが
             *     いたら切り替える (起床駆動)。周期タスク (時計)
             *     は、相手が計算中でも 1 秒ごとに番が回る
             * 切り替わった先のタスクの文脈でこのハンドラに
             * 戻ってくることがあるので、帰る前に「今の」タスクの
             * sepc / sstatus を CSR に戻す */
            int due = sched_wake_due();
            if (preempt_enabled || due) {
                uint64_t e, s;
                schedule();
                sched_restore_trapstate(&e, &s);
                w_sepc(e);
                w_sstatus(s);
            }
        }
        /* ジョブのラウンドロビン (段3): ユーザーモードから
         * の割り込みでだけ走者を切り替える。本当にユーザー
         * モードから来たかは sstatus の SPP (bit8 = 前の
         * 特権モード) で判定する。from_user は入口の都合を
         * 伝えるだけで、カーネルタスクの割り込みがユーザー用
         * 入口 (trap_entry_user) を踏んだ場合と区別できない
         * ため、こちらを物差しにする */
        if (from_user && (r_sstatus() & (1ULL << 8)) == 0) {
            job_round_robin(frame);
        }
        return;
    }

    if (is_interrupt) {
        kputs("trap: unexpected interrupt, code=");
        put_dec(code);
        kputc('\n');
        return;
    }

    /* ここから例外 */
    if (from_user && code == 8) {
        /* ユーザーモードからの ecall = システムコール */
        do_syscall(frame);
        return;
    }

    /* スタックの伸長 (demand paging): ユーザーの load / store が
     * スタック領域の未割り当てページに触れたら、ページを足して
     * 命令をそのままやり直させる。対象は load / store のフォルト
     * だけ (標準の 13/15、ドラフト仕様の 5/7)。実行のフォルトや
     * データ領域の未割り当ては違反として下へ流す */
    if (from_user &&
        (code == 13 || code == 15
#if BOARD_DRAFT_FAULT_CODES
         || code == 5 || code == 7
#endif
         ) &&
        vm_stack_grow(r_stval())) {
        return;
    }

    if (from_user) {
        /* ユーザープログラムの例外 (メモリ保護違反など):
         * そのプログラムだけを終わらせる。exec の子なら
         * 親に戻して exec の結果として -2 を返す */
        kputs("trap: user program fault scause=");
        put_hex(scause);
        kputs(" sepc=");
        put_hex(r_sepc());
        kputs(" stval=");
        put_hex(r_stval());
        kputc('\n');
        if ((r_sstatus() & (1ULL << 8)) == 0 && cur_runner != 0) {
            /* ジョブのフォルト: そのジョブだけを終わらせる
             * (障害の分離。SPP の判定はタイマ側と同じ理由) */
            job_finish(frame, (uint64_t)-2);
            return;
        }
        if (exec_depth > 0) {
            resume_parent(frame, (uint64_t)-2);
            return;
        }
        jobs_kill_all();  /* 根が落ちたらジョブも全部終わらせる */
        flush_sd_fds(fd_table);  /* 根のプログラムの書き込みも締める */
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
        kputs("trap: page fault (code=");
        put_dec(code);
        kputs(") ");
    }
    kputs("trap: exception scause=");
    put_hex(scause);
    kputs(" sepc=");
    put_hex(r_sepc());
    kputs(" stval=");
    put_hex(r_stval());
    kputc('\n');
    for (;;) {
        asm volatile("wfi");
    }
}
