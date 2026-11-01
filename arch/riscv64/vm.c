/* vm.c -- Sv39 ページング (riscv64)
 *
 * ページングは「プログラムから見える住所 (仮想アドレス)」と
 * 「実際のメモリの住所 (物理アドレス)」の対応表 (ページテーブル)
 * を CPU に渡し、アクセスのたびに変換させる仕組み。対応表に
 * 無い住所や、許可のない操作 (書けない場所への書き込みなど) は
 * ページフォルトになり、トラップとして OS に届く。これが
 * メモリ保護の土台。
 *
 * Sv39 の仮想アドレス (39 ビット) の内訳:
 *   [ VPN2 (9) | VPN1 (9) | VPN0 (9) | offset (12) ]
 * ページテーブルは 3 段で、各段は 512 個のエントリ (PTE) を持つ。
 * 途中の段で「葉 (leaf)」のエントリにすると、その段の大きさの
 * ページになる: VPN0 の段 = 4KB、VPN1 の段 = 2MB (megapage)、
 * VPN2 の段 = 1GB (gigapage)。
 *
 * ここでは最小形として、一番上の段だけで完結させる:
 *   エントリ 0: 仮想 0x00000000 → 物理 0x00000000 (1GB、デバイス領域。
 *               UART 0x10000000 を含む。読み書きのみ、実行は禁止)
 *   エントリ 2: 仮想 0x80000000 → 物理 0x80000000 (1GB、RAM。
 *               カーネル自身が住む。読み書き実行可)
 * 仮想 = 物理なので「恒等マッピング」。他のエントリは無効のまま
 * (値 0) で、そこへのアクセスはフォルトになる。
 * gigapage は Sv39 の仕様の一部で、Linux が動くクラスのボード
 * (安いものを含む) ではすべて使える (doc の計画案を参照)。
 *
 * PTE の中身: 上位に物理ページ番号 (物理アドレス >> 12)、
 * 下位 10 ビットに旗: V(1) 有効、R(2) 読み可、W(4) 書き可、
 * X(8) 実行可、U(16) ユーザー可、G(32)、A(64) アクセス済み、
 * D(128) 書き込み済み。A/D は CPU が自動で立てる機種もあるが、
 * ここでは最初から立てておく (立てないとフォルトする実装がある)。
 */

#include <stdint.h>
#include "vm.h"
#include "uart.h"

#define PTE_V (1ULL << 0)
#define PTE_R (1ULL << 1)
#define PTE_W (1ULL << 2)
#define PTE_X (1ULL << 3)
#define PTE_U (1ULL << 4)
#define PTE_A (1ULL << 6)
#define PTE_D (1ULL << 7)

/* 一番上の段のページテーブル。4KB 整列が必須 (住所の下位 12
 * ビットが 0 でないと satp に登録できない)。 */
static uint64_t root_table[512] __attribute__((aligned(4096)));

/* ユーザー領域用の中段テーブル (root のエントリ 1 の先)。
 * エントリ 0 = コード、1 = データ、2 = スタック (各 2MB) */
static uint64_t user_l1_table[512] __attribute__((aligned(4096)));

/* ユーザー領域が今向いている物理メモリ (exec で差し替わる) */
static uint64_t cur_code_pa = USER_CODE_PA;
static uint64_t cur_data_pa = USER_DATA_PA;
static uint64_t cur_stack_pa = USER_STACK_PA;

static uint64_t make_leaf_pte(uint64_t pa, uint64_t flags);

static void apply_user_region(void)
{
    user_l1_table[0] = make_leaf_pte(cur_code_pa,
                                     PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D);
    user_l1_table[1] = make_leaf_pte(cur_data_pa,
                                     PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
    user_l1_table[2] = make_leaf_pte(cur_stack_pa,
                                     PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
}

/* 物理アドレスと旗から「葉」の PTE を作る
 * (gigapage でも megapage でも符号化は同じ) */
static uint64_t make_leaf_pte(uint64_t pa, uint64_t flags)
{
    return ((pa >> 12) << 10) | flags;
}

static inline void w_satp(uint64_t x)
{
    asm volatile("csrw satp, %0" : : "r"(x));
}

void vm_init(void)
{
    /* エントリ番号は仮想アドレスの VPN2 (>> 30) */
    root_table[0] = make_leaf_pte(0x00000000ULL,
                                  PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    root_table[2] = make_leaf_pte(0x80000000ULL,
                                  PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);

    /* ユーザー領域 (ステップ 4): エントリ 1 は中段テーブルへの
     * ポインタ (V のみ = 葉ではない)。中段の葉には U の旗を付け、
     * 「ユーザーモードでも触ってよい場所」をここだけに限定する。
     * カーネルのページには U が無いので、ユーザーモードからは
     * カーネルのメモリに届かない。 */
    root_table[1] = ((uint64_t)user_l1_table >> 12 << 10) | PTE_V;
    cur_code_pa = USER_CODE_PA;
    cur_data_pa = USER_DATA_PA;
    cur_stack_pa = USER_STACK_PA;
    apply_user_region();

    /* satp に登録して変換を有効化する:
     *   上位 4 ビット = モード (8 = Sv39)
     *   下位 44 ビット = ページテーブルの物理ページ番号
     * sfence.vma は「変換のキャッシュ (TLB) を捨てて、新しい
     * 対応表で読み直して」という CPU への指示。 */
    uint64_t satp = (8ULL << 60) | ((uint64_t)root_table >> 12);
    w_satp(satp);
    asm volatile("sfence.vma zero, zero");

    uart_puts("vm: Sv39 paging on (identity map)\n");
}

/* デモ用: マップされていない住所 (0x60000000: ユーザー領域の
 * 中段テーブルで何も割り当てていない場所) をわざと読む。
 * ページフォルト (load page fault) がトラップハンドラに届き、
 * scause と stval が表示されて止まる。
 * 「保護が働く瞬間」を目で見るためのもの。 */
void vm_fault_demo(void)
{
    volatile unsigned char *p = (volatile unsigned char *)0x60000000ULL;
    unsigned char c;

    uart_puts("\nvm: touching unmapped address 0x60000000 ...\n");
    c = *p;              /* ここでフォルトする */
    uart_putc((char)c);  /* 届くはずがない行 */
}

/* --- exec のための領域差し替えとアドレス変換 (ステップ 6) -------- */

void vm_switch_user_region(uint64_t code_pa, uint64_t data_pa,
                           uint64_t stack_pa)
{
    cur_code_pa = code_pa;
    cur_data_pa = data_pa;
    cur_stack_pa = stack_pa;
    apply_user_region();
    asm volatile("sfence.vma zero, zero");
}

uint64_t vm_current_code_pa(void)
{
    return cur_code_pa;
}

uint64_t vm_current_data_pa(void)
{
    return cur_data_pa;
}

uint64_t vm_current_stack_pa(void)
{
    return cur_stack_pa;
}

/* 仮想アドレスの範囲は固定 (コード / データ / スタック各 2MB)
 * で、中身の物理メモリだけが差し替わる */
uint64_t user_va_to_pa(uint64_t va)
{
    if (va >= USER_CODE_VA && va < USER_CODE_VA + 0x200000ULL) {
        return cur_code_pa + (va - USER_CODE_VA);
    }
    if (va >= USER_DATA_VA && va < USER_DATA_VA + 0x200000ULL) {
        return cur_data_pa + (va - USER_DATA_VA);
    }
    if (va >= USER_STACK_VA && va < USER_STACK_VA + 0x200000ULL) {
        return cur_stack_pa + (va - USER_STACK_VA);
    }
    return 0;
}
