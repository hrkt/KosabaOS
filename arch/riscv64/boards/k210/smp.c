/* boards/k210/smp.c -- hart1 の起こし (K210)
 * (doc/smp-design.md 段1)
 *
 * K210 には SBI がいないので、起こす合図は共有メモリの
 * 解放ワードにする。hart1 は起動直後から M モードで
 * k210_hart1_release を回して見ている (boards/k210/
 * start.S の park)。hart0 のカーネル (S) が、まず
 * k210_hart1_satp に自分の sptbr を書き、次に解放
 * ワードを立てると、hart1 は k210_hart1_m_entry (M)
 * で hart0 と同じ段取り (sptbr + mstatus.VM = Sv39、
 * MPP = S、FS = Initial) を踏んで S の hart1_entry へ
 * mret で降りる。
 *
 * キャッシュの注意: 解放ワードも satp ワードも、通常の
 * キャッシュされる SRAM に置く。K210 の D キャッシュは
 * ハート間でコヒーレントでないと言われており、hart0 の
 * 書きが hart1 に見えるか、逆にハートビートが hart0 に
 * 見えるかは、段1 の実測そのもの (doc/smp-design.md)。
 */

#include "board.h"
#include "smp.h"
#include "vm.h"
#include "klog.h"

volatile uint64_t k210_hart1_release;
volatile uint64_t k210_hart1_satp;

/* 切り分けの印の置き場: CLINT の hart1 用 mtimecmp
 * (0x02004008)。MMIO なのでキャッシュが効かず、両方の
 * hart から即座に見える。hart1 はタイマ割り込みを
 * 有効化していないので、比較器としての副作用は無い。
 * 印の意味: 0xA1 = 解放待ちに入った、0xB2 = 解放を
 * 見た、0xC3 = hart1_main に着いた */
#define MTIMECMP1 (*(volatile uint64_t *)0x02004008ULL)

void board_hart1_mark(uint64_t mark)
{
    MTIMECMP1 = mark;
}

uint64_t board_hart1_mark_read(void)
{
    return MTIMECMP1;
}

extern unsigned char hart1_entry[];
extern unsigned char hart1_m_trap[];

/* CLINT の hart1 用 MSIP (ソフトウェア割り込み要求)。
 * SRAM ブートでは hart1 が _start に来ていないことが
 * 印の実測で分かったので、ROM の駐車をこれで解く */
#define MSIP1 (*(volatile uint32_t *)0x02000004ULL)

int board_start_hart1(void)
{
    /* 印チャンネルの自己診断 (自分で書いて読み戻せるか) */
    board_hart1_mark(0x5A5A);
    if (board_hart1_mark_read() == 0x5A5A) {
        kputs("smp: mark channel ok\n");
    } else {
        kputs("smp: mark channel NG\n");
    }
    board_hart1_mark(0);

    MSIP1 = 1;  /* hart1 を起こす (ROM の wfi を解く) */
    __asm__ volatile("fence" ::: "memory");
    k210_hart1_satp = vm_satp_value();
    __asm__ volatile("fence" ::: "memory");
    k210_hart1_release = 1;
    /* 解放ワードの行を自分のキャッシュから追い出して
     * メモリへ書き戻す。初回の実測 (2026-11-21) では
     * これが無く、hart1 から解放が見えなかった (alive=0
     * のまま)。書き戻しが要るのはキャッシュがハート間
     * コヒーレントでないため */
    smp_evict_cache();
    return 0;
}

/* hart1 の M モード入口 (start.S の park から呼ばれる)。
 * 解放ワードが立つのを待ち、mshim_boot の hart0 用の
 * 段取りのうち hart1 に要る分だけを踏んで S へ降りる。
 * 戻らない (mret で S へ)。
 * 待ちの輪の中で smp_evict_cache を回すのは、自分の
 * キャッシュに残った古い解放ワード (0) を捨てて、
 * メモリから読み直させるため */
void k210_hart1_m_entry(void)
{
    uint64_t ms;

    MSIP1 = 0;  /* 起こしに使った MSIP を自分で消す */
    board_hart1_mark(0xA1);  /* 解放待ちに入った */
    while (k210_hart1_release == 0) {
        smp_evict_cache();
    }
    board_hart1_mark(0xB2);  /* 解放を見た */

    __asm__ volatile("csrw mtvec, %0" :: "r"((uint64_t)hart1_m_trap));
    /* 例外は hart0 と同じ委譲 (違法命令だけ M に残る。
     * 行き先は hart1_m_trap = 印を残して駐車) */
    __asm__ volatile("csrw medeleg, %0" :: "r"(0xB1FBULL));
    __asm__ volatile("csrw mideleg, %0" :: "r"(0ULL));
    __asm__ volatile("csrw mie, %0" :: "r"(0ULL));

    /* ページングの点火 (mshim_boot と同じ作法) */
    __asm__ volatile("csrw 0x180, %0" :: "r"(k210_hart1_satp));
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
    ms &= ~(0x1FULL << 24);
    ms |= (9ULL << 24);              /* VM = Sv39 */
    ms &= ~(3ULL << 11);
    ms |= (1ULL << 11);              /* MPP = S */
    ms &= ~(1ULL << 7);              /* MPIE = 0 */
    ms &= ~(3ULL << 13);
    ms |= (1ULL << 13);              /* FS = Initial */
    __asm__ volatile("csrw mstatus, %0" :: "r"(ms));
    __asm__ volatile("csrw fcsr, zero");
    __asm__ volatile(".word 0x10400073");  /* sfence.vm x0 */
    __asm__ volatile("csrw mepc, %0" :: "r"((uint64_t)hart1_entry));
    __asm__ volatile("mret");
    for (;;) {
        __asm__ volatile("wfi");
    }
}
