/* lcd.c (k210) -- Maix Dock の LCD (ST7789) ドライバ。
 *
 * spike-k210/step4-lcd で実機検証したドライバの本体移植版。
 * 配線・転送方式・落とし穴の詳細は doc/k210-lcd-driver.md。
 * 要点だけ再掲:
 *   - CS=IO36/SPI0_SS3、CLK=IO39、RST=IO37/GPIOHS30、DC=IO38/GPIOHS31
 *   - データ線は sysctl の spi0_dvp_data で DVP 側へ出す
 *   - 転送は SPI0 octal (コマンド=instruction フェーズ、
 *     データ=address フェーズ)
 *   - コマンドのパラメータは 8bit address フェーズでは届かない。
 *     16bit フェーズに 2 バイトずつ詰めて送る (st7789_params)
 *   - 画素は窓ごとに 1 本の連続ストリームで送る
 */

#include <stdint.h>
#include "lcd.h"
#include "time.h"
#include "klog.h"

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define FPIOA_BASE 0x502B0000

static void delay_ms(uint32_t ms) {
    /* 390MHz でおよそ 1ms になる雑なビジーループ (初期化待ち用) */
    for (volatile uint32_t i = 0; i < ms * 130000u; i++) {
    }
}

/* ---- GPIOHS (RST / DC 用) ---- */
#define GPIOHS_OUTPUT_EN REG32(0x38001008)
#define GPIOHS_OUTPUT_VAL REG32(0x3800100C)
#define PIN_RST 30  /* GPIOHS30 = IO37 */
#define PIN_DC  31  /* GPIOHS31 = IO38 */

static void gpiohs_out(int pin, int val) {
    GPIOHS_OUTPUT_EN |= (1u << pin);
    if (val)
        GPIOHS_OUTPUT_VAL |= (1u << pin);
    else
        GPIOHS_OUTPUT_VAL &= ~(1u << pin);
}

/* ---- SPI0 (DesignWare SSI, 0x52000000) ---- */
#define SPI0 0x52000000
#define SPI_CTRLR0    REG32(SPI0 + 0x00)
#define SPI_SSIENR    REG32(SPI0 + 0x08)
#define SPI_SER       REG32(SPI0 + 0x10)
#define SPI_BAUDR     REG32(SPI0 + 0x14)
#define SPI_TXFLR     REG32(SPI0 + 0x20)
#define SPI_SR        REG32(SPI0 + 0x28)
#define SPI_IMR       REG32(SPI0 + 0x2C)
#define SPI_DMACR     REG32(SPI0 + 0x4C)
#define SPI_DMATDLR   REG32(SPI0 + 0x50)
#define SPI_DMARDLR   REG32(SPI0 + 0x54)
#define SPI_DR        REG32(SPI0 + 0x60)
/* 非標準転送 (octal) の制御レジスタ (SDK spi.h でオフセット 0xF4) */
#define SPI_XFER_CTRL REG32(SPI0 + 0xF4)

static void spi_init_octal(uint32_t data_bits) {
    if (SPI_BAUDR == 0) SPI_BAUDR = 0x14;
    SPI_IMR = 0;
    SPI_DMACR = 0;
    SPI_DMATDLR = 0x10;
    SPI_DMARDLR = 0;
    SPI_SER = 0;
    SPI_SSIENR = 0;
    /* mode0, FF_OCTAL=3 (frf 21), TMOD=1 (送信のみ), dfs (16) */
    SPI_CTRLR0 = (3u << 21) | (1u << 8) | ((data_bits - 1) << 16);
    SPI_XFER_CTRL = 0;
}

static void spi_non_standard(uint32_t inst_bits, uint32_t addr_bits) {
    uint32_t inst_l = (inst_bits == 8) ? 2 : 0;
    SPI_XFER_CTRL = (inst_l << 8) | ((addr_bits / 4) << 2) | 2u;
}

static void spi_send(const uint32_t *vals, uint32_t count) {
    SPI_SSIENR = 1;
    SPI_SER = 1u << 3; /* SS3 */
    uint32_t i = 0;
    while (i < count) {
        uint32_t room = 32 - SPI_TXFLR;
        while (room-- && i < count) {
            SPI_DR = vals[i++];
        }
    }
    while ((SPI_SR & 0x05) != 0x04) {
    }
    SPI_SER = 0;
    SPI_SSIENR = 0;
}

/* ---- ST7789 への送信 (変換はこの 2 つに閉じる) ---- */

static void st7789_cmd(uint32_t cmd) {
    gpiohs_out(PIN_DC, 0);
    spi_init_octal(8);
    spi_non_standard(8, 0);
    spi_send(&cmd, 1);
}

/* パラメータ列を 16bit address フェーズで送る。2 バイトを
 * 1 つの uint16 (上位が先) に詰め、奇数長は末尾 0x00
 * (パネル側では NOP として捨てられる) で偶数にする */
static void st7789_params(const uint8_t *d, uint32_t len) {
    uint32_t vals[4];
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i += 2) {
        uint32_t hi = d[i];
        uint32_t lo = (i + 1 < len) ? d[i + 1] : 0x00;
        vals[n++] = (hi << 8) | lo;
    }
    gpiohs_out(PIN_DC, 1);
    spi_init_octal(16);
    spi_non_standard(0, 16);
    spi_send(vals, n);
}

static void st7789_set_window(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    uint8_t d[4];
    d[0] = (x0 >> 8) & 0xFF; d[1] = x0 & 0xFF; d[2] = (x1 >> 8) & 0xFF; d[3] = x1 & 0xFF;
    st7789_cmd(0x2A); /* CASET */
    st7789_params(d, 4);
    d[0] = (y0 >> 8) & 0xFF; d[1] = y0 & 0xFF; d[2] = (y1 >> 8) & 0xFF; d[3] = y1 & 0xFF;
    st7789_cmd(0x2B); /* RASET */
    st7789_params(d, 4);
    st7789_cmd(0x2C); /* RAMWR */
}

/* ---- 公開 API ---- */

void lcd_init(void) {
    REG32(FPIOA_BASE + 36 * 4) = 15 | 0x1F00;  /* SPI0_SS3 */
    REG32(FPIOA_BASE + 39 * 4) = 17 | 0x1F00;  /* SPI0_SCLK */
    REG32(FPIOA_BASE + 37 * 4) = 54 | 0x1F00;  /* GPIOHS30 (RST) */
    REG32(FPIOA_BASE + 38 * 4) = 55 | 0x1F00;  /* GPIOHS31 (DC) */

    REG32(0x50440028) |= (1u << 5);        /* clk_en_cent.apb2 */
    REG32(0x5044002C) |= (1u << 6);        /* clk_en_peri.spi0 */
    REG32(0x5044003C) &= ~0xFFu;           /* SPI0 threshold = 0 */
    REG32(0x50440054) |= (1u << 10);       /* misc.spi_dvp_data_enable */
    SPI_BAUDR = 26;                        /* 390MHz / 26 = 15MHz */

    gpiohs_out(PIN_DC, 1);
    gpiohs_out(PIN_RST, 1);
    gpiohs_out(PIN_RST, 0);
    spi_init_octal(8);
    delay_ms(50);
    gpiohs_out(PIN_RST, 1);
    delay_ms(50);

    st7789_cmd(0x01);         /* SWRESET */
    delay_ms(50);
    st7789_cmd(0x11);         /* SLPOUT */
    delay_ms(120);
    st7789_cmd(0x3A);         /* COLMOD: RGB565 */
    { uint8_t v = 0x55; st7789_params(&v, 1); }
    delay_ms(10);
    st7789_cmd(0x36);         /* MADCTL: DIR_YX_RLDU = 0xA0 (横向き) */
    { uint8_t v = 0xA0; st7789_params(&v, 1); }
    st7789_cmd(0x13);         /* NORON */
    delay_ms(10);
    st7789_cmd(0x29);         /* DISPON */
}

void lcd_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color) {
    if (w == 0 || h == 0) return;
    st7789_set_window(x, y, x + w - 1, y + h - 1);
    /* 窓の中を 1 本の連続ストリームで塗る */
    gpiohs_out(PIN_DC, 1);
    spi_init_octal(16);
    spi_non_standard(0, 16);
    SPI_SSIENR = 1;
    SPI_SER = 1u << 3;
    uint32_t n = w * h;
    uint32_t i = 0;
    while (i < n) {
        uint32_t room = 32 - SPI_TXFLR;
        while (room-- && i < n) {
            SPI_DR = color;
            i++;
        }
    }
    while ((SPI_SR & 0x05) != 0x04) {
    }
    SPI_SER = 0;
    SPI_SSIENR = 0;
}

/* ---- 5x7 フォント ----
 * 列バイトの並び: 1 文字 = 5 バイト (左の列から)、各バイトの
 * bit0 が最下行、bit6 が最上行。表は行文字列から機械生成し、
 * ASCII アートへ再描画して全字形を検品済み (2026-11-03)。
 * 直すときは同じ手順で検品すること。 */
static const uint8_t *glyph(char c) {
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const uint8_t gA[5] = {0x3F, 0x48, 0x48, 0x48, 0x3F};  /* A */
    static const uint8_t gB[5] = {0x7F, 0x49, 0x49, 0x49, 0x36};  /* B */
    static const uint8_t gC[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};  /* C */
    static const uint8_t gD[5] = {0x7F, 0x41, 0x41, 0x41, 0x3E};  /* D */
    static const uint8_t gE[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};  /* E */
    static const uint8_t gF[5] = {0x7F, 0x48, 0x48, 0x48, 0x40};  /* F */
    static const uint8_t gG[5] = {0x3E, 0x41, 0x49, 0x49, 0x2E};  /* G */
    static const uint8_t gH[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};  /* H */
    static const uint8_t gI[5] = {0x00, 0x41, 0x7F, 0x41, 0x00};  /* I */
    static const uint8_t gJ[5] = {0x02, 0x01, 0x41, 0x41, 0x7E};  /* J */
    static const uint8_t gK[5] = {0x7F, 0x08, 0x14, 0x22, 0x41};  /* K */
    static const uint8_t gL[5] = {0x7F, 0x01, 0x01, 0x01, 0x01};  /* L */
    static const uint8_t gM[5] = {0x7F, 0x20, 0x18, 0x20, 0x7F};  /* M */
    static const uint8_t gN[5] = {0x7F, 0x30, 0x08, 0x06, 0x7F};  /* N */
    static const uint8_t gO[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};  /* O */
    static const uint8_t gP[5] = {0x7F, 0x48, 0x48, 0x48, 0x30};  /* P */
    static const uint8_t gQ[5] = {0x3E, 0x41, 0x45, 0x42, 0x3D};  /* Q */
    static const uint8_t gR[5] = {0x7F, 0x48, 0x4C, 0x4A, 0x31};  /* R */
    static const uint8_t gS[5] = {0x31, 0x49, 0x49, 0x49, 0x46};  /* S */
    static const uint8_t gT[5] = {0x40, 0x40, 0x7F, 0x40, 0x40};  /* T */
    static const uint8_t gU[5] = {0x7E, 0x01, 0x01, 0x01, 0x7E};  /* U */
    static const uint8_t gV[5] = {0x7C, 0x02, 0x01, 0x02, 0x7C};  /* V */
    static const uint8_t gW[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};  /* W */
    static const uint8_t gX[5] = {0x41, 0x22, 0x1C, 0x22, 0x41};  /* X */
    static const uint8_t gY[5] = {0x60, 0x10, 0x0F, 0x10, 0x60};  /* Y */
    static const uint8_t gZ[5] = {0x43, 0x45, 0x49, 0x51, 0x61};  /* Z */
    static const uint8_t g0[5] = {0x3E, 0x45, 0x49, 0x51, 0x3E};  /* 0 */
    static const uint8_t g1[5] = {0x00, 0x21, 0x7F, 0x01, 0x00};  /* 1 */
    static const uint8_t g2[5] = {0x21, 0x43, 0x45, 0x49, 0x31};  /* 2 */
    static const uint8_t g3[5] = {0x22, 0x41, 0x49, 0x49, 0x36};  /* 3 */
    static const uint8_t g4[5] = {0x0C, 0x14, 0x24, 0x7F, 0x04};  /* 4 */
    static const uint8_t g5[5] = {0x72, 0x51, 0x51, 0x51, 0x4E};  /* 5 */
    static const uint8_t g6[5] = {0x3E, 0x49, 0x49, 0x49, 0x06};  /* 6 */
    static const uint8_t g7[5] = {0x40, 0x40, 0x4F, 0x50, 0x60};  /* 7 */
    static const uint8_t g8[5] = {0x36, 0x49, 0x49, 0x49, 0x36};  /* 8 */
    static const uint8_t g9[5] = {0x30, 0x49, 0x49, 0x49, 0x3E};  /* 9 */
    static const uint8_t gEx[5] = {0x00, 0x00, 0x7D, 0x00, 0x00};  /* ! */
    static const uint8_t gPlus[5] = {0x00, 0x08, 0x3E, 0x08, 0x00};  /* + */
    static const uint8_t gMinus[5] = {0x00, 0x08, 0x08, 0x08, 0x00};  /* - */
    static const uint8_t gDot[5] = {0x00, 0x03, 0x03, 0x00, 0x00};  /* . */
    static const uint8_t gColon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};  /* : */
    static const uint8_t gSlash[5] = {0x03, 0x04, 0x08, 0x10, 0x60};  /* / */
    static const uint8_t gGt[5] = {0x41, 0x22, 0x14, 0x08, 0x00};  /* > */
    switch (c) {
        case 'A': return gA;
        case 'B': return gB;
        case 'C': return gC;
        case 'D': return gD;
        case 'E': return gE;
        case 'F': return gF;
        case 'G': return gG;
        case 'H': return gH;
        case 'I': return gI;
        case 'J': return gJ;
        case 'K': return gK;
        case 'L': return gL;
        case 'M': return gM;
        case 'N': return gN;
        case 'O': return gO;
        case 'P': return gP;
        case 'Q': return gQ;
        case 'R': return gR;
        case 'S': return gS;
        case 'T': return gT;
        case 'U': return gU;
        case 'V': return gV;
        case 'W': return gW;
        case 'X': return gX;
        case 'Y': return gY;
        case 'Z': return gZ;
        case '0': return g0;
        case '1': return g1;
        case '2': return g2;
        case '3': return g3;
        case '4': return g4;
        case '5': return g5;
        case '6': return g6;
        case '7': return g7;
        case '8': return g8;
        case '9': return g9;
        case '!': return gEx;
        case '+': return gPlus;
        case '-': return gMinus;
        case '.': return gDot;
        case ':': return gColon;
        case '/': return gSlash;
        case '>': return gGt;
        default: return blank;
    }
}

void lcd_draw_text(uint32_t x, uint32_t y, const char *s, uint32_t scale, uint16_t color) {
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = glyph(*s);
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                /* 画面の最上行 (row=0) がテーブルの bit6 */
                if (g[col] & (1u << (6 - row))) {
                    lcd_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}

/* 矩形 (ax, ay, aw, ah) とクリップ領域 (cx, cy, cw, ch) の
 * 交わりを塗る。交わりが無ければ何もしない */
static void fill_intersect(uint32_t ax, uint32_t ay, uint32_t aw,
                           uint32_t ah, uint32_t cx, uint32_t cy,
                           uint32_t cw, uint32_t ch, uint16_t color) {
    uint32_t x0 = ax > cx ? ax : cx;
    uint32_t y0 = ay > cy ? ay : cy;
    uint32_t x1 = ax + aw < cx + cw ? ax + aw : cx + cw;
    uint32_t y1 = ay + ah < cy + ch ? ay + ah : cy + ch;
    if (x1 > x0 && y1 > y0) {
        lcd_fill_rect(x0, y0, x1 - x0, y1 - y0, color);
    }
}

/* lcd_draw_text と同じ描画を、クリップ領域 (cx, cy, cw, ch)
 * の内側だけで行う (画面表示の窓用、doc/gfx-design.md)。
 * 字形 1 ブロック (scale x scale) ごとに交わりを塗るので、
 * ブロックの一部だけが領域に掛かる場合もその分だけ出る */
void lcd_draw_text_clip(uint32_t x, uint32_t y, const char *s,
                        uint32_t scale, uint16_t color,
                        uint32_t cx, uint32_t cy, uint32_t cw,
                        uint32_t ch) {
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = glyph(*s);
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (g[col] & (1u << (6 - row))) {
                    fill_intersect(x + (uint32_t)col * scale,
                                   y + (uint32_t)row * scale,
                                   scale, scale, cx, cy, cw, ch,
                                   color);
                }
            }
        }
    }
}

uint32_t lcd_draw_u32(uint32_t x, uint32_t y, uint64_t v, uint32_t scale,
                      uint16_t color) {
    char buf[24];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char rev[24];
        int m = 0;
        while (v > 0) {
            rev[m++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (m > 0) buf[n++] = rev[--m];
    }
    buf[n] = '\0';
    lcd_draw_text(x, y, buf, scale, color);
    return (uint32_t)n * 6 * scale;
}

/* ---- tick 表示 (時計タスクから毎秒呼ばれる。スケジューラ
 * 段2 で割り込みハンドラから移った) ----
 * 描く中身には 2 つの顔がある:
 *   - RTC が有効な日時を返す間は壁時計 "HH:MM:SS" (UTC)
 *   - それ以外 (未設定) は従来どおり起動秒数
 * 起動 → コンソールの 't' で合わせる → 次の秒から壁時計に
 * 切り替わる。切り替わった事実は 1 回だけ klog に残す
 * (毎秒は残さない) */
static int tick_armed;
static uint32_t tick_x, tick_y, tick_scale;
static uint16_t tick_fg, tick_bg;
static int tick_digits;   /* 前回描いた文字数 (消去幅の計算用) */
static int tick_wall;     /* 壁時計表示に切り替わったか */
/* セーバー段1 の移動用: 今のオフセットと、最後に実際に
 * 描いた位置 (消去はそちらで行う) */
static int tick_off_x, tick_off_y;
static uint32_t tick_drawn_x, tick_drawn_y;

void lcd_tick_setup(uint32_t x, uint32_t y, uint32_t scale,
                    uint16_t fg, uint16_t bg) {
    tick_x = x;
    tick_y = y;
    tick_scale = scale;
    tick_fg = fg;
    tick_bg = bg;
    tick_digits = 0;
    tick_off_x = 0;
    tick_off_y = 0;
    tick_drawn_x = x;
    tick_drawn_y = y;
    tick_armed = 1;
    /* 帯のラベルは帯の一部としてここで描く (従来は
     * kernel.c が描いていた。セーバーの帯再描画をこの
     * ファイルの中で閉じるため) */
    lcd_draw_text(28, y, "TICK", scale, fg);
}

static void tick_two_digits(char *p, int v)
{
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}

/* 表示文字列を作る (tick と段2 のセーバーで共用)。
 * 戻り値は文字数。壁時計への切り替わりの記録もここで
 * 1 回だけ行う */
static int tick_format(char *buf, uint64_t seconds)
{
    int n = 0;
    struct datetime dt;
    if (time_read(&dt) == 0) {
        /* 壁時計 "HH:MM:SS" */
        tick_two_digits(buf + 0, dt.hour);
        buf[2] = ':';
        tick_two_digits(buf + 3, dt.minute);
        buf[5] = ':';
        tick_two_digits(buf + 6, dt.second);
        n = 8;
        buf[n] = '\0';
        if (!tick_wall) {
            /* 切り替わりの節目だけ、描いた値を記録に残す */
            char fbuf[20];
            tick_wall = 1;
            time_format(&dt, fbuf);
            kputs("lcd: wall clock ");
            kputs(fbuf);
            kputs(" UTC\n");
        }
    } else {
        /* 10 進の文字列化 (このファイルの中で完結させる) */
        if (seconds == 0) {
            buf[n++] = '0';
        } else {
            char rev[24];
            int m = 0;
            while (seconds > 0) {
                rev[m++] = (char)('0' + (seconds % 10));
                seconds /= 10;
            }
            while (m > 0) buf[n++] = rev[--m];
        }
        buf[n] = '\0';
    }
    return n;
}

void lcd_tick_draw(uint64_t seconds) {
    if (!tick_armed) return;
    char buf[24];
    int n = tick_format(buf, seconds);
    /* 前回の数字を丸ごと背景で消してから描く。lcd_draw_text は
     * 点の部分しか塗らないので、消さずに重ねると前の数字が
     * 残って数秒で真っ平らな四角になってしまう。消す場所は
     * 「最後に描いた位置」で、オフセットが変わっていても
     * 前回分を正しく消せる */
    if (tick_digits > 0) {
        lcd_fill_rect(tick_drawn_x, tick_drawn_y,
                      6 * tick_scale * (uint32_t)tick_digits,
                      7 * tick_scale, tick_bg);
    }
    tick_digits = n;
    tick_drawn_x = tick_x + (uint32_t)tick_off_x;
    tick_drawn_y = tick_y + (uint32_t)tick_off_y;
    lcd_draw_text(tick_drawn_x, tick_drawn_y, buf, tick_scale,
                  tick_fg);
}

void lcd_tick_set_offset(int dx, int dy)
{
    tick_off_x = dx;
    tick_off_y = dy;
}

void lcd_tick_repaint(uint64_t seconds)
{
    if (!tick_armed) return;
    /* 帯ごと地色で消し直し、ラベルと時刻を定位置へ。
     * セーバー段2 が帯を覆った後の復帰用 */
    lcd_fill_rect(0, tick_y, LCD_WIDTH, LCD_HEIGHT - tick_y,
                  tick_bg);
    lcd_draw_text(28, tick_y, "TICK", tick_scale, tick_fg);
    tick_off_x = 0;
    tick_off_y = 0;
    tick_digits = 0;
    tick_drawn_x = tick_x;
    tick_drawn_y = tick_y;
    lcd_tick_draw(seconds);
}

/* ---- セーバー段2 (全面) の描画 ----
 * 地色は暗い紺、文字は淡い水色で scale 4。文字列と
 * 消去の作法は tick と同じ (前回分を地色で消してから
 * 描く)。位置の歩き方は saver.c が決める */
#define SAVER2_BG    LCD_RGB(8, 8, 24)
#define SAVER2_FG    LCD_RGB(140, 220, 255)
#define SAVER2_SCALE 4

static uint32_t s2_prev_x, s2_prev_y;
static int s2_prev_len;

void lcd_saver2_begin(void)
{
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, SAVER2_BG);
    s2_prev_len = 0;
}

void lcd_saver2_draw(uint64_t seconds, int x, int y)
{
    char buf[24];
    int n = tick_format(buf, seconds);
    if (s2_prev_len > 0) {
        lcd_fill_rect(s2_prev_x, s2_prev_y,
                      6 * SAVER2_SCALE * (uint32_t)s2_prev_len,
                      7 * SAVER2_SCALE, SAVER2_BG);
    }
    s2_prev_len = n;
    s2_prev_x = (uint32_t)x;
    s2_prev_y = (uint32_t)y;
    lcd_draw_text(s2_prev_x, s2_prev_y, buf, SAVER2_SCALE,
                  SAVER2_FG);
}
