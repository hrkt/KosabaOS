/* mshim.c -- K210 の薄い M モードシム (step 4)
 *
 * K210 には OpenSBI のようなファームウェアがいないので、
 * S モードカーネル (virt と共通の trap.c / vm.c / proc.c) が
 * 前提にする「M 側の仕事」だけをここで自前で担う:
 *   1. ページングの点火: 初期ページテーブルを作り、sptbr と
 *      mstatus.VM (= Sv39) を設定してから S モードの kmain へ
 *      mret で降りる (ドラフト Sv39 の作法は実験 probe2 で確認)
 *   2. SBI TIME: S からの ecall (a7 = 0x54494D45) で CLINT の
 *      mtimecmp を書き、STIP を消して戻る
 *   3. タイマ転送: MTI が来たら mip.STIP を立てて S へ知らせる
 *      (mideleg で STI を S に委譲してある)
 *   4. rdtime のエミュレーション: S では csrr time が違法命令に
 *      なるので、命令を読んで mtime を返す (probe4 で確認)
 *   5. 浮動小数点の土台: mstatus.FS を Initial にし、fcsr を
 *      初期化する (違法命令は S へ委譲されないので、土台は
 *      M 側で用意しないと FP 命令がここで止まる)
 * それ以外の例外は S へ委譲 (medeleg) し、カーネルの仕事は
 * すべて S 側に任せる。実験の記録は spike-k210/step4-usermode/。
 *
 * LCD の tick 表示はスケジューラ段2 で時計タスク (S 側、
 * proc.c の clock_body) の仕事になったので、ここ (M の
 * 割り込み文脈) では描かない。MTI の仕事は STIP の転送だけ。
 */

#include <stdint.h>
#include "board.h"
#include "uart.h"

#define REG64(addr) (*(volatile uint64_t *)(addr))
#define CLINT_MTIME    REG64(0x0200BFF8)
#define CLINT_MTIMECMP REG64(0x02004000)

#define SBI_EID_TIME 0x54494D45ULL  /* "TIME" (trap.c と同じ約束) */

/* ---- CSR アクセス -------------------------------------------------- */

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

static inline void w_mepc(uint64_t v)
{
    __asm__ volatile("csrw mepc, %0" :: "r"(v));
}

/* ---- 小さな表示部品 ------------------------------------------------ */

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

static void fatal(const char *what)
{
    uart_puts("\nmshim: ");
    uart_puts(what);
    uart_puts(" mcause=");
    put_hex(r_mcause());
    uart_puts(" mepc=");
    put_hex(r_mepc());
    uart_putc('\n');
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ---- 起動用ページテーブル ------------------------------------------
 * vm_init が自前のテーブルに切り替えるまでの仮の地図。
 * S の kmain が起動直後に必要とするものだけを恒等で割り当てる:
 *   root[0]: 0x00000000 の 1GB (CLINT / UARTHS / GPIOHS)
 *   root[1]: L1 経由で LCD 周辺 (FPIOA / SYSCTL / SPI0 の 2MB)
 *   root[2]: 0x80000000 の 1GB (SRAM、カーネルとスタック) */

#define PTE_V (1ULL << 0)
#define PTE_R (1ULL << 1)
#define PTE_W (1ULL << 2)
#define PTE_X (1ULL << 3)
#define PTE_A (1ULL << 6)
#define PTE_D (1ULL << 7)
#define LEAF(pa, flags) (((pa) >> 12) << 10 | (flags))
#define PTR(pa) (((pa) >> 12) << 10 | PTE_V)

static uint64_t boot_root[512] __attribute__((aligned(4096)));
static uint64_t boot_l1[512] __attribute__((aligned(4096)));

static void build_boot_table(void)
{
    const uint64_t RW = PTE_V | PTE_R | PTE_W | PTE_A | PTE_D;
    const uint64_t RWX = RW | PTE_X;
    int i;
    for (i = 0; i < 512; i++) {
        boot_root[i] = 0;
        boot_l1[i] = 0;
    }
    boot_root[0] = LEAF(0x00000000ULL, RW);
    boot_root[1] = PTR((uint64_t)boot_l1);
    boot_root[2] = LEAF(0x80000000ULL, RWX);
    boot_l1[0x81] = LEAF(0x50200000ULL, RW);  /* FPIOA 0x502B0000 */
    boot_l1[0x82] = LEAF(0x50400000ULL, RW);  /* SYSCTL 0x50440000 */
    boot_l1[0x90] = LEAF(0x52000000ULL, RW);  /* SPI0 */
}

/* ---- M 専用スタック (トラップ入口が mscratch で切り替える) -------- */

static unsigned char m_stack[4096] __attribute__((aligned(16)));
extern void mshim_entry(void);  /* mshim.S の入口 */
extern void kmain(void);        /* S モードで動くカーネル本体 */

/* start.S から呼ばれる。戻らずに S モードの kmain へ mret する */
void mshim_boot(void)
{
    uint64_t ms;

    build_boot_table();

    __asm__ volatile("csrw mtvec, %0" :: "r"((uint64_t)mshim_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((uint64_t)m_stack + sizeof(m_stack)));

    /* 例外は大半を S へ委譲する (ビット: 0,1,3,4,5,6,7,8,12,13,15)。
     * 委譲しないのは 2 (違法命令: rdtime の始末はこちら) と
     * 9 (S からの ecall = SBI の受け口) */
    __asm__ volatile("csrw medeleg, %0" :: "r"(0xB1FBULL));
    /* 割り込みは STI (bit5) だけ S へ委譲する */
    __asm__ volatile("csrw mideleg, %0" :: "r"(1ULL << 5));
    /* MTIE を立てないと MTI は S 実行中でも M に届かない */
    __asm__ volatile("csrs mie, %0" :: "r"(1ULL << 7));
    CLINT_MTIMECMP = ~0ULL;

    /* ページングの点火 (ドラフト Sv39): sptbr は CSR 0x180、
     * モード指定は mstatus の VM (bits 28:24) = 9 */
    __asm__ volatile("csrw 0x180, %0" :: "r"(((uint64_t)boot_root >> 12) & 0x3FFFFFFFFFFFULL));
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
    ms &= ~(0x1FULL << 24);
    ms |= (9ULL << 24);              /* VM = Sv39 */
    ms &= ~(3ULL << 11);
    ms |= (1ULL << 11);              /* MPP = S */
    ms &= ~(1ULL << 7);              /* MPIE = 0 */
    /* FS (bits 14:13) = Initial: S / U の浮動小数点を有効に
     * する。例外 2 (違法命令) は medeleg で S へ委譲されて
     * いないので、FS = Off のまま FP 命令が来るとここの
     * fatal 行きになる。S 側の sstatus.FS の設定 (user.S /
     * trap.c) だけでは土台の mstatus.FS が立っていないと
     * 効かないため、ここでも明示する */
    ms &= ~(3ULL << 13);
    ms |= (1ULL << 13);              /* FS = Initial */
    __asm__ volatile("csrw mstatus, %0" :: "r"(ms));
    /* ROM の残りで fcsr の丸めモード (frm) が予約値 111 に
     * なっていることがあり、そのままだと浮動小数点演算が
     * 違法命令になる (実測済み。SDK の start も fcsr = 0 で
     * 始める)。FS を有効にした直後に初期値へ戻す */
    __asm__ volatile("csrw fcsr, zero");
    __asm__ volatile(".word 0x10400073");  /* sfence.vm x0 */
    w_mepc((uint64_t)kmain);
    __asm__ volatile("mret");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ---- トラップの処理 ------------------------------------------------ */

/* レジスタ番号 -> 退避スロット (mshim.S の並び) */
static void set_frame_rd(uint64_t *tf, int rd, uint64_t v)
{
    static const int slot[32] = {
        -1, 0, -1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, -1, -1, 24, 25, 26, 27,
    };
    if (rd >= 0 && rd < 32 && slot[rd] >= 0) {
        tf[slot[rd]] = v;
    }
}

void mshim_handler(uint64_t *tf)
{
    uint64_t cause = r_mcause();

    if ((int64_t)cause < 0) {  /* 割り込み */
        if ((cause & 0xFF) == 7) {
            /* M タイマ割り込み: STIP を立てて S へ転送する。
             * 次の期限は S が SBI TIME で予約し直す */
            __asm__ volatile("csrs mip, %0" :: "r"(1ULL << 5));
            CLINT_MTIMECMP = ~0ULL;
        }
        return;  /* その他の割り込みは来ないはずだが、素通りする */
    }

    if (cause == 9) {  /* S からの ecall = SBI の受け口 */
        uint64_t a7 = tf[15];
        if (a7 == SBI_EID_TIME) {
            CLINT_MTIMECMP = tf[8];  /* a0 = when */
            __asm__ volatile("csrc mip, %0" :: "r"(1ULL << 5));
            tf[8] = 0;               /* 成功 */
        } else {
            tf[8] = (uint64_t)-2;    /* 未対応の拡張 */
        }
        w_mepc(r_mepc() + 4);        /* ecall の次へ戻す */
        return;
    }

    if (cause == 4 || cause == 6) {
        /* 境界にそろっていない読み (4) / 書き (6)。ドラフト仕様の
         * K210 では S へ委譲できず M に来る。いまはエミュレー
         * ションを持たないので、原因を明示して止める */
        fatal("misaligned load/store (not emulated)");
    }

    if (cause == 2) {  /* 違法命令: rdtime だけエミュレーション */
        uint64_t mp = r_mepc();
        /* mepc は 2 バイト境界のことがあるので 16bit ずつ読む。
         * S の住所は恒等マップなので M からそのまま読める */
        uint32_t ins = *(volatile uint16_t *)mp |
                       ((uint32_t)*(volatile uint16_t *)(mp + 2) << 16);
        if ((ins & 0xFFFFF07Fu) == 0xC0102073u) {  /* csrr rd, time */
            int rd = (int)((ins >> 7) & 0x1F);
            set_frame_rd(tf, rd, CLINT_MTIME);
            w_mepc(mp + 4);
            return;
        }
        fatal("illegal instruction");
    }

    fatal("unexpected trap");
}
