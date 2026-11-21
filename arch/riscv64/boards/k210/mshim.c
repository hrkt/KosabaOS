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
 *   4. カウンタ読みのエミュレーション: S では csrr time が
 *      違法命令になるので、命令を読んで mtime を返す
 *      (probe4 で確認)。同じ仕組みで cycle / instret も
 *      エミュレーションする (perf_read = syscall 33 の実機側
 *      の受け口。mcycle / minstret は M では直接読める
 *      ことがベンチ spike で確認済み)
 *   5. 浮動小数点の土台: mstatus.FS を Initial にし、fcsr を
 *      初期化する (違法命令は S へ委譲されないので、土台は
 *      M 側で用意しないと FP 命令がここで止まる)
 *   6. misaligned load/store のエミュレーション: ドラフト
 *      仕様では cause 4 / 6 が S へ委譲できず M に来る。
 *      整数の load/store をバイト単位に分解して実行し、
 *      プログラムを続行させる。検出は記録に残し、カーネルが
 *      klog へ drain する (doc/trap-recovery-design.md)
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
#include "klog.h"
#include "traplog.h"

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
    boot_l1[0x80] = LEAF(0x50000000ULL, RW);  /* DMAC 0x50000000 */
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

/* レジスタ番号 -> 退避スロット (mshim.S の並び)。
 * x2 (sp) はフレームに無く mscratch にあるので -2 で示す。
 * 注: 以前の表は s10 / s11 を落とし t3 以降が 2 つずれて
 * いた (csrr の宛先が a 系以外だと化ける潜在バグ)。
 * misaligned エミュレーションが全レジスタを読み書きする
 * のに合わせて、mshim.S の退避順と突き合わせて直した */
static const int reg_slot[32] = {
    -1, 0, -2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
};

static uint64_t get_frame_reg(uint64_t *tf, int r)
{
    uint64_t v;
    if (r == 0)
        return 0;
    if (r == 2) {
        __asm__ volatile("csrr %0, mscratch" : "=r"(v));
        return v;
    }
    return tf[reg_slot[r]];
}

static void set_frame_reg(uint64_t *tf, int r, uint64_t v)
{
    if (r == 0)
        return;
    if (r == 2) {
        /* mret の帰りに mscratch と交換されて sp になる */
        __asm__ volatile("csrw mscratch, %0" :: "r"(v));
        return;
    }
    tf[reg_slot[r]] = v;
}

static void set_frame_rd(uint64_t *tf, int rd, uint64_t v)
{
    if (rd > 0 && rd < 32)
        set_frame_reg(tf, rd, v);
}

/* ---- misaligned エミュレーション ------------------------------------
 * U / S の仮想アドレスを sptbr の Sv39 表で辿って物理化し、
 * バイト単位で読み書きする。表のメモリは物理 SRAM にあり
 * M から直接読める。変換できないアドレスはエミュレーション
 * 不可として呼び出し側に返す */

/* sptbr はドラフト仕様の CSR 0x180 (mshim_boot が書くのと同じ) */
static uint64_t r_sptbr(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, 0x180" : "=r"(v));
    return v;
}

static int va_to_pa(uint64_t va, uint64_t *pa)
{
    uint64_t table = (r_sptbr() & 0x3FFFFFFFFFFFULL) << 12;
    int level;
    for (level = 2; level >= 0; level--) {
        uint64_t idx = (va >> (12 + 9 * level)) & 0x1FF;
        uint64_t pte = *(volatile uint64_t *)(table + idx * 8);
        if (!(pte & 1ULL))
            return -1;                       /* 無効な項 */
        if (pte & 0xEULL) {                  /* 葉 (R / W / X) */
            uint64_t mask = (1ULL << (12 + 9 * level)) - 1;
            *pa = (((pte >> 10) << 12) & ~mask) | (va & mask);
            return 0;
        }
        table = (pte >> 10) << 12;
    }
    return -1;
}

static int read_byte_va(uint64_t va, uint8_t *out)
{
    uint64_t pa;
    if (va_to_pa(va, &pa) != 0)
        return -1;
    *out = *(volatile uint8_t *)pa;
    return 0;
}

static int write_byte_va(uint64_t va, uint8_t v)
{
    uint64_t pa;
    if (va_to_pa(va, &pa) != 0)
        return -1;
    *(volatile uint8_t *)pa = v;
    return 0;
}

/* ---- トラップ検出の記録 (カーネルが klog へ drain する) --------------
 * シムは klog を直接触らない。詳細 {cause, pc, addr} は
 * 直近 MSHIM_LOG_MAX 件のリングに残し、累計は別に数える。
 * (当初は先頭 8 件だけだったが、それだと最初のプログラム
 * で満杯になり、後から来る転送の記録が残らなかった。
 * 2026-11-19 の K29 で発覚してリングに直した)
 * drain は S モードのカーネルから呼ばれ、klog にだけ書く
 * (コンソールには出さない。このファイルの関数だが、実行は
 * 通常の C として S でもそのまま動く) */

#define MSHIM_LOG_MAX 8
static struct {
    uint64_t cause, pc, addr;
} mshim_log_recs[MSHIM_LOG_MAX];
static volatile uint32_t mshim_log_count;   /* 記録した総件数 */
static volatile uint64_t mshim_emu_total;   /* エミュレーション累計 */

/* cause に MSHIM_LOG_FORWARDED を重ねると「段2 でカーネルへ
 * 転送した」記録になる (drain 側で文言を変える) */
#define MSHIM_LOG_FORWARDED 0x100ULL

static void mshim_log_push(uint64_t cause, uint64_t pc, uint64_t addr)
{
    uint32_t slot = mshim_log_count % MSHIM_LOG_MAX;
    mshim_log_recs[slot].cause = cause;
    mshim_log_recs[slot].pc = pc;
    mshim_log_recs[slot].addr = addr;
    mshim_log_count++;   /* 中身を書いてから件数を進める */
}

void board_drain_trap_log(void)
{
    static uint32_t drained;
    static uint64_t last_total;
    /* drain が 1 周分より遅れたら、残っている最古に追いつく
     * (上書きされた記録は諦める) */
    if (mshim_log_count - drained > MSHIM_LOG_MAX)
        drained = mshim_log_count - MSHIM_LOG_MAX;
    while (drained < mshim_log_count) {
        uint32_t slot = drained % MSHIM_LOG_MAX;
        uint64_t cause = mshim_log_recs[slot].cause;
        uint64_t pc = mshim_log_recs[slot].pc;
        uint64_t addr = mshim_log_recs[slot].addr;
        drained++;
        if (cause & MSHIM_LOG_FORWARDED) {
            lkputs("trap: forwarded to kernel cause=");
            lkput_dec(cause & ~MSHIM_LOG_FORWARDED);
        } else {
            lkputs("trap: misaligned emulated cause=");
            lkput_dec(cause);
        }
        lkputs(" pc=");
        lkput_hex(pc);
        lkputs(" addr=");
        lkput_hex(addr);
        lkputc('\n');
    }
    if (mshim_emu_total != last_total) {
        last_total = mshim_emu_total;
        lkputs("trap: misaligned emulated total=");
        lkput_dec(last_total);
        lkputc('\n');
    }
}

/* 符号拡張 (bits ビットの値を 64 ビットへ) */
static uint64_t sign_ext(uint64_t v, int bits)
{
    uint64_t sign = 1ULL << (bits - 1);
    return (v ^ sign) - sign;
}

/* cause 4 / 6 の命令を解読して実行する。成功したら 0、
 * 対象外 (浮動小数点、未対応の形、変換できないアドレス)
 * なら -1。整数の load / store と圧縮形が対象 */
static int emulate_misaligned(uint64_t *tf, uint64_t cause)
{
    uint64_t epc = r_mepc();
    uint64_t ipa, addr, value;
    uint32_t ins;
    int ilen, is_load, size, sext, rd, rs1, rs2;
    uint64_t imm;

    if (va_to_pa(epc, &ipa) != 0)
        return -1;
    ins = *(volatile uint16_t *)ipa |
          ((uint32_t)*(volatile uint16_t *)(ipa + 2) << 16);

    is_load = (cause == 4);
    sext = 0;
    rd = rs1 = rs2 = 0;
    imm = 0;

    if ((ins & 3) == 3) {          /* 4 バイト命令 */
        uint32_t op = ins & 0x7F;
        uint32_t f3 = (ins >> 12) & 7;
        ilen = 4;
        if (op == 0x03) {          /* LOAD */
            if (!is_load)
                return -1;
            rd = (int)((ins >> 7) & 0x1F);
            rs1 = (int)((ins >> 15) & 0x1F);
            imm = sign_ext((ins >> 20) & 0xFFF, 12);
            switch (f3) {
            case 0: size = 1; sext = 1; break;  /* LB */
            case 1: size = 2; sext = 1; break;  /* LH */
            case 2: size = 4; sext = 1; break;  /* LW */
            case 3: size = 8; break;            /* LD */
            case 4: size = 1; break;            /* LBU */
            case 5: size = 2; break;            /* LHU */
            case 6: size = 4; break;            /* LWU */
            default: return -1;
            }
        } else if (op == 0x23) {   /* STORE */
            if (is_load)
                return -1;
            rs1 = (int)((ins >> 15) & 0x1F);
            rs2 = (int)((ins >> 20) & 0x1F);
            imm = sign_ext((((ins >> 25) & 0x7F) << 5) |
                           ((ins >> 7) & 0x1F), 12);
            switch (f3) {
            case 0: size = 1; break;            /* SB */
            case 1: size = 2; break;            /* SH */
            case 2: size = 4; break;            /* SW */
            case 3: size = 8; break;            /* SD */
            default: return -1;
            }
        } else {
            return -1;   /* 浮動小数点 (0x07 / 0x27) などは段2へ */
        }
    } else {                       /* 2 バイト圧縮命令 */
        uint32_t q = ins & 3;
        uint32_t f3 = (ins >> 13) & 7;
        ilen = 2;
        if (q == 0) {
            rs1 = 8 + (int)((ins >> 7) & 7);
            if (f3 == 2 || f3 == 3) {           /* C.LW / C.LD */
                if (!is_load)
                    return -1;
                rd = 8 + (int)((ins >> 2) & 7);
                if (f3 == 2) {
                    size = 4; sext = 1;
                    imm = (((ins >> 10) & 7) << 3) |
                          (((ins >> 6) & 1) << 2) |
                          (((ins >> 5) & 1) << 6);
                } else {
                    size = 8;
                    imm = (((ins >> 10) & 7) << 3) |
                          (((ins >> 5) & 3) << 6);
                }
            } else if (f3 == 6 || f3 == 7) {    /* C.SW / C.SD */
                if (is_load)
                    return -1;
                rs2 = 8 + (int)((ins >> 2) & 7);
                if (f3 == 6) {
                    size = 4;
                    imm = (((ins >> 10) & 7) << 3) |
                          (((ins >> 6) & 1) << 2) |
                          (((ins >> 5) & 1) << 6);
                } else {
                    size = 8;
                    imm = (((ins >> 10) & 7) << 3) |
                          (((ins >> 5) & 3) << 6);
                }
            } else {
                return -1;   /* C.FLD / C.FSD などは段2へ */
            }
        } else if (q == 2) {
            rs1 = 2;                            /* sp */
            if (f3 == 2 || f3 == 3) {           /* C.LWSP / C.LDSP */
                if (!is_load)
                    return -1;
                rd = (int)((ins >> 7) & 0x1F);
                if (rd == 0)
                    return -1;
                if (f3 == 2) {
                    size = 4; sext = 1;
                    imm = (((ins >> 12) & 1) << 5) |
                          (((ins >> 4) & 7) << 2) |
                          (((ins >> 2) & 3) << 6);
                } else {
                    size = 8;
                    imm = (((ins >> 12) & 1) << 5) |
                          (((ins >> 5) & 3) << 3) |
                          (((ins >> 2) & 7) << 6);
                }
            } else if (f3 == 6 || f3 == 7) {    /* C.SWSP / C.SDSP */
                if (is_load)
                    return -1;
                rs2 = (int)((ins >> 2) & 0x1F);
                if (f3 == 6) {
                    size = 4;
                    imm = (((ins >> 9) & 0xF) << 2) |
                          (((ins >> 7) & 3) << 6);
                } else {
                    size = 8;
                    imm = (((ins >> 10) & 7) << 3) |
                          (((ins >> 7) & 7) << 6);
                }
            } else {
                return -1;
            }
        } else {
            return -1;
        }
    }

    addr = get_frame_reg(tf, rs1) + imm;
    if (is_load) {
        uint64_t v = 0;
        int i;
        for (i = 0; i < size; i++) {
            uint8_t b;
            if (read_byte_va(addr + (uint64_t)i, &b) != 0)
                return -1;
            v |= (uint64_t)b << (8 * i);
        }
        if (sext && size < 8)
            v = sign_ext(v, size * 8);
        set_frame_reg(tf, rd, v);
    } else {
        uint64_t v = get_frame_reg(tf, rs2);
        int i;
        for (i = 0; i < size; i++) {
            if (write_byte_va(addr + (uint64_t)i,
                              (uint8_t)(v >> (8 * i))) != 0)
                return -1;
        }
    }
    mshim_emu_total++;
    mshim_log_push(cause, epc, addr);
    w_mepc(epc + (uint64_t)ilen);
    return 0;
}

/* ---- 段2: エミュレーションできないトラップの S への転送 -------------
 * U モード由来のトラップは、S 側の CSRs (sepc / scause /
 * stval / sstatus) を本物のトラップと同じ形に仕立て、
 * mret の行き先を trap.S のユーザー用入口 (trap_entry_user)
 * へ差し替える。レジスタと sp は mshim_entry の通常の復帰が
 * ユーザーのものを戻すので、入口から見ると本物のユーザー
 * トラップと区別がつかない。カーネル側は既存の
 * 「ユーザープログラムの例外」経路がそのプログラムだけを
 * 終わらせる (trap.c)。S 由来のトラップは転送しない
 * (カーネル自身のバグを握りつぶさない) */

extern void trap_entry_user(void);  /* trap.S のユーザー用入口 */

static uint64_t r_mstatus(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static void w_mstatus(uint64_t v)
{
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
}

static uint64_t r_mtval(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, mtval" : "=r"(v));
    return v;
}

static uint64_t r_sstatus(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    return v;
}

static int forward_to_kernel(uint64_t cause)
{
    uint64_t ms = r_mstatus();
    uint64_t ss;
    if (((ms >> 11) & 3) != 0)
        return -1;                   /* U 由来でない */
    mshim_log_push(cause | MSHIM_LOG_FORWARDED, r_mepc(), r_mtval());
    __asm__ volatile("csrw scause, %0" :: "r"(cause));
    __asm__ volatile("csrw sepc, %0" :: "r"(r_mepc()));
    __asm__ volatile("csrw stval, %0" :: "r"(r_mtval()));
    ss = r_sstatus();
    ss &= ~(1ULL << 8);              /* SPP = U から */
    ss |= (1ULL << 5);               /* SPIE = 1 (元の許可状態) */
    ss &= ~(1ULL << 1);              /* SIE = 0 (入口の作法) */
    __asm__ volatile("csrw sstatus, %0" :: "r"(ss));
    ms &= ~(3ULL << 11);
    ms |= (1ULL << 11);              /* MPP = S */
    w_mstatus(ms);
    w_mepc((uint64_t)trap_entry_user);
    return 0;
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
         * K210 では S へ委譲できず M に来る。整数の load/store は
         * エミュレーションして続行させる。対象外 (浮動小数点、
         * 変換できないアドレス) は U 由来なら S へ転送して
         * プログラムだけ落とす (段2) */
        if (emulate_misaligned(tf, cause) == 0)
            return;
        if (forward_to_kernel(cause) == 0)
            return;
        fatal("misaligned load/store (not emulated)");
    }

    if (cause == 2) {  /* 違法命令: カウンタ読みをエミュレーション */
        uint64_t mp = r_mepc();
        uint64_t ipa;
        uint32_t ins;
        /* 命令を読む。U 由来では mepc はユーザー仮想アドレス
         * なので、物理を直読みせずページ表で変換してから読む
         * (S 由来の恒等マップも同じ経路で辿れる) */
        if (va_to_pa(mp, &ipa) != 0) {
            if (forward_to_kernel(cause) == 0)
                return;
            fatal("illegal instruction (unmapped pc)");
        }
        ins = *(volatile uint16_t *)ipa |
              ((uint32_t)*(volatile uint16_t *)(ipa + 2) << 16);
        if ((ins & 0xFFFFF07Fu) == 0xC0102073u) {  /* csrr rd, time */
            int rd = (int)((ins >> 7) & 0x1F);
            set_frame_rd(tf, rd, CLINT_MTIME);
            w_mepc(mp + 4);
            return;
        }
        if ((ins & 0xFFFFF07Fu) == 0xC0002073u) {  /* csrr rd, cycle */
            int rd = (int)((ins >> 7) & 0x1F);
            uint64_t v;
            __asm__ volatile("csrr %0, mcycle" : "=r"(v));
            set_frame_rd(tf, rd, v);
            w_mepc(mp + 4);
            return;
        }
        if ((ins & 0xFFFFF07Fu) == 0xC0202073u) {  /* csrr rd, instret */
            int rd = (int)((ins >> 7) & 0x1F);
            uint64_t v;
            __asm__ volatile("csrr %0, minstret" : "=r"(v));
            set_frame_rd(tf, rd, v);
            w_mepc(mp + 4);
            return;
        }
        /* カウンタ読み以外の違法命令: U 由来なら S へ転送 */
        if (forward_to_kernel(cause) == 0)
            return;
        fatal("illegal instruction");
    }

    /* 想定外の例外も、U 由来なら S へ転送してプログラムだけ
     * 落とす。それ以外 (S 由来) は今までどおり止める */
    if (forward_to_kernel(cause) == 0)
        return;
    fatal("unexpected trap");
}
