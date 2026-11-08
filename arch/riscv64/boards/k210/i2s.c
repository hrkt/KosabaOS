/* i2s.c -- K210 I2S0 ドライバ (オーディオ syscall の実体)
 *
 * 設計は doc/audio-driver-design.md。Maix Dock の
 * TM8211 DAC へ I2S0 で送る。レジスタ配置と初期化の
 * 手順は Kendryte SDK (reference/kendryte-standalone-sdk)
 * の i2s.c / sysctl に従う。要点:
 *   - ピン: IO33 = WS (FPIOA 89)、IO34 = DATA
 *     (95 = I2S0_OUT_D1)、IO35 = SCLK (88)。
 *     OUT_D1 に対応する送信チャンネル 1 を使う
 *   - 形式: 16bit、SCLK 32 サイクル、RIGHT_JUSTIFYING
 *     (MaixPy の wav 再生作例の実績どおり)
 *   - 源クロック PLL2 は 45.0667MHz に固定する
 *     (45.1584MHz ちょうどは分周フィールドでは
 *     作れない。最良の組み合わせで 0.2% 低い)
 *   - DMA は使わず FIFO へポーリングで書く。満杯の
 *     待ちでは sched_poll() を挟み、カーネルタスク
 *     (時計) を止めない */

#include <stdint.h>
#include "audio.h"
#include "klog.h"
#include "proc.h"

#define REG32(a) (*(volatile uint32_t *)(a))

#define SYSCTL_BASE 0x50440000ULL
#define I2S0_BASE   0x50250000ULL
#define FPIOA_BASE  0x502B0000ULL

/* SYSCTL レジスタ */
#define SYSCTL_PLL2        REG32(SYSCTL_BASE + 0x10)
#define SYSCTL_PLL_LOCK    REG32(SYSCTL_BASE + 0x14)
#define SYSCTL_CLK_EN_CENT REG32(SYSCTL_BASE + 0x28)
#define SYSCTL_CLK_EN_PERI REG32(SYSCTL_BASE + 0x2C)
#define SYSCTL_PERI_RESET  REG32(SYSCTL_BASE + 0x34)
#define SYSCTL_CLK_TH3     REG32(SYSCTL_BASE + 0x44)

/* I2S0 レジスタ (DesignWare I2S) */
#define I2S_IER   REG32(I2S0_BASE + 0x00)
#define I2S_IRER  REG32(I2S0_BASE + 0x04)
#define I2S_ITER  REG32(I2S0_BASE + 0x08)
#define I2S_CER   REG32(I2S0_BASE + 0x0C)
#define I2S_CCR   REG32(I2S0_BASE + 0x10)
#define I2S_TXFFR REG32(I2S0_BASE + 0x18)
/* 送信チャンネル 1 (ベース +0x60) */
#define I2S_CH1_LTHR REG32(I2S0_BASE + 0x60)
#define I2S_CH1_RTHR REG32(I2S0_BASE + 0x64)
#define I2S_CH1_TER  REG32(I2S0_BASE + 0x6C)
#define I2S_CH1_TCR  REG32(I2S0_BASE + 0x74)
#define I2S_CH1_ISR  REG32(I2S0_BASE + 0x78)
#define I2S_CH1_IMR  REG32(I2S0_BASE + 0x7C)
#define I2S_CH1_TOR  REG32(I2S0_BASE + 0x84)
#define I2S_CH1_TFCR REG32(I2S0_BASE + 0x8C)
#define I2S_CH1_TFF  REG32(I2S0_BASE + 0x94)

#define ISR_TXFE (1u << 4)  /* TX FIFO が trigger 以下 */
#define ISR_TXFO (1u << 5)  /* TX FIFO あふれ */

/* CCR: align = RIGHT_JUSTIFYING (2) << 5、
 * word_size = 32 サイクル (2) << 3、gate なし、DMA なし */
#define CCR_VALUE ((2u << 5) | (2u << 3))
/* TCR: 解決度 16bit = 2 */
#define TCR_VALUE 2u
/* TFCR: 残り 12 段以下で txfe。FIFO は 16 段なので
 * txfe のとき 1 フレーム (2 段) の余裕が必ずある */
#define TFCR_VALUE 12u

/* PLL2 = 26MHz / (0+1) × (25+1) / (14+1) = 45.0667MHz */
#define PLL2_HZ 45066666ULL

static int i2s_ready;        /* クロックと PLL2 の設定済み */
static int i2s_open_flag;    /* board_audio_open の状態 */
static uint64_t stat_frames; /* 送ったステレオフレーム数 */
static uint64_t stat_waits;  /* 満杯でポーリングした回数 */
static uint64_t stat_over;   /* あふれ (txfo) の回数 */

static void i2s_spin(void)
{
    volatile int i;
    for (i = 0; i < 2000; i++)
        ;
}

/* PLL2 を PLL2_HZ に設定する (SDK の
 * sysctl_pll_set_freq の手順)。ロックできたら 0 */
static int pll2_setup(void)
{
    uint32_t v;
    int i;

    v = SYSCTL_PLL2;
    v &= ~(1u << 25);            /* out_en を落とす */
    SYSCTL_PLL2 = v;
    v &= ~(1u << 21);            /* pwrd を落とす */
    SYSCTL_PLL2 = v;
    i2s_spin();
    /* 分周フィールドだけ書き換える (ckin_sel は触らない):
     * clkr = 0、clkf = 25、clkod = 14、bwadj = 12 */
    v &= ~((0xFu) | (0x3Fu << 4) | (0xFu << 10) | (0x3Fu << 14));
    v |= (25u << 4) | (14u << 10) | (12u << 14);
    SYSCTL_PLL2 = v;
    v |= (1u << 21);             /* pwrd を上げる */
    SYSCTL_PLL2 = v;
    i2s_spin();
    v &= ~(1u << 20);            /* リセットパルス */
    SYSCTL_PLL2 = v;
    v |= (1u << 20);
    SYSCTL_PLL2 = v;
    i2s_spin();
    v &= ~(1u << 20);
    SYSCTL_PLL2 = v;
    /* ロック待ち (pll_lock2 = bit17:16 が両方 1) */
    for (i = 0; i < 1000000; i++) {
        if (((SYSCTL_PLL_LOCK >> 16) & 0x3u) == 0x3u) {
            break;
        }
    }
    v = SYSCTL_PLL2;
    v |= (1u << 25);             /* out_en を上げる */
    SYSCTL_PLL2 = v;
    return (((SYSCTL_PLL_LOCK >> 16) & 0x3u) == 0x3u) ? 0 : -1;
}

/* 最初の open で 1 回だけ: ピン、クロック、リセット、
 * PLL2。失敗 (ロックしない) なら -1 */
static int i2s_setup(void)
{
    if (i2s_ready) {
        return 0;
    }
    /* FPIOA: 出力 3 本 (lcd.c の流儀で func | 0x1F00) */
    REG32(FPIOA_BASE + 33 * 4) = 89 | 0x1F00;  /* I2S0_WS */
    REG32(FPIOA_BASE + 34 * 4) = 95 | 0x1F00;  /* I2S0_OUT_D1 */
    REG32(FPIOA_BASE + 35 * 4) = 88 | 0x1F00;  /* I2S0_SCLK */
    /* クロック有効化: apb0 と i2s0 */
    SYSCTL_CLK_EN_CENT |= (1u << 3);
    SYSCTL_CLK_EN_PERI |= (1u << 10);
    i2s_spin();
    /* I2S0 のリセットパルス (peri_reset の bit10) */
    SYSCTL_PERI_RESET |= (1u << 10);
    i2s_spin();
    SYSCTL_PERI_RESET &= ~(1u << 10);
    i2s_spin();
    if (pll2_setup() != 0) {
        kputs("audio: i2s pll2 lock FAILED lock=");
        kput_hex(SYSCTL_PLL_LOCK);
        kputc('\n');
        return -1;
    }
    i2s_ready = 1;
    return 0;
}

int board_audio_open(uint64_t rate, uint64_t channels)
{
    uint64_t th, actual, diff;
    (void)channels;  /* 1 でも 2 でもハードは常時ステレオ */

    if (i2s_open_flag || rate == 0) {
        return -1;
    }
    if (i2s_setup() != 0) {
        return -1;
    }
    /* threshold = round(pll2 / (rate × 128)) − 1。
     * 実レートが要求の 1% を超えてずれる組み合わせは
     * 受けない (48000 はここで落ちる) */
    th = (PLL2_HZ + rate * 64) / (rate * 128);
    if (th == 0 || th > 0x10000) {
        return -1;
    }
    th -= 1;
    actual = PLL2_HZ / ((th + 1) * 128);
    diff = (actual > rate) ? (actual - rate) : (rate - actual);
    if (diff * 100 > rate) {
        return -1;
    }
    SYSCTL_CLK_TH3 =
        (SYSCTL_CLK_TH3 & 0xFFFF0000u) | (uint32_t)th;

    /* I2S0 の設定 (SDK の i2s_init + tx_channel_config の順) */
    I2S_CH1_TER = 0;
    I2S_IER = 1;
    I2S_IRER = 0;
    I2S_ITER = 1;
    I2S_TXFFR = 1;               /* 送信ブロックの FIFO を流す */
    I2S_CH1_TFF = 1;             /* チャンネルの FIFO を流す */
    I2S_CH1_TCR = TCR_VALUE;
    I2S_CCR = CCR_VALUE;
    I2S_CH1_TFCR = TFCR_VALUE;
    I2S_CH1_IMR = 0x33;          /* 割り込みは全てマスク (ポーリング) */
    (void)I2S_CH1_TOR;           /* 残っているあふれ印を消す */
    I2S_CER = 1;                 /* クロック生成を開始 */
    I2S_CH1_TER = 1;             /* 送信チャンネルを開く */

    stat_frames = 0;
    stat_waits = 0;
    stat_over = 0;
    i2s_open_flag = 1;
    kputs("audio: i2s open pll2=");
    kput_dec(PLL2_HZ);
    kputs(" lock=");
    kput_hex(SYSCTL_PLL_LOCK);
    kputs(" th=");
    kput_dec(th);
    kputs(" rate=");
    kput_dec(actual);
    kputc('\n');
    return 0;
}

/* 1 フレーム (左右 1 組) を FIFO へ。空きが無ければ
 * ポーリングで待つ。その間もカーネルタスクが走れる
 * よう、待ちの合間に sched_poll() を挟む (getc の
 * 待ちと同じ作法)。ポーリングは速いので、スケジューラ
 * へは 1024 回に 1 回だけ顔を出す */
static void i2s_push_frame(uint16_t l, uint16_t r)
{
    uint32_t spin = 0;
    while ((I2S_CH1_ISR & ISR_TXFE) == 0) {
        stat_waits++;
        if (I2S_CH1_ISR & ISR_TXFO) {
            stat_over++;
            (void)I2S_CH1_TOR;  /* 読むと消える */
        }
        if ((++spin & 0x3FF) == 0) {
            sched_poll();
        }
    }
    I2S_CH1_LTHR = (uint32_t)l;
    I2S_CH1_RTHR = (uint32_t)r;
    stat_frames++;
}

int board_audio_write(const unsigned char *buf, uint64_t nbytes,
                      uint64_t channels)
{
    uint64_t i;
    if (!i2s_open_flag || (nbytes & 1) != 0) {
        return -1;
    }
    if (channels == 1) {
        /* モノラルは左右へ複製 (仕様側の決定) */
        for (i = 0; i + 1 < nbytes; i += 2) {
            uint16_t s = (uint16_t)(buf[i] |
                                    ((uint16_t)buf[i + 1] << 8));
            i2s_push_frame(s, s);
        }
    } else {
        for (i = 0; i + 3 < nbytes; i += 4) {
            uint16_t l = (uint16_t)(buf[i] |
                                    ((uint16_t)buf[i + 1] << 8));
            uint16_t r = (uint16_t)(buf[i + 2] |
                                    ((uint16_t)buf[i + 3] << 8));
            i2s_push_frame(l, r);
        }
    }
    return 0;
}

void board_audio_close(void)
{
    if (!i2s_open_flag) {
        return;
    }
    i2s_open_flag = 0;
    kputs("audio: i2s frames=");
    kput_dec(stat_frames);
    kputs(" waits=");
    kput_dec(stat_waits);
    kputs(" over=");
    kput_dec(stat_over);
    kputc('\n');
    /* 送信を止める。FIFO に残った分 (最大 16 フレーム、
     * 0.4ms 弱) は鳴り切らずに捨てる (割り切り) */
    I2S_CH1_TER = 0;
    I2S_ITER = 0;
    I2S_CER = 0;
}
