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
#include "page.h"
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
 * エントリ 0/1/2 はコード / データ / スタックの下段テーブルへの
 * ポインタ (中身は 4KB ページの葉)。大きなエントリ番号側には
 * ボード固有のデバイス割り当ても入る (vm_map_device)。 */
static uint64_t user_l1_table[512] __attribute__((aligned(4096)));

/* 下段 (4KB ページ用) テーブルの置き場所。ユーザー領域は
 * VA の並びが固定なので、必要になるのはコード / データ /
 * スタックの下段テーブルだけ (virt で 4 枚、K210 で 3 枚)。
 * exec のたびに中身を書き換えて使い回す */
#define PT_POOL_TABLES 8
static uint64_t pt_pool[PT_POOL_TABLES][512] __attribute__((aligned(4096)));
static int pt_pool_used = 0;

static uint64_t *pt_new_table(void)
{
    uint64_t *t;
    int i;
    if (pt_pool_used >= PT_POOL_TABLES) {
        return 0;
    }
    t = pt_pool[pt_pool_used++];
    for (i = 0; i < 512; i++) {
        t[i] = 0;
    }
    return t;
}

static uint64_t make_leaf_pte(uint64_t pa, uint64_t flags);

/* --- 段ごとのページ台帳 (exec 多段化) --------------------------------
 * 物理メモリは「段ごとに固定の領域セット」ではなく、プール
 * (page.c) からページ単位で借りて台帳に記録する形になった。
 * ページ表への反映は vm_apply_pages が台帳どおりに下段テーブルを
 * 書き換えることで行う (段の切り替え = 書き換え + TLB flush)。
 * ページ表自体は全段で共有の 1 組で、同時に表に出ているのは
 * 現在の段だけ (走るプログラムは常に最上段の 1 つ)。 */

#define USER_CODE_FLAGS  (PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D)
#define USER_DATA_FLAGS  (PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D)
/* スタックの旗はデータと同じ (読み書き可・実行不可) */

static struct user_pages levels[EXEC_MAX_DEPTH + 1];
static int cur_level = 0;  /* ページ表に今出ている段 */

/* ユーザー領域の 4KB ページ 1 枚分の PTE を書く (pa = 0 なら
 * 無効化)。下段テーブルが無ければプールから作る。ボード固有の
 * デバイス割り当て (中段の葉) の場所は触らない */
static void set_user_pte(uint64_t va, uint64_t pa, uint64_t flags)
{
    uint64_t vpn1 = (va >> 21) & 0x1FF;
    uint64_t vpn0 = (va >> 12) & 0x1FF;
    uint64_t l1_pte = user_l1_table[vpn1];
    uint64_t *l0;

    if (l1_pte & (PTE_R | PTE_W | PTE_X)) {
        return;  /* メガページの葉 = デバイス割り当ての場所 */
    }
    if (l1_pte & PTE_V) {
        l0 = (uint64_t *)((l1_pte >> 10) << 12);
    } else {
        l0 = pt_new_table();
        if (l0 == 0) {
            return;
        }
        user_l1_table[vpn1] = ((uint64_t)l0 >> 12 << 10) | PTE_V;
    }
    l0[vpn0] = pa != 0 ? make_leaf_pte(pa, flags) : 0;
}

struct user_pages *vm_pages(int level)
{
    return &levels[level];
}

/* 台帳のページを全部プールへ返して台帳を空にする。
 * ページ表の方は次の vm_apply_pages で書き換わるので、
 * ここでは触らない (返した直後にユーザーモードへ降りない
 * 前提で、呼び出し側が exec / 終了の区切りで使う) */
void vm_pages_release(int level)
{
    struct user_pages *up = &levels[level];
    uint64_t i;

    for (i = 0; i < USER_CODE_PAGES; i++) {
        if (up->code[i] != 0) {
            page_free(up->code[i]);
            up->code[i] = 0;
        }
    }
    for (i = 0; i < USER_DATA_PAGES; i++) {
        if (up->data[i] != 0) {
            page_free(up->data[i]);
            up->data[i] = 0;
        }
    }
    for (i = 0; i < USER_STACK_PAGES; i++) {
        if (up->stack[i] != 0) {
            page_free(up->stack[i]);
            up->stack[i] = 0;
        }
    }
}

/* 台帳をページ表へ反映して、その段を「現在」にする */
void vm_apply_pages(int level)
{
    struct user_pages *up = &levels[level];
    uint64_t i;

    cur_level = level;
    for (i = 0; i < USER_CODE_PAGES; i++) {
        set_user_pte(USER_CODE_VA + i * 4096, up->code[i],
                     USER_CODE_FLAGS);
    }
    for (i = 0; i < USER_DATA_PAGES; i++) {
        set_user_pte(USER_DATA_VA + i * 4096, up->data[i],
                     USER_DATA_FLAGS);
    }
    for (i = 0; i < USER_STACK_PAGES; i++) {
        set_user_pte(USER_STACK_VA + i * 4096, up->stack[i],
                     USER_DATA_FLAGS);
    }
    BOARD_SFENCE_VMA();
}

uint64_t vm_pages_va_to_pa(const struct user_pages *up, uint64_t va)
{
    uint64_t pa;

    if (va >= USER_CODE_VA && va < USER_CODE_VA + BOARD_CODE_SIZE) {
        pa = up->code[(va - USER_CODE_VA) / 4096];
        return pa != 0 ? pa + (va & 4095) : 0;
    }
    if (va >= USER_DATA_VA && va < USER_DATA_VA + BOARD_DATA_SIZE) {
        pa = up->data[(va - USER_DATA_VA) / 4096];
        return pa != 0 ? pa + (va & 4095) : 0;
    }
    if (va >= USER_STACK_VA && va < USER_STACK_TOP_VA) {
        pa = up->stack[(va - USER_STACK_VA) / 4096];
        return pa != 0 ? pa + (va & 4095) : 0;
    }
    return 0;
}

/* スタックの末尾側 nbytes 分のページを用意する。
 * 既に割り当ててあるページはそのまま使う */
int vm_stack_prepare(struct user_pages *up, uint64_t nbytes)
{
    uint64_t need = (nbytes + 4095) / 4096;
    uint64_t i;

    if (need > USER_STACK_PAGES) {
        need = USER_STACK_PAGES;
    }
    for (i = 0; i < need; i++) {
        uint64_t idx = USER_STACK_PAGES - 1 - i;
        if (up->stack[idx] == 0) {
            uint64_t pa = page_alloc();
            if (pa == 0) {
                return -1;
            }
            up->stack[idx] = pa;
        }
    }
    return 0;
}

/* fault をきっかけにスタックを 1 ページ伸ばす (現在の段)。
 * スタック領域の中の未割り当てページだけが対象で、それ以外は
 * 「伸ばせない = 本物の違反」として 0 を返す */
int vm_stack_grow(uint64_t va)
{
    struct user_pages *up = &levels[cur_level];
    uint64_t idx, pa;

    if (va < USER_STACK_VA || va >= USER_STACK_TOP_VA) {
        return 0;
    }
    idx = (va - USER_STACK_VA) / 4096;
    if (up->stack[idx] != 0) {
        return 0;  /* 既に写してあるのに fault した = 違反 */
    }
    pa = page_alloc();
    if (pa == 0) {
        return 0;  /* プール枯渇: 伸ばせない */
    }
    up->stack[idx] = pa;
    set_user_pte(USER_STACK_VA + idx * 4096, pa, USER_DATA_FLAGS);
    BOARD_SFENCE_VMA();  /* 無効な PTE が TLB に残っている場合に備える */
    return 1;
}

/* ボード固有のデバイス割り当て: pa を含む 2MB ブロックを
 * VA = PA で中段の葉にする (カーネル専用 RW)。
 * root のエントリがテーブルを指している住所でのみ使う */
void vm_map_device(uint64_t pa)
{
    uint64_t vpn2 = (pa >> 30) & 0x1FF;
    uint64_t vpn1 = (pa >> 21) & 0x1FF;
    uint64_t *l1 = (uint64_t *)((root_table[vpn2] >> 10) << 12);

    l1[vpn1] = make_leaf_pte(pa & ~0x1FFFFFULL,
                             PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
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
    vm_apply_pages(0);  /* 最初は段 0 の空の台帳 = ユーザー領域は空 */
#if BOARD_FEATURE_VM_HOOK
    board_vm_hook();  /* ボード固有の追加割り当て (k210: LCD 用 MMIO) */
#endif

    /* satp に登録して変換を有効化する:
     *   上位 4 ビット = モード (8 = Sv39)
     *   下位 44 ビット = ページテーブルの物理ページ番号
     * sfence.vma は「変換のキャッシュ (TLB) を捨てて、新しい
     * 対応表で読み直して」という CPU への指示。 */
    uint64_t satp = (8ULL << 60) | ((uint64_t)root_table >> 12);
    w_satp(satp);
    BOARD_SFENCE_VMA();

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

/* --- exec のためのアドレス変換と引数の配置 ------------------------- */

/* スタック末尾への引数の配置 (vm.h の説明参照)。置き先は台帳の
 * ページで、書き込みは台帳経由の物理アドレスで行う */
#define ARGV_MAX_TOKENS 16

uint64_t user_stack_setup_argv(struct user_pages *up, int argc,
                               const char *const *tokens,
                               uint64_t *argv_va)
{
    uint64_t str_va[ARGV_MAX_TOKENS];
    uint64_t cursor = USER_STACK_TOP_VA;  /* 末尾 (排他的) */
    int i;

    if (argc > ARGV_MAX_TOKENS) {
        argc = ARGV_MAX_TOKENS;
    }
    /* 文字列と argv 配列と整列の余りが収まる分だけ、末尾側の
     * スタックページを用意しておく */
    {
        uint64_t need = (uint64_t)(argc + 1) * 8 + 16;
        int j;
        for (j = 0; j < argc; j++) {
            uint64_t len = 0;
            while (tokens[j][len] != '\0') {
                len++;
            }
            need += len + 1;
        }
        if (vm_stack_prepare(up, need) != 0) {
            return 0;
        }
    }
    /* 文字列を末尾から積む (最後の引数から置く) */
    for (i = argc - 1; i >= 0; i--) {
        uint64_t len = 0, j;
        while (tokens[i][len] != '\0') {
            len++;
        }
        len++;  /* NUL の分 */
        cursor -= len;
        for (j = 0; j < len; j++) {
            uint64_t pa = vm_pages_va_to_pa(up, cursor + j);
            if (pa == 0) {
                return 0;
            }
            *(char *)pa = tokens[i][j];
        }
        str_va[i] = cursor;
    }
    /* その下に argv 配列。16 バイト境界に揃え、そこが新しい sp */
    cursor -= (uint64_t)(argc + 1) * 8;
    cursor &= ~15ULL;
    for (i = 0; i <= argc; i++) {
        uint64_t pa = vm_pages_va_to_pa(up, cursor + (uint64_t)i * 8);
        if (pa == 0) {
            return 0;
        }
        *(uint64_t *)pa = i < argc ? str_va[i] : 0;
    }
    *argv_va = cursor;
    return cursor;
}

/* 現在の段の台帳で訳す (trap.c のユーザーメモリコピー用) */
uint64_t user_va_to_pa(uint64_t va)
{
    return vm_pages_va_to_pa(&levels[cur_level], va);
}
