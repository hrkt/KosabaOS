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

volatile uint64_t k210_hart1_release;
volatile uint64_t k210_hart1_satp;

extern unsigned char hart1_entry[];
extern unsigned char hart1_m_trap[];

int board_start_hart1(void)
{
    k210_hart1_satp = vm_satp_value();
    __asm__ volatile("fence" ::: "memory");
    k210_hart1_release = 1;
    return 0;
}

/* hart1 の M モード入口 (解放ワードが立った後に start.S
 * から呼ばれる)。mshim_boot の hart0 用の段取りのうち、
 * hart1 に要る分だけを踏む。戻らない (mret で S へ) */
void k210_hart1_m_entry(void)
{
    uint64_t ms;

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
