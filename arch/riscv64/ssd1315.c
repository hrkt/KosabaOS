/* ssd1315.c -- SSD1315 OLED ドライバ (共通層)
 *
 * 購入したモジュールは SSD1315 (2026-11-19 に判明)。
 * SSD1315 は SSD1306 とコマンド互換・同じ 128x64 の
 * ページメモリ・同じ I2C アドレスで、初期化列もそのまま
 * 使える (違いは発振周波数まわり程度。ちらつく場合は
 * D5 / D9 の値を段3で調整する: doc/oled-design.md)。
 * 128x64 モノクロ、I2C アドレス 0x3C (モジュールにより
 * 0x3D のものもある。到着時に実物で確認する)。
 * 輸送はボード層の board_i2c_write (i2c.h) だけに依存する。
 * virt ではダミーが常に成功を返すので、この層の検証は
 * virt の klog で行う (gfx の virt ログと同じ流儀):
 *   oled: init ok
 *   oled: text "KOSABAOS"
 * 実機でモジュールが無いときは最初の送信で ACK が無く、
 * oled: no ack と 1 行残して以後は無言になる (起動は続ける)。
 *
 * フレームバッファは 128x64 / 8 = 1024 バイト。
 * SSD1315 の GDDRAM と同じ並び (ページ = 8 行の帯、
 * バイト内 bit0 が帯の最上行) なので、そのまま送れる。
 * フォントは LCD と同じ 5x7 大文字フォント (lcd.c の表の
 * 複製。字形の並びも同じ: テーブルの bit6 が最上行)。
 */

#include <stdint.h>
#include "ssd1315.h"
#include "i2c.h"
#include "klog.h"

#define OLED_ADDR 0x3C
#define OLED_W 128
#define OLED_PAGES 8
#define OLED_LINE_CHARS 21   /* 128 / 6 = 21.33 → 21 文字 */
#define OLED_MAX_LINES 8

static uint8_t oled_fb[OLED_W * OLED_PAGES];
static int oled_is_present;

/* コマンド列を 1 トランザクションで送る (先頭は制御バイト 0x00) */
static int oled_send_cmds(const uint8_t *cmds, uint32_t n)
{
    uint8_t buf[32];
    uint32_t i;
    if (n + 1 > sizeof(buf))
        return -1;
    buf[0] = 0x00;
    for (i = 0; i < n; i++)
        buf[i + 1] = cmds[i];
    return board_i2c_write(OLED_ADDR, buf, n + 1);
}

static int oled_flush(void)
{
    static const uint8_t win[] = {0x21, 0, 127, 0x22, 0, 7};
    uint8_t buf[129];
    uint32_t off, i;

    if (!oled_is_present)
        return -1;
    if (oled_send_cmds(win, sizeof(win)) != 0)
        return -1;
    buf[0] = 0x40;   /* データの制御バイト */
    for (off = 0; off < sizeof(oled_fb); off += 128) {
        for (i = 0; i < 128; i++)
            buf[i + 1] = oled_fb[off + i];
        if (board_i2c_write(OLED_ADDR, buf, 129) != 0)
            return -1;
    }
    return 0;
}

static void oled_clear_fb(void)
{
    uint32_t i;
    for (i = 0; i < sizeof(oled_fb); i++)
        oled_fb[i] = 0;
}

static int oled_init(void)
{
    /* SSD1315 の初期化列 (SSD1306 と共通、128x64、チャージポンプ有効) */
    static const uint8_t init_seq[] = {
        0xAE,             /* 表示 off */
        0xD5, 0x80,       /* クロック分周 */
        0xA8, 0x3F,       /* multiplex = 64 行 */
        0xD3, 0x00,       /* 表示オフセット 0 */
        0x40,             /* 先頭行 0 */
        0x8D, 0x14,       /* チャージポンプ on */
        0x20, 0x00,       /* アドレッシング = 横方向 */
        0xA1,             /* segment remap */
        0xC8,             /* COM 逆方向スキャン */
        0xDA, 0x12,       /* COM ピン構成 (128x64) */
        0x81, 0xCF,       /* コントラスト */
        0xD9, 0xF1,       /* pre-charge */
        0xDB, 0x40,       /* VCOMH */
        0xA4,             /* GDDRAM の内容を表示 */
        0xA6,             /* 通常表示 (非反転) */
        0xAF              /* 表示 on */
    };
    if (oled_send_cmds(init_seq, sizeof(init_seq)) != 0) {
        lkputs("oled: no ack\n");
        oled_is_present = 0;
        return -1;
    }
    oled_is_present = 1;
    oled_clear_fb();
    (void)oled_flush();
    lkputs("oled: init ok\n");
    return 0;
}

/* ---- 5x7 フォント (lcd.c の表の複製) ---- */
static const uint8_t *oled_glyph(char c) {
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

/* ---- 描画 ---- */

static void oled_set_px(uint32_t x, uint32_t y)
{
    if (x >= OLED_W || y >= OLED_PAGES * 8)
        return;
    oled_fb[x + (y / 8) * OLED_W] |= (uint8_t)(1u << (y % 8));
}

/* 1 文字を (px, page) の位置へ描く。glyph の bit6 が最上行 */
static void oled_draw_char(uint32_t px, uint32_t page, char c)
{
    const uint8_t *g = oled_glyph(c);
    int col, row;
    for (col = 0; col < 5; col++) {
        for (row = 0; row < 7; row++) {
            if (g[col] & (1u << (6 - row)))
                oled_set_px(px + (uint32_t)col, page * 8 + (uint32_t)row);
        }
    }
}

static char oled_upper(char c)
{
    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return c;
}

/* 1 行分 (最大 21 文字) を描いて klog に残す */
static void oled_draw_line(uint32_t line, const char *s, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        oled_draw_char(i * 6, line, oled_upper(s[i]));
    lkputs("oled: text \"");
    for (i = 0; i < len; i++)
        lkputc(oled_upper(s[i]));
    lkputs("\"\n");
}

int oled_text_multi(const char *s)
{
    /* 単語ごとに組み立てて行に詰める (ima と同じ流儀:
     * 単語 "|" は改行、入らない単語は次の行へ) */
    char lines[OLED_MAX_LINES][OLED_LINE_CHARS + 1];
    uint32_t nlines = 0;
    uint32_t cur = 0;   /* 現在の行の文字数 */
    uint32_t i;

    if (!oled_is_present)
        return -1;

    for (i = 0; i < OLED_MAX_LINES; i++)
        lines[i][0] = '\0';

    while (*s) {
        const char *w;
        uint32_t wlen = 0;
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        w = s;
        while (s[wlen] && s[wlen] != ' ')
            wlen++;
        s += wlen;

        if (wlen == 1 && w[0] == '|') {
            if (nlines + 1 < OLED_MAX_LINES) {
                nlines++;
                cur = 0;
            }
            continue;
        }
        if (wlen > OLED_LINE_CHARS)
            wlen = OLED_LINE_CHARS;   /* 長い単語は切る */
        if (cur > 0 && cur + 1 + wlen > OLED_LINE_CHARS) {
            if (nlines + 1 >= OLED_MAX_LINES)
                break;
            nlines++;
            cur = 0;
        }
        if (cur > 0)
            lines[nlines][cur++] = ' ';
        {
            uint32_t k;
            for (k = 0; k < wlen; k++)
                lines[nlines][cur++] = w[k];
        }
        lines[nlines][cur] = '\0';
    }
    if (cur > 0 || nlines == 0)
        nlines++;

    oled_clear_fb();
    for (i = 0; i < nlines; i++) {
        uint32_t len = 0;
        while (lines[i][len])
            len++;
        if (len > 0)
            oled_draw_line(i, lines[i], len);
    }
    return oled_flush();
}

int oled_clear_flush(void)
{
    if (!oled_is_present)
        return -1;
    oled_clear_fb();
    return oled_flush();
}

void oled_bootstrap(void)
{
    if (oled_init() != 0)
        return;
    (void)oled_text_multi("KOSABAOS | OLED OK");
}
