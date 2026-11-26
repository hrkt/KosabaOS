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
#include "gfx.h"
#include "audio.h"
#include "saver.h"   /* 入力活動の記録 (SYS_GETC の成功) */
#include "ssd1315.h" /* OLED 表示 (syscall 48 / 49) */
#include "traplog.h" /* M シムのトラップ記録の drain (k210) */
#include "smp.h"     /* hart1 再生係との受け渡し (SMP 段2) */

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

/* uptime (31) 用: 起動以来のミリ秒。time CSR (r_time、
 * K210 = 7.8MHz / virt = 10MHz) から直接組み立てるので
 * 分解能は 1ms (読みのたびに進む)。以前は 100ms 間隔の
 * タイマ割り込みで数える ticks + tick_divider から作って
 * いて 100ms 刻みにしか進まなかったが、sleep_ms の期限や
 * getc タイムアウトは既に r_time 基準だったので、読みの
 * 時計もそちらへ揃えた (2026-11-25、SMF プレーヤーの
 * 実時間ペーシングが ms で読める時計を要するため)。
 * ticks の方は klog の [秒] 表示と時計表示が使うので残す */
uint64_t trap_uptime_ms(void)
{
    return r_time() * 1000 / TIMER_HZ;
}

/* perf_read (33) 用: ハードウェアの性能カウンタを読む。
 * 種類 0 = cycle CSR (リセット以来のサイクル数)、1 = instret
 * CSR (実行を終えた命令数)。どちらもマシン全体の通算で、
 * 他のタスクやカーネル、割り込みの分も含む (タスクごとの
 * 集計ではない)。区間の前後で読んで差を取り、かかった
 * サイクル数や命令数を測るのが使い方 (仕様は
 * doc/syscalls.md の 33 番)。種類が不正なら (uint64_t)-1 */
static uint64_t trap_perf_read(uint64_t kind)
{
    uint64_t v;

    if (kind == 0) {
        asm volatile("csrr %0, cycle" : "=r"(v));
    } else if (kind == 1) {
        asm volatile("csrr %0, instret" : "=r"(v));
    } else {
        return (uint64_t)-1;
    }
    return v;
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
    gfx_init();       /* 前面の窓を開く (画面表示、doc/gfx-design.md) */

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
#define SYS_LSEEK 17
#define SYS_JOB_RUN 20
#define SYS_JOB_LIST 21
#define SYS_JOB_WAIT 22
#define SYS_KLOG_READ 30
#define SYS_UPTIME 31
#define SYS_PERF_READ 33
#define SYS_SLEEP_MS 34
#define SYS_AUDIO_OPEN 40
#define SYS_AUDIO_WRITE 41
#define SYS_AUDIO_CLOSE 42
#define SYS_GFX_INFO 43
#define SYS_GFX_CLEAR 44
#define SYS_GFX_RECT 45
#define SYS_GFX_TEXT 46
#define SYS_GFX_LINE 47
#define SYS_OLED_TEXT 48
#define SYS_OLED_CLEAR 49
#define SYS_AUDIO_SET_VOLUME 50

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

/* 範囲 [va, va+n) が全部訳せるかを、ページごとに見る。
 * バイトごとに user_pa を呼ぶ版と意味は同じだが、
 * ページ表引きがページあたり 1 回で済む */
static int user_range_ok(uint64_t va, uint64_t n)
{
    uint64_t end = va + n;
    while (va < end) {
        if (user_pa(va) == 0) {
            return 0;
        }
        va = (va & ~4095ULL) + 4096;
    }
    return 1;
}

/* ページごとに訳して、そのページの中は一気に写す版。
 * 意味は copy_from_user と同じ (訳せないページが
 * あれば -1)。大きな塊を運ぶ経路 (オーディオ) では、
 * バイトごとのページ表引き直しが実時間の予算を食い
 * 潰すので、こちらを使う (doc/audio-driver-design.md
 * の追補 2) */
static int copy_from_user_bulk(unsigned char *dst, uint64_t va,
                               uint64_t n)
{
    while (n > 0) {
        uint64_t pa = user_pa(va);
        uint64_t room, take, i;
        if (pa == 0) {
            return -1;
        }
        room = 4096 - (va & 4095);
        take = (n < room) ? n : room;
        for (i = 0; i < take; i++) {
            dst[i] = *(unsigned char *)(pa + i);
        }
        dst += take;
        va += take;
        n -= take;
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
/* 走者の CPU 精算 (実体は走者の退避場所の後ろ) */
static void exec_charge_push(const char *path);
static void exec_charge_pop(void);

static void resume_parent(uint64_t *frame, uint64_t result)
{
    int i;
    exec_charge_pop();  /* 子の CPU を精算してから段を戻す */
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
    exec_charge_push(name);  /* 親の CPU を精算し、子の名前と
                              * CPU を新しい段に用意する */
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
    /* 前面の窓のタイトルを実行したプログラム名へ
     * (画面表示 段B。do_exec は前面からしか呼ばれない。
     * 戻っても戻さない割り切り) */
    gfx_fg_title(name);
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
#define JOB_SLEEPING 4 /* sleep_ms で寝ている (sleep_until まで) */

struct job_slot {
    uint64_t frame[32];   /* 退避したレジスタ一式 */
    uint64_t sepc;
    uint64_t sp;          /* ユーザーの sp (sscratch の中身) */
    uint64_t status;      /* JOB_DONE のときの終了ステータス */
    int state;
    char name[32];        /* jobs 表示用 (起動時に渡された名前) */
    char cwd[PATH_MAX];
    uint64_t cpu;         /* 使った CPU 時間の積算 (time CSR の刻み) */
    uint64_t sleep_until; /* JOB_SLEEPING の起床時刻 (time CSR) */
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
    int sleeping;  /* sleep_ms で寝ている */
    uint64_t sleep_until; /* 前面の起床時刻 (time CSR) */
    uint64_t getc_until;  /* getc タイムアウトの期限 (0 = 無限) */
} fg_save;

static int cur_runner = 0;     /* 今走っている走者 (0 = 前面) */
static int console_owner = 0;  /* コンソールの持ち主 (0 = 前面) */

/* 前面の持ち主が SYS_GETC の待ちの中にいるか
 * (スクリーンセーバーの段2 判定用、doc/screensaver-
 * design.md)。持ち主の getc ポーリングループの入口で
 * 立て、文字が返るときに落とす。ジョブへ番を渡して
 * 止まっている間も立ったまま (前面は getc の中、という
 * 事実は変わらないため) */
static int fg_getc_wait;

/* --- 走者の CPU 時間と名前 (top の材料) ------------------------------
 * 走者が切り替わる地点 (restore_runner、exec の段の出入り)
 * で、前回の精算からの分を今の走者へ足す。前面は exec の
 * 段ごとに名前と CPU を持つ (段 0 = 根のプログラム)。
 * 前面が getc の待ちに入っている間と、止まっている
 * ジョブの分は数えない (待機は CPU 使用ではない) */
static char fg_name[EXEC_MAX_DEPTH + 1][32] = { "nish" };
static uint64_t fg_cpu[EXEC_MAX_DEPTH + 1];
static uint64_t runner_charge_last;
static int runner_charge_started;

static void copy_str(char *dst, const char *src, int max);

static void charge_runner(void)
{
    uint64_t now = r_time();
    if (!runner_charge_started) {
        runner_charge_started = 1;
        runner_charge_last = now;
        return;
    }
    if (cur_runner == 0) {
        if (!fg_getc_wait && !fg_save.blocked && !fg_save.sleeping) {
            fg_cpu[exec_depth] += now - runner_charge_last;
        }
    } else {
        struct job_slot *j = &jobs[cur_runner - 1];
        if (j->state == JOB_RUNNING) {
            j->cpu += now - runner_charge_last;
        }
    }
    runner_charge_last = now;
}

/* パスの最後の要素だけを取り出す (表示用の名前) */
static const char *base_name(const char *path)
{
    const char *b = path;
    const char *p;
    for (p = path; *p != '\0'; p++) {
        if (*p == '/') {
            b = p + 1;
        }
    }
    return b;
}

/* バッファへの追記ヘルパ (SYS_TASKS の走者行用) */
static int buf_put_ch(char *buf, int n, int max, char c)
{
    if (n < max) {
        buf[n++] = c;
    }
    return n;
}

static int buf_puts(char *buf, int n, int max, const char *s)
{
    while (*s != '\0') {
        n = buf_put_ch(buf, n, max, *s++);
    }
    return n;
}

static int buf_put_dec(char *buf, int n, int max, uint64_t v)
{
    char rev[20];
    int m = 0;
    if (v == 0) {
        rev[m++] = '0';
    }
    while (v > 0) {
        rev[m++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (m > 0) {
        n = buf_put_ch(buf, n, max, rev[--m]);
    }
    return n;
}

/* 走者の行をタスク一覧の後ろへ足す (SYS_TASKS から)。
 * 形は "run N: 名前 状態 cpu=ミリ秒" */
static int tasks_append_runners(char *buf, int n, int max)
{
    int r;
    n = buf_puts(buf, n, max, "run 0: ");
    n = buf_puts(buf, n, max, fg_name[exec_depth]);
    n = buf_put_ch(buf, n, max, ' ');
    n = buf_puts(buf, n, max,
                 fg_save.blocked ? "waiting" :
                 fg_save.sleeping ? "sleeping" :
                 fg_getc_wait ? "input" : "running");
    n = buf_puts(buf, n, max, " cpu=");
    n = buf_put_dec(buf, n, max,
                    fg_cpu[exec_depth] / (TIMER_HZ / 1000));
    n = buf_put_ch(buf, n, max, '\n');
    for (r = 1; r <= JOB_MAX; r++) {
        struct job_slot *j = &jobs[r - 1];
        if (j->state == JOB_FREE) {
            continue;
        }
        n = buf_puts(buf, n, max, "run ");
        n = buf_put_dec(buf, n, max, (uint64_t)r);
        n = buf_puts(buf, n, max, ": ");
        n = buf_puts(buf, n, max, j->name);
        n = buf_put_ch(buf, n, max, ' ');
        n = buf_puts(buf, n, max,
                     j->state == JOB_RUNNING ? "running" :
                     j->state == JOB_STOPPED ? "stopped" :
                     j->state == JOB_SLEEPING ? "sleeping" : "done");
        n = buf_puts(buf, n, max, " cpu=");
        n = buf_put_dec(buf, n, max, j->cpu / (TIMER_HZ / 1000));
        n = buf_put_ch(buf, n, max, '\n');
    }
    return n;
}

/* exec の段の出入り (do_exec / resume_parent から)。
 * push: 親の分を精算し、新しい段の名前と CPU を用意する。
 * pop: 子の分を精算する (名前と CPU は段に残るが、次に
 * その段へ入るとき push で用意し直す) */
static void exec_charge_push(const char *path)
{
    charge_runner();
    copy_str(fg_name[exec_depth + 1], base_name(path),
             sizeof(fg_name[0]));
    fg_cpu[exec_depth + 1] = 0;
}

static void exec_charge_pop(void)
{
    charge_runner();
}

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
    charge_runner();  /* 切り替わる前に今の走者へ精算する */
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
        if (j->state == JOB_SLEEPING) {
            j->state = JOB_RUNNING;  /* 寝ていたジョブの再開 */
        }
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
        if (fg_save.blocked) {
            return 0;
        }
        /* 寝ている前面は、起床時刻を過ぎたら実行可能に戻る */
        return !fg_save.sleeping || r_time() >= fg_save.sleep_until;
    }
    if (jobs[r - 1].state == JOB_SLEEPING) {
        return r_time() >= jobs[r - 1].sleep_until;
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

/* スクリーンセーバー (saver.c) への走者状態の公開
 * (doc/screensaver-design.md の段2 の定義)。前面が
 * シェルの入力待ちとは「持ち主が前面で、前面の段が
 * 根 (exec_depth == 0) で、その根が getc の待ちの中に
 * いる」こと。子プログラムの入力待ちは数に入らない */
int trap_saver_env(void)
{
    int env = 0;
    if (jobs_any_active()) {
        env |= TRAP_SAVER_JOBS_PRESENT;
    }
    if (console_owner == 0 && cur_runner == 0 && exec_depth == 0 &&
        fg_getc_wait) {
        env |= TRAP_SAVER_FG_SHELL_WAIT;
    }
    return env;
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

    gfx_window_done(jobno);  /* ジョブの窓を終了の見た目に (段B) */

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
        gfx_window_close(jobno);  /* 回収まで済んだので窓も消す */
        gfx_set_focus(0);         /* フォーカスは前面へ戻る */
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
            gfx_window_close(i + 1);  /* ジョブの窓も消す (段B) */
        }
    }
    fg_save.valid = 0;
    fg_save.blocked = 0;
    fg_save.wait_job = 0;
    fg_save.sleeping = 0;
    fg_save.sleep_until = 0;
    fg_save.getc_until = 0;
    console_owner = 0;
    cur_runner = 0;
    gfx_set_focus(0);
    /* 走者の CPU と名前もセッションごとに戻す */
    for (i = 0; i <= EXEC_MAX_DEPTH; i++) {
        fg_cpu[i] = 0;
        fg_name[i][0] = '\0';
    }
    copy_str(fg_name[0], "nish", sizeof(fg_name[0]));
    runner_charge_started = 0;
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
        j->cpu = 0;
        j->sleep_until = 0;
        j->state = JOB_RUNNING;
        gfx_window_open(slot + 1, j->name);  /* ジョブの窓 (段B) */
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
        : j->state == JOB_SLEEPING ? "sleeping"
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
        gfx_window_close((int)num);  /* 窓も消す (段B) */
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
    gfx_set_focus((int)num);  /* 持ち主の窓にフォーカス (段B) */
    return 1;
}

/* --- オーディオ (デバイス、40〜42) ----------------------------------
 * 仕様は doc/syscalls.md (Ikesu 段2の確定仕様。Ikesu 側の
 * doc/audio-syscall.md が原案)。カーネルは「サンプルの流れを
 * デバイスへ渡す」ことだけを知り、形式は符号付き 16bit
 * リトルエンディアン、インターリーブ固定。同時に開ける
 * 流れは 1 本だけ。
 *
 * 実体はボードで違う (doc/audio-driver-design.md):
 *   - virt: ダミー。audio_write は待たずに全部受け付ける
 *   - K210: boards/k210/i2s.c の I2S ドライバが実デバイス
 *     へ送る (再生のペーシングで待つ)
 * どちらのボードでも、受けたバイト列の FNV-1a 64bit
 * ハッシュと総サンプル数をここで貯めておき、audio_close
 * のときにカーネルログへ 1 行残す。テストは Nish の
 * dmesg 経由でこの行を照合する (テスト専用 syscall は
 * 作らない)。rate の対応可否はボード側が決める: ダミーは
 * 検証せず任意の rate を受け、K210 のドライバは対応
 * できない組み合わせで -1 を返し、open 自体が失敗する。 */

#define FNV1A64_OFFSET 14695981039346656037ULL /* 0xcbf29ce484222325 */
#define FNV1A64_PRIME 1099511628211ULL         /* 0x100000001b3 */

static int audio_open_flag;      /* 1 = 開いている */
static uint64_t audio_rate;      /* audio_open の a0 の保持 */

/* audio.h の口: 再生中かどうかを共通コードへ返す */
int audio_is_open(void)
{
    return audio_open_flag;
}
static uint64_t audio_channels;  /* 1 または 2 */
static uint64_t audio_fnv;       /* 受けたバイト列の FNV-1a 64bit */
static uint64_t audio_samples;   /* 受け付けた総サンプル数 */
static int audio_vol_shift = 1;  /* 音量シフト (syscall 50、既定は半分) */

#if BOARD_FEATURE_AUDIO
/* --- リングバッファと供給 (追補3 + DMA 化) --------------------------
 * I2S の FIFO は 8 フレーム (約 0.18ms) しかなく、書き手が
 * 少しでも手を止めると枯れる。そこで audio_write はまず
 * カーネルのリングへ写すだけにして即座に戻り、FIFO への
 * 供給は汲み手が担う。
 *
 * リングの中身は DMA の語列 (1 ワード = 1 チャンネル
 * サンプル、下位 16 ビットにサンプル)。1 フレーム =
 * 2 ワード = 8 バイトで、モノラルは投入側で左右へ複製
 * してから入れる。divide_16 の詰め形 (1 ワードに左右)
 * は使わず、SDK の 32 ビット経路と同じ形に揃えた。
 *
 * 汲み手は 2 形態 (ストリームごとに open で選ぶ):
 *   - DMA 供給 (通常、doc/audio-dma-design.md): DMAC が
 *     リングを直接読む。管理 (ブロックの完了確認と次の
 *     装填) は hart0 が担い、タイマ割り込み (100µs) と
 *     syscall 文脈 (write の待ち・close の drain) から
 *     回す。CPU がサンプルを運ばないので、補充の窓に
 *     よる周期クリックが消えた (2026-11-21 実測)
 *   - 割り込みフィーダ (落としどころ): タイマ割り込みが
 *     100µs ごとに try_push で汲む。AUDIO_DMA_OFF の
 *     ビルドや DMA が使えない場合用
 *
 * 索引は head (食べ手だけが書く) と tail (作り手だけが
 * 書く) の 2 本で、入っている量は差分で出す (共有の
 * count は持たない)。満杯と空の混同を避けるため、使える
 * のは全容量 - 8 バイトまで。
 * DMAC はキャッシュを見ないので、DMA 形態の公開は
 * 「データを書く → 追い出す → tail を進める」。管理は
 * tail が進んだ分しか装填しないので、装填時点でデータ
 * は必ずメモリにある */
#define AUDIO_RING_BYTES 65536u  /* 2 の冪。8192 フレーム = 約 186ms 分 */
#define AUDIO_RING_MASK  (AUDIO_RING_BYTES - 1)
#define AUDIO_FEED_TICKS (TIMER_HZ / 10000)  /* フィード間隔 100µs */

static unsigned char audio_ring[AUDIO_RING_BYTES]
    __attribute__((aligned(8)));
static volatile uint64_t audio_ring_head;  /* 食べ手の読み位置 */
static volatile uint64_t audio_ring_tail;  /* 作り手の書き位置 */
static volatile uint64_t audio_underruns;  /* 無音を流したフレーム数 */
static int audio_stream_live;      /* 1 フレームでも流れたら 1 */

static int audio_dma_active;  /* このストリームは DMA が汲む */

/* DMA 管理の状態。dma_head は DMA が読み終えた位置で、
 * ブロック完了のたびに audio_ring_head へ写す (head =
 * 消費済みの印。作り手の満杯判定は head で見る)。
 * ブロックは最大 1024 フレーム (約 23ms) で、リングの
 * 折り返しは跨がない */
#define AUDIO_DMA_BLOCK_FRAMES 1024
/* 無音ブロックは短くする。長くすると、その間に届いた
 * 実データがブロックの終わりまで待たされ、作り手と
 * 食べ手の位相がずれて空転が自己持続する (2026-11-21
 * の nami 実測: 1024 フレームの無音を 206 回 = 約 4.8 秒
 * 分挟んだ)。64 フレーム (約 1.5ms) なら待ちは聞こえず、
 * 位相もすぐ戻る */
#define AUDIO_SILENCE_FRAMES 64
static uint32_t audio_silence_block[AUDIO_DMA_BLOCK_FRAMES];
static uint64_t dma_head;
static int dma_block_active;   /* 張っているブロックがある */
static int dma_block_silence;  /* 張っているのは無音ブロック */
static uint64_t dma_block_frames;
static int dma_started;        /* 実データを 1 回でも流した */
static int audio_dma_closing;  /* close の drain 中 */

/* 装填遅れ (late) の計装 (doc/realtime-audio-design.md
 * 段0、doc/audio-dma-design.md 追補)。張ったブロックが
 * 再生を終えるはずの時刻 (dma_block_due) に対して、
 * 完了の観測がどれだけ遅れたかを数える。観測は
 * audio_dma_step が担うが、syscall の実行中 (SIE 落ち)
 * は誰も step を回さないので、長い syscall を跨いだ
 * ブロックは観測が遅れ、その間に I2S FIFO が枯れて
 * 音が途切れても under には乗らない。その見えない
 * 途切れを数字にするための計装で、再生の挙動は変えない。
 * 遅れが FIFO の残量 (約 8 フレーム ≒ 180µs) 以内なら
 * DAC は飢えないので、それを超えた分だけを数える:
 *   late     = 閾値を超えて遅れたブロックの数
 *   late_us  = 超過分の合計 (µs)。DAC が飢えた時間の見積り
 *   late_max = 1 ブロックの最大の遅れ (µs、閾値引き前) */
#define AUDIO_LATE_FIFO_US 180
static uint64_t dma_block_due;     /* 張ったブロックの完了予定 (r_time) */
static uint64_t audio_late_count;  /* 閾値を超えて遅れたブロック数 */
static uint64_t audio_late_us;     /* 超過分の合計 (µs) */
static uint64_t audio_late_max_us; /* 最大の遅れ (µs) */

/* ブロックを張る時に完了予定を立てる。所要時間は
 * ボードの実効レート (K210 では 44010Hz) で計算する。
 * 要求レート (44100) で計算すると 1 ブロックあたり
 * 約 46µs の系統誤差が late に混ざるので、必ず実効
 * レートを使う (不明な時だけ要求レートで代用) */
static void dma_arm_set_due(uint64_t frames)
{
    uint64_t rate = board_audio_actual_rate();
    if (rate == 0) {
        rate = audio_rate;
    }
    if (rate == 0) {
        dma_block_due = 0;
        return;
    }
    dma_block_due = r_time() + frames * TIMER_HZ / rate;
}

/* ブロックの完了を観測した時に遅れを帳簿へ入れる */
static void dma_note_late(void)
{
    uint64_t now = r_time();
    uint64_t late_us;
    if (dma_block_due == 0 || now <= dma_block_due) {
        return;
    }
    late_us = (now - dma_block_due) * 1000000 / TIMER_HZ;
    if (late_us <= AUDIO_LATE_FIFO_US) {
        return;
    }
    audio_late_count++;
    audio_late_us += late_us - AUDIO_LATE_FIFO_US;
    if (late_us > audio_late_max_us) {
        audio_late_max_us = late_us;
    }
}

/* リングに入っているバイト数 (索引の差分) */
static uint64_t ring_used(void)
{
    return (audio_ring_tail - audio_ring_head) & AUDIO_RING_MASK;
}

/* 次にタイマを張る時刻: 本来の 100ms 刻みとフィード刻みの
 * 早い方。フィード刻みは DMA 管理と割り込みフィーダの
 * 両方が要る */
static uint64_t timer_arm_value(void)
{
    uint64_t feed = r_time() + AUDIO_FEED_TICKS;
    if (audio_open_flag && feed < next_deadline) {
        return feed;
    }
    return next_deadline;
}

/* リングの先頭 1 フレームを読む (位置は進めない)。
 * リングは語列 (1 ワード = 1 チャンネルサンプル) なので
 * 1 フレーム = 8 バイト。サンプルは各ワードの下位
 * 16 ビット。1 フレーム分に足りなければ 0 */
static int ring_peek_frame(uint16_t *l, uint16_t *r)
{
    uint64_t h = audio_ring_head;
    if (ring_used() < 8) {
        return 0;
    }
    *l = (uint16_t)audio_ring[h];
    *l |= (uint16_t)((uint16_t)audio_ring[(h + 1) &
                                          AUDIO_RING_MASK] << 8);
    *r = (uint16_t)audio_ring[(h + 4) & AUDIO_RING_MASK];
    *r |= (uint16_t)((uint16_t)audio_ring[(h + 5) &
                                          AUDIO_RING_MASK] << 8);
    return 1;
}

/* 先頭 1 フレームを捨てる (流し終えた分) */
static void ring_drop_frame(void)
{
    audio_ring_head = (audio_ring_head + 8) & AUDIO_RING_MASK;
}

/* リングから 1 フレームを取り出す */
static int ring_pop_frame(uint16_t *l, uint16_t *r)
{
    if (!ring_peek_frame(l, r)) {
        return 0;
    }
    ring_drop_frame();
    return 1;
}

/* DMA 管理: 完了したブロックの後始末と、次のブロックの
 * 装填。タイマ割り込み (audio_feed) と syscall 文脈
 * (write の待ち、close の drain) の両方から呼ばれる。
 * syscall 文脈では割り込みが走らない作りなので、二重に
 * 回ることはない */
static void audio_dma_step(void)
{
    if (dma_block_active) {
        if (!board_audio_dma_done()) {
            return;
        }
        dma_note_late();
        if (dma_block_silence) {
            audio_underruns += dma_block_frames;
        } else {
            dma_head = (dma_head + dma_block_frames * 8) &
                       AUDIO_RING_MASK;
            audio_ring_head = dma_head;
            board_audio_dma_note_frames(dma_block_frames);
            audio_stream_live = 1;
        }
        dma_block_active = 0;
    }
    if (ring_used() >= 8) {
        uint64_t frames = ring_used() / 8;
        uint64_t contig = (AUDIO_RING_BYTES - dma_head) / 8;
        /* 最初の装填だけは 1 ブロック分たまるまで待つ
         * (作り手に先行量を持たせる)。close の drain
         * では端数でも流す */
        if (!dma_started && !audio_dma_closing &&
            frames < AUDIO_DMA_BLOCK_FRAMES) {
            return;
        }
        if (frames > contig) {
            frames = contig;
        }
        if (frames > AUDIO_DMA_BLOCK_FRAMES) {
            frames = AUDIO_DMA_BLOCK_FRAMES;
        }
        board_audio_dma_arm(
            (const uint32_t *)(audio_ring + dma_head), frames * 2);
        dma_block_active = 1;
        dma_block_silence = 0;
        dma_block_frames = frames;
        dma_arm_set_due(frames);
        dma_started = 1;
    } else if (dma_started && !audio_dma_closing) {
        /* 実データが流れ始めた後の空転は無音でつなぐ
         * (DAC を飢えさせない)。無音の分は完了時に
         * アンダーランとして数える */
        board_audio_dma_arm(audio_silence_block,
                            AUDIO_SILENCE_FRAMES * 2);
        dma_block_active = 1;
        dma_block_silence = 1;
        dma_block_frames = AUDIO_SILENCE_FRAMES;
        dma_arm_set_due(AUDIO_SILENCE_FRAMES);
    }
}

/* フィーダ (タイマ割り込み文脈): FIFO に空きがある間、
 * リングから流す。リングが空で、既に流れ始めているなら
 * 無音を 1 フレームだけ流して DAC を飢えさせず、
 * アンダーランとして数える。DMA 形態では管理だけ回す */
static void audio_feed(void)
{
    int i;
    if (audio_dma_active) {
        audio_dma_step();
        return;
    }
    for (i = 0; i < 32; i++) {
        uint16_t l, r;
        if (ring_peek_frame(&l, &r)) {
            if (!board_audio_try_push(l, r)) {
                break;  /* FIFO は満ちている (フレームは残す) */
            }
            ring_drop_frame();
            audio_stream_live = 1;
        } else {
            if (audio_stream_live &&
                board_audio_try_push(0, 0)) {
                audio_underruns++;
            }
            break;
        }
    }
}

/* ポンプ (syscall 文脈): リングから最大 32 フレームを、
 * FIFO の空きを待ちながら流す。待ちでは sched_poll() で
 * カーネルタスクへ番を渡す (従来の i2s_push_frame と
 * 同じ作法)。戻り値は流したフレーム数 */
static int ring_pump_some(void)
{
    int done = 0;
    while (done < 32) {
        uint16_t l, r;
        uint32_t spin = 0;
        if (!ring_pop_frame(&l, &r)) {
            break;
        }
        while (!board_audio_try_push(l, r)) {
            if ((++spin & 0x3FF) == 0) {
                sched_poll();
            }
        }
        audio_stream_live = 1;
        done++;
    }
    return done;
}

/* 塊をリングへ入れる。満杯なら自分で汲み出して空きを
 * 作る (syscall 文脈ではフィーダが走らないので、待つ
 * だけでは空かない)。割り込みフィーダ形態専用 */
static void ring_enqueue(const unsigned char *buf, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++) {
        while (ring_used() >= AUDIO_RING_BYTES - 8) {
            if (ring_pump_some() == 0) {
                sched_poll();
            }
        }
        audio_ring[audio_ring_tail] = buf[i];
        audio_ring_tail = (audio_ring_tail + 1) & AUDIO_RING_MASK;
    }
}

/* 塊をリングへ入れる (DMA 形態)。汲むのは DMAC なので、
 * 満杯なら head (DMA の消費位置) が進むのを待つ。待ちの
 * 間は管理を自分で回す (syscall 文脈ではタイマ側の管理
 * が走らない)。
 * 共有の tail はここでは進めない: データだけをローカル
 * な書き位置 (audio_tail_local) で書き進め、公開は
 * 呼び手 (SYS_AUDIO_WRITE) が「追い出す → tail を
 * 進める」の順で行う。こうすると、管理が新しい tail を
 * 見た時点でデータは必ずメモリにある (DMAC は
 * キャッシュを見ない) */
static uint64_t audio_tail_local;

static void ring_enqueue_dma(const unsigned char *buf, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++) {
        while (((audio_tail_local - audio_ring_head) &
                AUDIO_RING_MASK) >= AUDIO_RING_BYTES - 8) {
            audio_dma_step();
            sched_poll();
        }
        audio_ring[audio_tail_local] = buf[i];
        audio_tail_local = (audio_tail_local + 1) & AUDIO_RING_MASK;
    }
}

/* 音量の下げ幅 (ビットシフト)。基板2 のアンプは素の
 * 振幅だと大きすぎるので、投入側で一律に落とす
 * (2026-11-21、ユーザーの希望)。1 = 半分 (約 -6dB)。
 * 2026-11-23 からは syscall 50 (audio_set_volume) で
 * 実行時に変えられる (表口は vol.elf)。既定は 1。
 * FNV の帳簿はユーザーの元のバイト列で数える (write
 * 側) ので、検証値には影響しない。
 * 変数そのものは上の状態変数の並び (BOARD_FEATURE_AUDIO
 * の外) に置く: virt (ダミー) でも syscall 50 は受ける */

/* ユーザーの塊 (サンプル列) をリングの形 (語列) に
 * して入れる。1 サンプル = 1 ワード (下位 16 ビット)、
 * モノラルは左右へ複製し、音量シフトを全サンプルに
 * かける。塊のバイト数は常に偶数 (write の区切りが
 * 偶数) */
static void ring_enqueue_packed(const unsigned char *buf, uint64_t chunk)
{
    unsigned char pbuf[1024];
    uint64_t j;
    if (audio_channels == 1) {
        for (j = 0; j < chunk; j += 2) {
            int16_t s = (int16_t)((uint16_t)buf[j] |
                                  ((uint16_t)buf[j + 1] << 8));
            s = (int16_t)(s >> audio_vol_shift);
            pbuf[j * 4] = (unsigned char)(s & 0xFF);
            pbuf[j * 4 + 1] = (unsigned char)((s >> 8) & 0xFF);
            pbuf[j * 4 + 2] = 0;
            pbuf[j * 4 + 3] = 0;
            pbuf[j * 4 + 4] = pbuf[j * 4];
            pbuf[j * 4 + 5] = pbuf[j * 4 + 1];
            pbuf[j * 4 + 6] = 0;
            pbuf[j * 4 + 7] = 0;
        }
        if (audio_dma_active) {
            ring_enqueue_dma(pbuf, chunk * 4);
        } else {
            ring_enqueue(pbuf, chunk * 4);
        }
    } else {
        for (j = 0; j < chunk; j += 2) {
            int16_t s = (int16_t)((uint16_t)buf[j] |
                                  ((uint16_t)buf[j + 1] << 8));
            s = (int16_t)(s >> audio_vol_shift);
            pbuf[j * 2] = (unsigned char)(s & 0xFF);
            pbuf[j * 2 + 1] = (unsigned char)((s >> 8) & 0xFF);
            pbuf[j * 2 + 2] = 0;
            pbuf[j * 2 + 3] = 0;
        }
        if (audio_dma_active) {
            ring_enqueue_dma(pbuf, chunk * 2);
        } else {
            ring_enqueue(pbuf, chunk * 2);
        }
    }
}

#endif /* BOARD_FEATURE_AUDIO */

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
    case SYS_SLEEP_MS: {
        /* 走者を寝かせる (doc/top-design.md 段2)。前面は
         * getc 待ちと同じ作法 (文脈は ecall の位置で保存
         * し、再開時に syscall をやり直す)。ジョブは
         * JOB_SLEEPING で止める。寝ている間は CPU 積算に
         * 数えない (charge_runner が旗と状態を見て外す) */
        uint64_t ms = frame[FRAME_A0];
        if (ms == 0) {
            frame[FRAME_A0] = 0;
            break;
        }
        if (cur_runner == 0) {
            if (!fg_save.sleeping) {
                charge_runner();  /* 寝る前に走っていた分を精算 */
                fg_save.sleep_until = r_time() +
                    ms * (TIMER_HZ / 1000);
                fg_save.sleeping = 1;
            }
            for (;;) {
                int k, n, target = 0;
                if (r_time() >= fg_save.sleep_until) {
                    charge_runner();  /* 寝ていた間は数えない */
                    fg_save.sleeping = 0;
                    fg_save.sleep_until = 0;
                    frame[FRAME_A0] = 0;
                    break;
                }
                for (k = 0; k < JOB_MAX; k++) {
                    n = k + 1;
                    if (runner_runnable(n)) {
                        target = n;
                        break;
                    }
                }
                if (target != 0) {
                    save_runner(frame, r_sepc());
                    restore_runner(frame, target);
                    return 1;
                }
                sched_poll();
            }
            break;
        }
        {
            struct job_slot *j = &jobs[cur_runner - 1];
            int next;
            if (j->sleep_until == 0) {
                charge_runner();  /* 寝る前に走っていた分を精算 */
                j->sleep_until = r_time() +
                    ms * (TIMER_HZ / 1000);
            }
            if (r_time() >= j->sleep_until) {
                j->sleep_until = 0;
                frame[FRAME_A0] = 0;
                break;
            }
            if (console_owner != cur_runner &&
                runner_runnable(console_owner)) {
                next = console_owner;
            } else {
                next = pick_next_runner();
            }
            if (next != cur_runner) {
                save_runner(frame, r_sepc());
                j->state = JOB_SLEEPING;
                restore_runner(frame, next);
                return 1;
            }
            /* 誰も走れない: その場でポーリングしながら待つ
             * (スケジューラは回るので時計タスクは進む) */
            j->state = JOB_SLEEPING;
            while (r_time() < j->sleep_until) {
                sched_poll();
            }
            j->state = JOB_RUNNING;
            j->sleep_until = 0;
            frame[FRAME_A0] = 0;
            break;
        }
    }
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
        charge_runner();  /* 待ちに入る前に走っていた分を精算 */
        fg_getc_wait = 1;
        /* タイムアウト (a1 ミリ秒、0 = 無限) は持ち主の
         * 待ちにだけ付ける。期限は fg_save に持ち、ジョブへ
         * 番が移って戻ってきても数え直さない */
        if (frame[FRAME_A1] != 0 && fg_save.getc_until == 0) {
            fg_save.getc_until = r_time() +
                frame[FRAME_A1] * (TIMER_HZ / 1000);
        }
        for (;;) {
            int c = uart_try_getc();
            if (c >= 0) {
                charge_runner();  /* 待ちの間は数えない (旗が
                                   * 立っている間に精算する) */
                fg_getc_wait = 0;
                fg_save.getc_until = 0;
                /* 入力活動 (スクリーンセーバーの物差し。
                 * セーバーが入っていれば、ここで終わって
                 * 画面が戻ってから文字が配られる) */
                saver_note_activity();
                frame[FRAME_A0] = (uint64_t)(unsigned char)c;
                break;
            }
            if (fg_save.getc_until != 0 &&
                r_time() >= fg_save.getc_until) {
                charge_runner();  /* 待ちの間は数えない */
                fg_getc_wait = 0;
                fg_save.getc_until = 0;
                frame[FRAME_A0] = (uint64_t)-1;
                break;
            }
            /* 文字が無い間は、実行可能なジョブに番を回す。
             * 自分の文脈は ecall の位置で保存しておき、
             * 戻ってきたら getc を再実行する */
            {
                int k, n, target = 0;
                for (k = 0; k < JOB_MAX; k++) {
                    n = (cur_runner + k) % JOB_MAX + 1;
                    if (n != cur_runner && runner_runnable(n)) {
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
        char kbuf[1024];
        int n;
        proc_charge_now();  /* カーネルタスクの CPU を精算 */
        charge_runner();    /* 走者の CPU も精算 */
        n = proc_list(kbuf, sizeof(kbuf));
        n = tasks_append_runners(kbuf, n, sizeof(kbuf));
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
            resolve_path(name, abs) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (str_eq(abs, proc_cwd())) {
            frame[FRAME_A0] = (uint64_t)-1;  /* 自分の cwd は消せない */
            break;
        }
        if (is_sd_abs(abs)) {
            /* SD 側も消せる (fat_rmdir)。自分の cwd の点検は
             * 上で RAM 側と共通に済ませてある。他の走者の
             * cwd や、cwd の祖先にあたるディレクトリは
             * RAM FS と同じく未対策 (割り切り。経緯は
             * doc/sd-write-design.md の追補) */
            frame[FRAME_A0] = (uint64_t)fat_rmdir(sd_rel(abs));
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
             * どちらも「fd の現在位置へ書く」意味づけは同じ
             * (位置指定の書き込み、FS 再設計 段3) */
            w = fd_table[fd].is_sd
                    ? fat_write_at(&fd_table[fd].fat,
                                   fd_table[fd].pos + done, buf, chunk)
                    : fs_write_at(fd_table[fd].file,
                                  fd_table[fd].pos + done, buf, chunk);
            done += w;
            if (w < chunk) {
                break;  /* 容量いっぱい */
            }
        }
        if (done != (uint64_t)-1) {
            fd_table[fd].pos += done;
            frame[FRAME_A0] = done;
        }
        break;
    }
    case SYS_LSEEK: {
        /* a0 = fd、a1 = オフセット (符号付き)、a2 = 基準
         * (0 = 先頭、1 = 現在位置、2 = 末尾)。新しい位置を
         * a0 に返す。結果が負になる移動・基準の不正・fd の
         * 不正は -1 で、そのとき位置は変わらない。末尾より
         * 後にも動かせる (書かずに閉じればファイルは伸び
         * ない)。仕様は doc/syscalls.md の 17 番 */
        uint64_t fd = frame[FRAME_A0];
        int64_t off = (int64_t)frame[FRAME_A1];
        int64_t base, np;

        if (fd >= MAX_FD || !fd_table[fd].used) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        if (frame[FRAME_A2] == 0) {
            base = 0;
        } else if (frame[FRAME_A2] == 1) {
            base = (int64_t)fd_table[fd].pos;
        } else if (frame[FRAME_A2] == 2) {
            base = (int64_t)(fd_table[fd].is_sd
                                 ? fd_table[fd].fat.size
                                 : fd_table[fd].file->size);
        } else {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        np = base + off;
        if (np < 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        fd_table[fd].pos = (uint64_t)np;
        frame[FRAME_A0] = (uint64_t)np;
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
    case SYS_PERF_READ:
        /* a0 = 種類 (0 = サイクル数、1 = 実行命令数)。
         * カウンタの現在値 (64 ビット) を a0 に返す。
         * 不正な種類は -1。仕様は doc/syscalls.md の 33 番 */
        frame[FRAME_A0] = trap_perf_read(frame[FRAME_A0]);
        break;
    case SYS_AUDIO_OPEN: {
        /* a0 = サンプルレート (Hz)、a1 = チャンネル数
         * (1 または 2)。同時に開けるのは 1 本だけ */
        uint64_t channels = frame[FRAME_A1];
        if (audio_open_flag || (channels != 1 && channels != 2)) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
#if BOARD_FEATURE_AUDIO
        /* 実ドライバが対応できない組み合わせは -1 で、
         * open 自体を失敗させる */
        if (board_audio_open(frame[FRAME_A0], channels) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
#endif
        audio_open_flag = 1;
        audio_rate = frame[FRAME_A0];
        audio_channels = channels;
        audio_fnv = FNV1A64_OFFSET;
        audio_samples = 0;
#if BOARD_FEATURE_AUDIO
        /* リングを空にし、供給の状態を初期化する。
         * DMA が使えるストリームでは DMAC が汲み、
         * いなければ割り込みフィーダが汲む。どちらも
         * フィード刻みでタイマを張り直す */
        audio_ring_head = 0;
        audio_ring_tail = 0;
        audio_tail_local = 0;
        audio_underruns = 0;
        audio_late_count = 0;
        audio_late_us = 0;
        audio_late_max_us = 0;
        dma_block_due = 0;
        audio_stream_live = 0;
        audio_dma_active = board_audio_dma_active();
        dma_head = 0;
        dma_block_active = 0;
        dma_block_silence = 0;
        dma_block_frames = 0;
        dma_started = 0;
        audio_dma_closing = 0;
        sbi_set_timer(timer_arm_value());
#endif
        frame[FRAME_A0] = 0;
        break;
    }
    case SYS_AUDIO_WRITE: {
        /* a0 = サンプル列へのポインタ、a1 = サンプル数
         * (int16 の個数)。受けたバイト列で FNV-1a を更新
         * しながら運ぶ。実ドライバのあるボードでは、塊
         * ごとにリングバッファへ入れる (追補3) */
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
         * ので、先に全範囲が触れることを確かめる。確認は
         * ページごと (バイトごとだと、最初のフレームを
         * 流す前にページ表引きだけで数 ms 食い、浅い
         * I2S FIFO が書き込みのたびに枯れる) */
        if (!user_range_ok(frame[FRAME_A0], nbytes)) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        /* ユーザーメモリから小分けに運びながらハッシュする */
        while (done < nbytes) {
            uint64_t chunk = nbytes - done;
            uint64_t i;
            if (chunk > sizeof(buf)) {
                chunk = sizeof(buf);
            }
            if (copy_from_user_bulk(buf, frame[FRAME_A0] + done,
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
#if BOARD_FEATURE_AUDIO
            /* 同じ塊をリングへ入れる。供給は汲み手 (DMA
             * か割り込みフィーダ) が担う。塊のバイト数は
             * 常に偶数 (全体が偶数で、区切りも偶数) */
            ring_enqueue_packed(buf, chunk);
#endif
            done += chunk;
        }
#if BOARD_FEATURE_AUDIO
        if (audio_dma_active && done != (uint64_t)-1) {
            /* データの公開: DMAC はキャッシュを見ない
             * ので、追い出してから tail を進める。tail
             * を読むのは同じ hart0 の管理だけなので、
             * 追い出しは 1 回で足りる */
            smp_evict_cache();
            audio_ring_tail = audio_tail_local;
        }
#endif
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
#if BOARD_FEATURE_AUDIO
        /* リングに残っている分を全部流してから閉じる。
         * フレームに満たない半端な残りは従来どおり捨てる */
        if (audio_dma_active) {
            /* DMA に食べ切らせる。無音ブロックが飛んで
             * いたら止めてよい (中身は 0)。上限 (2 秒) を
             * 超えたらチャンネルを止めて落とす (close は
             * 必ず完了させる) */
            uint64_t t0 = r_time();
            audio_dma_closing = 1;
            for (;;) {
                if (dma_block_active && dma_block_silence) {
                    board_audio_dma_stop();
                    dma_block_active = 0;
                }
                if (!dma_block_active &&
                    dma_head == audio_ring_tail) {
                    break;
                }
                audio_dma_step();
                if (r_time() - t0 > (uint64_t)TIMER_HZ * 2) {
                    break;
                }
                sched_poll();
            }
            board_audio_dma_stop();
            dma_block_active = 0;
            audio_dma_active = 0;
            audio_ring_head = audio_ring_tail;
        } else {
            /* 割り込みフィーダ形態 (追補3): 流し切る間は
             * open のままにして、フィーダにも手伝わせる */
            while (ring_used() >= 8u) {
                if (ring_pump_some() == 0) {
                    sched_poll();
                }
            }
            audio_ring_head = audio_ring_tail;
        }
        kputs("audio: ring under=");
        kput_dec(audio_underruns);
        kputs(" late=");
        kput_dec(audio_late_count);
        kputs(" late_us=");
        kput_dec(audio_late_us);
        kputs(" late_max=");
        kput_dec(audio_late_max_us);
        kputc('\n');
#endif
        audio_open_flag = 0;
#if BOARD_FEATURE_AUDIO
        board_audio_close();  /* ドライバ側の要約行を先に出す */
#endif
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
    case SYS_AUDIO_SET_VOLUME: {
        /* a0 = 新しい音量シフト (0〜4、範囲外は 4 に
         * 丸める)。負なら問い合わせのみ。戻り値は
         * 設定後のシフト値。仕様は doc/syscalls.md
         * の 50 番 */
        int64_t v = (int64_t)frame[FRAME_A0];
        if (v >= 0) {
            if (v > 4) {
                v = 4;
            }
            audio_vol_shift = (int)v;
        }
        frame[FRAME_A0] = (uint64_t)audio_vol_shift;
        break;
    }
    case SYS_GFX_INFO: {
        /* a0 = バッファ (16 バイト)。画面と自分の窓の情報を
         * u32 x4 で書く (doc/gfx-design.md) */
        uint32_t out[4];
        gfx_info(cur_runner, out);
        if (copy_to_user(frame[FRAME_A0],
                         (const unsigned char *)out, 16) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
        } else {
            frame[FRAME_A0] = 0;
        }
        break;
    }
    case SYS_GFX_CLEAR:
        /* a0 = 色 (RGB565)。自分の窓の内容領域を消す */
        gfx_clear(cur_runner, (uint32_t)frame[FRAME_A0]);
        frame[FRAME_A0] = 0;
        break;
    case SYS_GFX_RECT:
        /* a0 = x | (y<<16)、a1 = w | (h<<16)、a2 = 色。
         * 座標は窓の中の相対座標 (クリップは gfx 側) */
        gfx_rect(cur_runner,
                 (int)(frame[FRAME_A0] & 0xFFFF),
                 (int)(frame[FRAME_A0] >> 16),
                 (int)(frame[FRAME_A1] & 0xFFFF),
                 (int)(frame[FRAME_A1] >> 16),
                 (uint32_t)frame[FRAME_A2]);
        frame[FRAME_A0] = 0;
        break;
    case SYS_GFX_TEXT: {
        /* a0 = x | (y<<16)、a1 = 文字列、a2 = 色 |
         * (scale<<16)。文字列はカーネルに写してから描く
         * (最大 48 文字。写せなければ -1) */
        char buf[49];
        if (copy_user_string(frame[FRAME_A1], buf, sizeof(buf)) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        gfx_text(cur_runner,
                 (int)(frame[FRAME_A0] & 0xFFFF),
                 (int)(frame[FRAME_A0] >> 16),
                 buf,
                 (int)(frame[FRAME_A2] >> 16),
                 (uint32_t)frame[FRAME_A2]);
        frame[FRAME_A0] = 0;
        break;
    }
    case SYS_GFX_LINE:
        /* a0 = x0 | (y0<<16)、a1 = x1 | (y1<<16)、a2 = 色 */
        gfx_line(cur_runner,
                 (int)(frame[FRAME_A0] & 0xFFFF),
                 (int)(frame[FRAME_A0] >> 16),
                 (int)(frame[FRAME_A1] & 0xFFFF),
                 (int)(frame[FRAME_A1] >> 16),
                 (uint32_t)frame[FRAME_A2]);
        frame[FRAME_A0] = 0;
        break;
    case SYS_OLED_TEXT: {
        /* a0 = 文字列。消去してから描いて転送する
         * (doc/oled-design.md)。最大 175 文字 (8 行 x 21 文字
         * + 改行の合図)。写せなければ -1、OLED が無くても -1 */
        char buf[176];
        if (copy_user_string(frame[FRAME_A0], buf, sizeof(buf)) != 0) {
            frame[FRAME_A0] = (uint64_t)-1;
            break;
        }
        frame[FRAME_A0] = (uint64_t)oled_text_multi(buf);
        break;
    }
    case SYS_OLED_CLEAR:
        /* OLED を消去して転送する。無いときは -1 */
        frame[FRAME_A0] = (uint64_t)oled_clear_flush();
        break;
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

#if BOARD_FEATURE_AUDIO
    /* フィード専用の早いタイマ (追補3): 100ms の本刻みが
     * まだ来ていない割り込みは、FIFO への供給だけをして
     * 戻る。本刻みの仕事 (起床判定、tick、切り替え) は
     * 何もしない。トラップ状態の退避も、切り替えが無い
     * ので要らない */
    if (is_interrupt && code == 5 && r_time() < next_deadline) {
        if (audio_open_flag) {
            audio_feed();
        }
        sbi_set_timer(timer_arm_value());
        return;
    }
#endif

    /* M シムがエミュレーションしたトラップの記録を klog へ
     * 移す (k210 のみ実体あり。doc/trap-recovery-design.md) */
    board_drain_trap_log();

    /* スケジューリング中に割り込まれたら、このタスクの
     * sepc / sstatus を退避しておく (CSR は CPU に 1 組だけ
     * なので、切り替え先のタスクのトラップで上書きされる前に) */
    if (sched_active) {
        sched_save_trapstate(r_sepc(), r_sstatus());
    }

    if (is_interrupt && code == 5) {
        /* スーパーバイザ・タイマ割り込み (100ms ごと。
         * audio 再生中はフィード刻みも混ざるが、そちらは
         * 上の早い道で戻っている) */
        next_deadline += TIMER_INTERVAL;
#if BOARD_FEATURE_AUDIO
        sbi_set_timer(timer_arm_value());
#else
        sbi_set_timer(next_deadline);
#endif
        if (++tick_divider >= 10) {
            tick_divider = 0;
            ticks++;
            /* tick はコンソールだけ (ログには残さない)。
             * オーディオ再生中は印字を省く: 割り込み文脈で
             * UART の送信待ちに入ると、その間 I2S FIFO へ
             * 誰も供給できず音が欠ける (数え自体は続ける) */
            if (!audio_is_open()) {
                uart_puts("tick ");
                con_dec(ticks);
                uart_putc('\n');
            }
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
