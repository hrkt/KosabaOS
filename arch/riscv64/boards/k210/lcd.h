#ifndef KOSABAOS_BOARD_K210_LCD_H
#define KOSABAOS_BOARD_K210_LCD_H

/* lcd.h (k210) -- Maix Dock の LCD (ST7789) ドライバの公開部分。
 * 実装は lcd.c。開発の経緯と落とし穴は doc/k210-lcd-driver.md、
 * 仕組みの解説は doc/lcd-basics.md。
 *
 * 使い方は 3 層に分かれている:
 *   - 基本描画: lcd_init / lcd_fill_rect / lcd_draw_text
 *   - 色は LCD_RGB で作る (RGB565 への変換はここだけ)
 *   - tick 表示: lcd_tick_setup で場所と色を覚えさせると、
 *     以後は lcd_tick_draw(秒数) でその場所だけを描き直す。
 *     setup 前の tick_draw は何もしない (起動中の描画と
 *     割り込みからの描画がぶつからないための番人でもある)
 */

#include <stdint.h>

/* 横向き (MADCTL 0xA0) の論理サイズ */
#define LCD_WIDTH  320
#define LCD_HEIGHT 240

/* RGB 各 0..255 -> RGB565 */
#define LCD_RGB(r, g, b) \
    ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

#define LCD_BLACK LCD_RGB(0, 0, 0)
#define LCD_WHITE LCD_RGB(255, 255, 255)
#define LCD_RED   LCD_RGB(255, 0, 0)
#define LCD_GREEN LCD_RGB(0, 255, 0)
#define LCD_CYAN  LCD_RGB(0, 255, 255)

/* ピン割り当て・クロック・リセット・ST7789 初期化までを行う */
void lcd_init(void);

/* 矩形を単色で塗る */
void lcd_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint16_t color);

/* 5x7 フォントで文字列を描く。1 文字の幅は 6*scale (5+余白 1)。
 * 対応する文字は A-Z、0-9、空白と ! + - . : / > (他は空白になる) */
void lcd_draw_text(uint32_t x, uint32_t y, const char *s, uint32_t scale,
                   uint16_t color);

/* 10 進の数を描き、描いた幅 (ピクセル) を返す。文字列と
 * つなげて 1 行を作るときに使う */
uint32_t lcd_draw_u32(uint32_t x, uint32_t y, uint64_t v, uint32_t scale,
                      uint16_t color);

/* tick 表示の場所・大きさ・色を覚える。以後の lcd_tick_draw が
 * 有効になる */
void lcd_tick_setup(uint32_t x, uint32_t y, uint32_t scale,
                    uint16_t fg, uint16_t bg);

/* 覚えた場所に秒数 (10 進) を描き直す。setup 前は何もしない。
 * 割り込みハンドラから呼ばれる想定で、描画はこの 1 箇所だけ */
void lcd_tick_draw(uint64_t seconds);

#endif
