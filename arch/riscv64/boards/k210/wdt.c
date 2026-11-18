/* wdt.c (k210) -- ウォッチドッグ WDT0 (ボード層)
 *
 * カーネルが固まったら自動でリセットして、フラッシュ済み
 * OS で再起動するための最終防衛線 (doc/trap-recovery-design.md
 * の段3)。DesignWare WDT (Kendryte SDK でレジスタ形状を確認):
 *
 *   WDT0 = 0x50400000
 *   cr   (0x00): bit0 = enable、bit1 = RMOD (0 = リセット)、
 *                bits[4:2] = リセットパルス長
 *   torr (0x04): タイムアウト = 2^(16 + TOP) サイクル。
 *                TOP は下位ニブルと上位ニブル (<<4) の両方へ
 *   ccvr (0x08): 現在のカウンタ値
 *   crr  (0x0C): 0x76 を書くと餌やり (カウンタ再開)
 *
 * クロック: clk_en_cent の apb1 (bit4) と clk_en_peri の
 * wdt0 (bit24) を有効にする。pclk の実値は起動時に ccvr の
 * 減り方を CLINT mtime と比べて測り、klog に残す。
 * 餌やりは時計タスクの起床 (proc.c) から約 1 秒周期。
 *
 * WDT_TEST ビルドでは 15 回で餌やりを止め、自動リセットが
 * 起きることを確認できるようにする (build.sh の WDT_TEST)。
 */

#include <stdint.h>
#include "wdt.h"
#include "klog.h"

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define REG64(addr) (*(volatile uint64_t *)(addr))

#define WDT_CR   REG32(0x50400000)
#define WDT_TORR REG32(0x50400004)
#define WDT_CCVR REG32(0x50400008)
#define WDT_CRR  REG32(0x5040000C)

#define CLK_EN_CENT REG32(0x50440028)   /* apb1 = bit4 */
#define CLK_EN_PERI REG32(0x5044002C)   /* wdt0 = bit24 */
#define CLINT_MTIME REG64(0x0200BFF8)   /* 10MHz (ボード共通) */

#define WDT_TOP 15                      /* 2^31 サイクル */

/* mtime (10MHz) 基準で ms ミリ秒だけ待つ (概算でよい) */
static void wait_ms(uint64_t ms)
{
    uint64_t until = CLINT_MTIME + ms * 10000;
    while (CLINT_MTIME < until) {
    }
}

void board_wdt_start(void)
{
    uint32_t c0, c1;
    uint64_t pclk, timeout_ms;

    CLK_EN_CENT |= (1u << 4);
    CLK_EN_PERI |= (1u << 24);

    WDT_CRR = 0x76;
    WDT_CR = 0;                          /* いったん止める */
    WDT_TORR = (uint32_t)(WDT_TOP | (WDT_TOP << 4));

    /* pclk の実測: 100ms の間の ccvr の減りから求める。
     * カウンタは停止中でも…ではなく、測るために先に
     * 動かす必要があるので、有効化してから測る */
    WDT_CR = 1;                          /* enable、RMOD = リセット */
    WDT_CRR = 0x76;
    c0 = WDT_CCVR;
    wait_ms(100);
    c1 = WDT_CCVR;
    if (c0 > c1) {
        pclk = (uint64_t)(c0 - c1) * 10;  /* 100ms 分を 1 秒へ */
        timeout_ms = ((1ULL << (16 + WDT_TOP)) * 1000) / pclk;
        lkputs("wdt: started, pclk approx ");
        lkput_dec(pclk);
        lkputs(" Hz, timeout approx ");
        lkput_dec(timeout_ms);
        lkputs(" ms\n");
    } else {
        lkputs("wdt: started (pclk unmeasured)\n");
    }
#ifdef WDT_TEST
    lkputs("wdt: TEST build, feeding stops after 15 feeds\n");
#endif
}

void board_wdt_feed(void)
{
#ifdef WDT_TEST
    static int feeds;
    if (++feeds > 15) {
        if (feeds == 16)
            lkputs("wdt: TEST stop feeding\n");
        return;
    }
#endif
    WDT_CRR = 0x76;
}
