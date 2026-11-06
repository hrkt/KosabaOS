/* sd_hw.c -- SD カードのハードウェア層 (K210)
 *
 * 配線は 2026-11-06 の二分探索 (spike-k210/step5-sd、実機で
 * 読み取り確認済みの最小スパイク sdspiW12 と本物ドライバ) で
 * 確定した値:
 *   コントローラは SPI1、MOSI=IO28 (SPI1_D0=70)、
 *   MISO=IO26 (SPI1_D1=71)、SCLK=IO27 (SPI1_SCLK=83)、
 *   CS=IO29 (GPIOHS29、手動制御)
 * パッドフラグの要点 (長い間 SD が無応答だった真因):
 *   - SCLK と CS は oe_inv (bit13) = 0 でなければならない。
 *     oe_inv=1 だとパッド出力が反転し、クロックと CS が
 *     カードに正しく届かない。D0/D1 は oe_inv=1 が正規。
 *   - MISO にプルアップを足してはいけない (本物ドライバが
 *     死ぬことを単独変異で確認済み)。
 *   - ROM/kflash 残りで IO45 にも GPIOHS29 が割り当たって
 *     いるので、競合を消すために IO45 を中立化する。
 *   - spi_ctrlr0 (0xF4) は ROM 残りで 0x200 になっている。
 *     本物 spi_init と同じく 0 を書く。
 * クロック: BAUDR と SPI1 の threshold (0 に固定) で決める。
 * 初期化中は 400kHz 以下に収まるよう BAUDR を大きく取り、
 * 初期化後は実用十分な数 MHz に上げる (MP3 の所要は
 * 最大 40KB/s)
 */

#include <stdint.h>
#include "sd_hw.h"

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* ---- FPIOA / GPIOHS ---- */
#define FPIOA(n) REG32(0x502B0000UL + (n) * 4)
/* SDK 正規テーブル準拠のパッドフラグ (sdspiW12 で実証)。
 * DATA 系 (D0/D1) は oe_inv=1 を含む 0xB03F00、SCLK と CS は
 * oe_inv=0 の 0xB01F00。どちらも DS 最大・IE_EN・ST 付き */
#define FPIOA_SD_DATA 0xB03F00
#define FPIOA_SD_NOINV 0xB01F00
#define FUNC_SPI1_D0 70
#define FUNC_SPI1_D1 71
#define FUNC_SPI1_SCLK 83
#define FUNC_GPIOHS29 (24 + 29)

#define GPIOHS_OUTPUT_EN REG32(0x38001008)
#define GPIOHS_OUTPUT_VAL REG32(0x3800100C)
#define CS_BIT (1u << 29)

/* ---- SPI1 (DesignWare SSI、標準モード) ---- */
#define SPI1 0x53000000
#define SPI_CTRLR0 REG32(SPI1 + 0x00)
#define SPI_SSIENR REG32(SPI1 + 0x08)
#define SPI_SER    REG32(SPI1 + 0x10)
#define SPI_BAUDR  REG32(SPI1 + 0x14)
#define SPI_TXFLR  REG32(SPI1 + 0x20)
#define SPI_RXFLR  REG32(SPI1 + 0x24)
#define SPI_IMR    REG32(SPI1 + 0x2C)
#define SPI_DMACR  REG32(SPI1 + 0x4C)
#define SPI_DR     REG32(SPI1 + 0x60)
#define SPI_SPI_CTRLR0 REG32(SPI1 + 0xF4)

/* 初期化中: 入力 390MHz としても約 95kHz、/2 が入っても
 * 約 48kHz。どちらでも SD の上限 400kHz 以下 */
#define BAUD_INIT 4096
/* 初期化後: 約 3〜6MHz。MP3 再生には余りある */
#define BAUD_FAST 64

static void spi_config(uint32_t baud)
{
    SPI_SSIENR = 0;
    SPI_IMR = 0;
    SPI_DMACR = 0;
    /* 標準 (Motorola) 形式、8 ビット、TMOD=0 (送受信)、
     * モード 0 (CPOL=0, CPHA=0)。DFS の位置は lcd.c (SPI0)
     * で実績のある並びと同じ */
    SPI_CTRLR0 = (7u << 16);
    SPI_SPI_CTRLR0 = 0;   /* ROM 残りの 0x200 を消す */
    SPI_BAUDR = baud;
    SPI_SSIENR = 1;
    /* 実物の CS は GPIOHS で握る。SER は転送を始めるための
     * 内部の選択印 (このピンは FPIOA で外に出していない)。
     * 本物ドライバの SD_SS (CHIP_SELECT_1) に合わせて 2 */
    SPI_SER = 2;
}

void sd_hw_init(void)
{
    /* SPI0/SPI1 は APB2。lcd.c と同じ流儀でクロックを開く
     * (SPI1 の peri ビットはスパイクで使った bit7) */
    REG32(0x50440028) |= (1u << 5);              /* clk_en_cent.apb2 */
    REG32(0x5044002C) |= (1u << 6) | (1u << 7);  /* clk_en_peri spi0/1 */
    /* SPI1 の threshold (clk_th1 の第2バイト) を 0 に固定
     * (本物 spi_clk_init と同じ) */
    REG32(0x5044003C) &= ~(0xFFu << 8);

    /* 競合ピンの中立化: IO45 に残っている GPIOHS29 の割当を
     * 消す (sdspiW12 と同じ値) */
    FPIOA(45) = 0x00000078;
    FPIOA(26) = FUNC_SPI1_D1 | FPIOA_SD_DATA;    /* MISO */
    FPIOA(27) = FUNC_SPI1_SCLK | FPIOA_SD_NOINV; /* SCLK */
    FPIOA(28) = FUNC_SPI1_D0 | FPIOA_SD_DATA;    /* MOSI */
    FPIOA(29) = FUNC_GPIOHS29 | FPIOA_SD_NOINV;  /* CS */
    GPIOHS_OUTPUT_EN |= CS_BIT;
    sd_cs(1);

    spi_config(BAUD_INIT);
}

void sd_hw_set_fast(void)
{
    spi_config(BAUD_FAST);
}

uint8_t sd_xfer(uint8_t out)
{
    /* どちらの待ちも有限回で諦める (カード不在や配線不良で
     * カーネルが止まらないため。失敗時は 0xFF を返す) */
    uint32_t t = 1000000;
    while (SPI_TXFLR >= 32 && t--) {
    }
    if (t == 0) {
        return 0xFF;
    }
    SPI_DR = out;
    t = 1000000;
    while (SPI_RXFLR == 0 && t--) {
    }
    if (t == 0) {
        return 0xFF;
    }
    return (uint8_t)(SPI_DR & 0xFF);
}

void sd_cs(int level)
{
    if (level) {
        GPIOHS_OUTPUT_VAL |= CS_BIT;
    } else {
        GPIOHS_OUTPUT_VAL &= ~CS_BIT;
    }
}

void sd_delay_ms(uint32_t ms)
{
    /* 390MHz でおよそ 1ms (lcd.c と同じ雑な待ち) */
    for (volatile uint32_t i = 0; i < ms * 130000u; i++) {
    }
}
