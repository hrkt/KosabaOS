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
 * wdt0 (bit24) を有効にする。WDT のクロック源は IN0
 * (26MHz) で、pclk = 26MHz / ((threshold + 1) * 2)。
 * threshold は clk_th6 (0x50440050) の bits[7:0] で、
 * SDK と同じく 0 にする (pclk = 13MHz)。開始前に
 * peri_reset (0x50440034) の bit24 でモジュールを
 * リセットするのも SDK の開始手順どおり。
 * pclk の実値は起動時に ccvr の減り方を時刻 CSR と
 * 比べて測り、klog に残す (CLINT の MMIO 直読みは
 * S モードから使えなかったので、計測は csrr time で
 * 行う。2026-11-19 の実機で確認)。餌やりは時計
 * タスクの起床 (proc.c) から約 1 秒周期。
 *
 * WDT_TEST ビルドでは 15 回で餌やりを止め、自動リセットが
 * 起きることを確認できるようにする (build.sh の WDT_TEST)。
 */

#include <stdint.h>
#include "wdt.h"
#include "klog.h"
#include "board.h"  /* BOARD_TIMER_HZ (待ち時間の換算) */

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define WDT_CR   REG32(0x50400000)
#define WDT_TORR REG32(0x50400004)
#define WDT_CCVR REG32(0x50400008)
#define WDT_CRR  REG32(0x5040000C)

#define CLK_EN_CENT REG32(0x50440028)   /* apb1 = bit4 */
#define CLK_EN_PERI REG32(0x5044002C)   /* wdt0 = bit24 */
#define PERI_RESET  REG32(0x50440034)   /* wdt0 = bit24 */
#define CLK_TH6     REG32(0x50440050)   /* wdt0 閾値 = bits[7:0] */

#define WDT_TOP 10                      /* 2^26 / 13MHz ≒ 5.2 秒 */

/* 時刻 CSR (M シムが CLINT からエミュレーションする、
 * OS 全体の時刻の読み方と同じ)。CLINT の MMIO 直読みは
 * S モードから使えなかったのでこちらを使う */
static uint64_t r_time(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, time" : "=r"(v));
    return v;
}

/* BOARD_TIMER_HZ 基準で ms ミリ秒だけ待つ (概算でよい) */
static void wait_ms(uint64_t ms)
{
    uint64_t until = r_time() + ms * (BOARD_TIMER_HZ / 1000);
    while (r_time() < until) {
    }
}

void board_wdt_start(void)
{
    uint32_t c0, c1;
    uint64_t pclk, timeout_ms;

    CLK_EN_CENT |= (1u << 4);
    CLK_EN_PERI |= (1u << 24);

    /* モジュールのリセット (SDK の開始手順)。パルスを打つ */
    PERI_RESET |= (1u << 24);
    PERI_RESET &= ~(1u << 24);
    /* クロック閾値を 0 に (pclk = 26MHz / 2 = 13MHz) */
    CLK_TH6 &= ~0xFFu;

    WDT_CRR = 0x76;
    WDT_CR = 0;                          /* いったん止める */
    WDT_TORR = (uint32_t)(WDT_TOP | (WDT_TOP << 4));

    /* pclk の実測: ccvr の減りから求める。測るために先に
     * 動かす必要があるので、有効化してから測る。餌やりは
     * SDK の wdt_enable と同じく有効化の前に書く。有効化
     * 直後の ccvr 読みは再開処理と競合して 0 が返ることが
     * ある (実機で確認) ので、少し待ってから読み始める */
    WDT_CRR = 0x76;
    WDT_CR = 1;                          /* enable、RMOD = リセット */
    wait_ms(5);
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
