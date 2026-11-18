/* i2c.c (k210) -- I2C0 マスタ送信ドライバ (ボード層)
 *
 * 口は board_i2c_write(addr, buf, len) の 1 つだけ (送信のみ)。
 * 設計は doc/oled-design.md。最初の利用者は SSD1306 OLED
 * (ssd1315.c)。受信が要る実需 (RTC 等) が出たら別途足す。
 *
 * ハードウェアの事実 (Kendryte SDK の i2c.c / sysctl.c /
 * fpioa.c で確認。レジスタ配置は DesignWare I2C):
 *   - I2C0 本体は 0x50280000。FPIOA 機能番号は
 *     I2C0_SCLK = 126 / I2C0_SDA = 127。
 *   - ピンは Maixduino の I2C 専用ピン SCL = IO30 / SDA = IO31
 *     (基板上に 4.7K プルアップあり)。Maix Dock でも IO30/31 は
 *     ヘッダの GPIO で、他に割り当てていないので無害。
 *   - FPIOA のパッド設定は SDK の I2C 既定どおり
 *     (oe_en=1, pu=1, sl=1, ie_en=1 → フラグ 0x191000)。
 *   - クロック: clk_en_peri (0x5044002C) の bit13 が i2c0 の
 *     有効化。入力クロックの分周は clk_th5 (0x5044004C) の
 *     [15:8] が i2c0 の threshold。SDK と同じ threshold = 3
 *     (入力は PLL0 / 8 = 97.5MHz 前提)。
 *   - SCL は標準速度 100kHz: hcnt = lcnt = 入力 / (2 * 100kHz)
 *     = 487。入力周波数の前提が多少ずれても SSD1306 の
 *     許容範囲 (〜400kHz、下はほぼ無制限) に収まる。
 *   - 送信手順も SDK と同じ: tar に宛先を置き、data_cmd へ
 *     FIFO の空き分ずつ書く。STOP は FIFO が空になったときに
 *     コントローラが出す。tx_abrt_source が立ったら ACK が
 *     無かった (デバイス不在) として -1。
 *   - SDK は待ちを無限ループにするが、こちらは上限を付ける
 *     (ハードが応答しないときに OS ごと止まらないため)。
 */

#include <stdint.h>
#include "board.h"
#include "i2c.h"

#define REG32(a) (*(volatile uint32_t *)(a))

/* I2C0 レジスタ (オフセット) */
#define I2C_CON        REG32(0x50280000UL + 0x00)
#define I2C_TAR        REG32(0x50280000UL + 0x04)
#define I2C_DATA_CMD   REG32(0x50280000UL + 0x10)
#define I2C_SS_HCNT    REG32(0x50280000UL + 0x14)
#define I2C_SS_LCNT    REG32(0x50280000UL + 0x18)
#define I2C_INTR_MASK  REG32(0x50280000UL + 0x30)
#define I2C_CLR_TX_ABRT REG32(0x50280000UL + 0x54)
#define I2C_ENABLE     REG32(0x50280000UL + 0x6C)
#define I2C_STATUS     REG32(0x50280000UL + 0x70)
#define I2C_TXFLR      REG32(0x50280000UL + 0x74)
#define I2C_TX_ABRT_SOURCE REG32(0x50280000UL + 0x80)
#define I2C_DMA_CR     REG32(0x50280000UL + 0x88)
#define I2C_DMA_TDLR   REG32(0x50280000UL + 0x8C)
#define I2C_DMA_RDLR   REG32(0x50280000UL + 0x90)

#define I2C_STATUS_ACTIVITY 0x01u
#define I2C_STATUS_TFE      0x04u

#define SYSCTL_CLK_EN_PERI REG32(0x5044002CUL)
#define SYSCTL_CLK_TH5     REG32(0x5044004CUL)
#define FPIOA(n)           REG32(0x502B0000UL + (n) * 4)

/* FPIOA: 機能番号 | (oe_en + pu + sl + ie_en) */
#define FPIOA_I2C_FLAGS 0x191000u
#define FUNC_I2C0_SCLK 126u
#define FUNC_I2C0_SDA  127u

#define PIN_SCL 30
#define PIN_SDA 31

static int i2c_started;
static uint8_t i2c_addr;

static void i2c_hw_init(uint8_t addr)
{
    FPIOA(PIN_SCL) = FUNC_I2C0_SCLK | FPIOA_I2C_FLAGS;
    FPIOA(PIN_SDA) = FUNC_I2C0_SDA | FPIOA_I2C_FLAGS;

    SYSCTL_CLK_EN_PERI |= (1u << 13);            /* i2c0_clk_en */
    SYSCTL_CLK_TH5 = (SYSCTL_CLK_TH5 & ~(0xFFu << 8)) | (3u << 8);

    I2C_ENABLE = 0;
    /* master / 標準速度 / restart 有効 / slave 無効 (SDK と同じ) */
    I2C_CON = 0x01u | 0x02u | 0x20u | 0x40u;
    I2C_SS_HCNT = 487;
    I2C_SS_LCNT = 487;
    I2C_TAR = addr;
    I2C_INTR_MASK = 0;
    I2C_DMA_CR = 0x3;      /* SDK の初期化に合わせる (DMA 自体は不使用) */
    I2C_DMA_RDLR = 0;
    I2C_DMA_TDLR = 4;
    I2C_ENABLE = 1;

    i2c_started = 1;
    i2c_addr = addr;
}

/* 1 トランザクション分の送信 (START 〜 STOP)。
 * 戻り値: 0 = 成功、-1 = ACK が無い / 応答なし */
int board_i2c_write(uint8_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t i = 0;
    uint32_t guard;

    if (!i2c_started || addr != i2c_addr)
        i2c_hw_init(addr);

    (void)I2C_CLR_TX_ABRT;   /* 読みでクリア */

    while (i < len) {
        uint32_t room = 8 - I2C_TXFLR;
        while (room > 0 && i < len) {
            I2C_DATA_CMD = buf[i];
            i++;
            room--;
        }
        if (I2C_TX_ABRT_SOURCE != 0)
            return -1;
    }

    /* 送信が終わるまで待つ (activity が落ちて TX FIFO が空) */
    guard = 10000000u;
    while (guard > 0) {
        uint32_t st = I2C_STATUS;
        if (!(st & I2C_STATUS_ACTIVITY) && (st & I2C_STATUS_TFE))
            break;
        guard--;
    }
    if (guard == 0)
        return -1;
    if (I2C_TX_ABRT_SOURCE != 0)
        return -1;
    return 0;
}
