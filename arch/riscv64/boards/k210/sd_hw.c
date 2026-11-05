/* sd_hw.c -- SD カードのハードウェア層 (K210)
 *
 * 配線は段1 スパイク (spike-k210/step5-sd) で確定した値:
 *   コントローラは SPI1 (回路図のネット名は "SPI0_" だが
 *   MaixPy の実績はこちら)、MOSI=IO26 (SPI1_D0=70)、
 *   MISO=IO27 (SPI1_D1=71)、SCLK=IO28 (SPI1_SCLK=83)、
 *   CS=IO29 (GPIOHS29、手動制御)
 * クロック: BAUDR だけで決める (sysctl の threshold は
 * 触らない)。SSI 入力の見積もり幅が広くても初期化中は
 * 400kHz 以下に収まるよう BAUDR を大きく取り、初期化後は
 * 実用十分な数 MHz に上げる (MP3 の所要は最大 40KB/s)
 */

#include <stdint.h>
#include "sd_hw.h"

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* ---- FPIOA / GPIOHS ---- */
#define FPIOA(n) REG32(0x502B0000UL + (n) * 4)
#define FPIOA_OUT 0x1F00      /* 出力用 (OE_EN + DS 最大) */
#define FPIOA_IN 0x00900000   /* 入力用 (IE_EN + ST) */
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
    SPI_BAUDR = baud;
    SPI_SSIENR = 1;
    /* 実物の CS は GPIOHS で握る。SER は転送を始めるための
     * 内部の選択印 (このピンは FPIOA で外に出していない)。
     * lcd.c と同じく有効化の後に書く */
    SPI_SER = 1;
}

void sd_hw_init(void)
{
    /* SPI0/SPI1 は APB2。lcd.c と同じ流儀でクロックを開く
     * (SPI1 の peri ビットはスパイクで使った bit7) */
    REG32(0x50440028) |= (1u << 5);              /* clk_en_cent.apb2 */
    REG32(0x5044002C) |= (1u << 6) | (1u << 7);  /* clk_en_peri spi0/1 */

    FPIOA(26) = FUNC_SPI1_D0 | FPIOA_OUT;
    FPIOA(27) = FUNC_SPI1_D1 | FPIOA_IN;
    FPIOA(28) = FUNC_SPI1_SCLK | FPIOA_OUT;
    FPIOA(29) = FUNC_GPIOHS29 | FPIOA_OUT;
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
