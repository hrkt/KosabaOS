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
 *   - DMA は使わない。FIFO への供給は共通コードの
 *     リングバッファ + フィーダ (trap.c) が担い、この
 *     ドライバは board_audio_try_push で「今 1 フレーム
 *     入るか」にだけ答える (追補3) */

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
#define SYSCTL_PLL_LOCK    REG32(SYSCTL_BASE + 0x18)
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

/* ---- DMAC (doc/audio-dma-design.md。チャンネル 0 を
 * I2S0 TX 専用に使う) ---- */
#define REG64(a) (*(volatile uint64_t *)(a))
#define DMAC_CFG    REG64(0x50000010ULL)
#define DMAC_CHEN   REG64(0x50000018ULL)
#define DMAC_CH0_SAR   REG64(0x50000100ULL)
#define DMAC_CH0_DAR   REG64(0x50000108ULL)
#define DMAC_CH0_BLOCK REG64(0x50000110ULL)
#define DMAC_CH0_CTL   REG64(0x50000118ULL)
#define DMAC_CH0_CFG   REG64(0x50000120ULL)
#define DMAC_CH0_INTCLEAR REG64(0x50000198ULL)
#define DMAC_CH0_STATUS   REG64(0x50000130ULL)
#define DMAC_CH0_INTSTATUS REG64(0x50000188ULL)
#define SYSCTL_DMA_SEL0 REG32(SYSCTL_BASE + 0x64)
#define I2S_TXDMA_ADDR (I2S0_BASE + 0x1C8)
#define DMA_SEL_I2S0_TX 25u   /* sysctl の DMA 選択値 */

/* DMA 供給 (doc/audio-dma-design.md): open で DMA が
 * 使えれば i2s_dma_mode が立ち、共通コード (trap.c) は
 * リングを直接 DMAC に読ませる。AUDIO_DMA_OFF を
 * 定義すると従来の FIFO 供給に戻る (A/B 比較用) */


/* DMA まわりの前方宣言 (実体はファイル後半) */
static void dma_init_once(void);
void board_audio_dma_stop(void);
static int dma_ready;      /* DMAC の初期化が済んだか */
static int i2s_dma_mode;   /* この open は DMA 供給か */

/* PLL2 = 26MHz / (0+1) × (25+1) / (14+1) = 45.0667MHz */
#define PLL2_HZ 45066666ULL

static int i2s_ready;        /* クロックと PLL2 の設定済み */
static int i2s_open_flag;    /* board_audio_open の状態 */
static uint64_t i2s_actual_rate; /* open で確定した実効レート (Hz) */
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
    /* ロック待ち。判定は SDK の sysctl_pll_is_lock
     * どおり pll_lock2 (bit17:16) の下位ビットで見る
     * (PLL0 の == 3 とは流儀が違う) */
    for (i = 0; i < 1000000; i++) {
        if ((SYSCTL_PLL_LOCK >> 16) & 0x1u) {
            break;
        }
    }
    v = SYSCTL_PLL2;
    v |= (1u << 25);             /* out_en を上げる */
    SYSCTL_PLL2 = v;
    return ((SYSCTL_PLL_LOCK >> 16) & 0x1u) ? 0 : -1;
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
    /* 供給方式の決定: DMA が初期化できれば DMA 供給 */
#ifdef AUDIO_DMA_OFF
    i2s_dma_mode = 0;
#else
    dma_init_once();
    i2s_dma_mode = dma_ready;
#endif
    /* threshold = round(pll2 / (rate × 128)) − 1。
     * 実レートが要求の 1% を超えてずれる組み合わせは
     * 受けない (48000 はここで落ちる) */
    th = (PLL2_HZ + rate * 64) / (rate * 128);
    if (th == 0 || th > 0x10000) {
        return -1;
    }
    th -= 1;
    actual = PLL2_HZ / ((th + 1) * 128);
    i2s_actual_rate = actual;
    diff = (actual > rate) ? (actual - rate) : (rate - actual);
    if (diff * 100 > rate) {
        return -1;
    }
    SYSCTL_CLK_TH3 =
        (SYSCTL_CLK_TH3 & 0xFFFF0000u) | (uint32_t)th;

    /* I2S0 の設定 (SDK の i2s_init + tx_channel_config の順) */
    /* 他チャンネルの送信を明示的に止める。txdma は
     * ブロック共有の口で、有効な全チャンネルへ語が
     * 分配される疑いがあるため (2026-11-21 の切り分け) */
    REG32(I2S0_BASE + 0x2C) = 0;  /* CH0 TER */
    REG32(I2S0_BASE + 0xAC) = 0;  /* CH2 TER */
    REG32(I2S0_BASE + 0xEC) = 0;  /* CH3 TER */
    I2S_CH1_TER = 0;
    I2S_IER = 1;
    I2S_IRER = 0;
    I2S_ITER = 1;
    I2S_TXFFR = 1;               /* 送信ブロックの FIFO を流す */
    I2S_CH1_TFF = 1;             /* チャンネルの FIFO を流す */
    I2S_CH1_TCR = TCR_VALUE;
    /* DMA のビットは送信開始前に CCR へ入れる (開始後に
     * 書き換えてもハードが DMA 要求を出さない)。
     * divide_16 (bit10) は使わない: リングの形は SDK の
     * 32 ビット経路と同じ 1 ワード = 1 チャンネル
     * サンプルに揃えた (doc/audio-dma-design.md) */
    I2S_CCR = CCR_VALUE |
        (i2s_dma_mode ? (1u << 8) : 0u);
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

/* 1 フレーム (左右 1 組) を FIFO へ。空きが無ければ待たずに
 * 0 を返す (入れたら 1)。供給のペーシングは共通コードの
 * リングバッファ + フィーダ (trap.c) が持ち、ドライバは
 * 「今入るか」だけに答える (doc/audio-driver-design.md
 * 追補3)。waits は「空きが無くて断られた回数」の数えに
 * 意味が変わった */
int board_audio_try_push(uint16_t l, uint16_t r)
{
    if (!i2s_open_flag) {
        return 0;
    }
    if (I2S_CH1_ISR & ISR_TXFO) {
        stat_over++;
        (void)I2S_CH1_TOR;  /* 読むと消える */
    }
    if ((I2S_CH1_ISR & ISR_TXFE) == 0) {
        stat_waits++;
        return 0;
    }
    I2S_CH1_LTHR = (uint32_t)l;
    I2S_CH1_RTHR = (uint32_t)r;
    stat_frames++;
    return 1;
}

/* ---- DMAC の最小ドライバ (チャンネル 0、メモリ→
 * I2S0 txdma、32 ビット幅、単一ブロック) ---- */
static void dma_init_once(void)
{
    if (dma_ready) {
        return;
    }
    SYSCTL_CLK_EN_PERI |= (1u << 1);   /* DMAC のクロック */
    /* ch0 のハンドシェイク相手 = I2S0 TX (欄は 6 ビット) */
    SYSCTL_DMA_SEL0 = (SYSCTL_DMA_SEL0 & ~0x3Fu) | DMA_SEL_I2S0_TX;
    DMAC_CFG |= 0x3;                   /* dmac_en + int_en */
    dma_ready = 1;
}

int board_audio_dma_active(void)
{
    return i2s_dma_mode;
}

/* DMA で流れた実フレーム数の帳簿 (close の要約行用) */
void board_audio_dma_note_frames(uint64_t frames)
{
    stat_frames += frames;
}

/* open で確定した実効レート (未 open なら 0) */
uint64_t board_audio_actual_rate(void)
{
    return i2s_actual_rate;
}

/* buf (1 ワード = 1 チャンネルサンプルの列) を words
 * ワード分、I2S へ流すブロック転送を張る */
void board_audio_dma_arm(const uint32_t *buf, uint64_t words)
{
    uint64_t v;
    int i;
    for (i = 0; i < 1000000; i++) {   /* 前の転送の完了待ち */
        if ((DMAC_CHEN & 1u) == 0) {
            break;
        }
    }
    DMAC_CH0_INTCLEAR = 0xFFFFFFFFu;
    /* cfg: tt_fc = MEM2PRF (1) を [34:32]、hs_sel_src =
     * ソフト (bit35)、hs_sel_dst = ハード (bit36 = 0)、
     * src_per/dst_per = チャンネル番号 0 */
    v = DMAC_CH0_CFG;
    v &= ~((7ULL << 32) | (1ULL << 36) | (7ULL << 39) |
           (7ULL << 42));
    v |= (1ULL << 32) | (1ULL << 35);
    DMAC_CH0_CFG = v;
    /* ctl: dms = MASTER2 (bit2)、dinc = 固定 (bit6)、
     * sinc = 増やす (bit4 = 0)、幅 32 ビット
     * (src [10:8] = 2、dst [13:11] = 2)、バースト 1 */
    v = DMAC_CH0_CTL;
    v &= ~((1ULL << 0) | (1ULL << 4) | (7ULL << 8) |
           (7ULL << 11) | (0xFULL << 14) | (0xFULL << 18));
    v |= (1ULL << 2) | (1ULL << 6) | (2ULL << 8) | (2ULL << 11);
    DMAC_CH0_CTL = v;
    DMAC_CH0_SAR = (uint64_t)(uintptr_t)buf;
    DMAC_CH0_DAR = (uint64_t)I2S_TXDMA_ADDR;
    DMAC_CH0_BLOCK = words - 1;   /* 語数 - 1 */
    DMAC_CHEN = (1ULL << 0) | (1ULL << 8);   /* ch0 有効化 */
}

int board_audio_dma_done(void)
{
    return (DMAC_CHEN & 1u) == 0;
}

void board_audio_dma_stop(void)
{
    int i;
    DMAC_CHEN = (1ULL << 8);   /* we だけ立てて値を 0 に */
    for (i = 0; i < 1000000; i++) {
        if ((DMAC_CHEN & 1u) == 0) {
            break;
        }
    }
}

void board_audio_close(void)
{
    if (!i2s_open_flag) {
        return;
    }
    if (i2s_dma_mode) {
        board_audio_dma_stop();  /* 止め忘れの保険 */
        i2s_dma_mode = 0;
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
