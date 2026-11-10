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
 *     以後は lcd_tick_draw でその場所だけを描き直す (中身は
 *     壁時計か起動秒数。lcd.c の tick 表示の節を参照)。
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

/* lcd_draw_text と同じ描画を、クリップ領域 (cx, cy, cw, ch)
 * の内側だけで行う (画面表示の窓用、doc/gfx-design.md) */
void lcd_draw_text_clip(uint32_t x, uint32_t y, const char *s,
                        uint32_t scale, uint16_t color,
                        uint32_t cx, uint32_t cy, uint32_t cw,
                        uint32_t ch);

/* 10 進の数を描き、描いた幅 (ピクセル) を返す。文字列と
 * つなげて 1 行を作るときに使う */
uint32_t lcd_draw_u32(uint32_t x, uint32_t y, uint64_t v, uint32_t scale,
                      uint16_t color);

/* tick 表示の場所・大きさ・色を覚える。以後の lcd_tick_draw が
 * 有効になる */
void lcd_tick_setup(uint32_t x, uint32_t y, uint32_t scale,
                    uint16_t fg, uint16_t bg);

/* 覚えた場所の表示を描き直す。RTC が有効な日時を返す間は
 * 壁時計 "HH:MM:SS" (UTC)、それ以外は引数の秒数 (10 進)。
 * setup 前は何もしない。スケジューラ段2 以後は時計タスク
 * (proc.c の clock_body) から呼ばれる。描画はこの 1 箇所だけ */
void lcd_tick_draw(uint64_t seconds);

/* tick 表示のオフセット (スクリーンセーバー段1 用、
 * doc/screensaver-design.md)。次の lcd_tick_draw から
 * 効く。前回分は前回描いた位置で消されるので、表示の
 * 移動はこの口だけで実現する */
void lcd_tick_set_offset(int dx, int dy);

/* 時計帯を丸ごと描き直す (セーバー段2 からの復帰用):
 * 帯を地色で消し、TICK ラベルと時刻を定位置に描く */
void lcd_tick_repaint(uint64_t seconds);

/* セーバー段2 (全面) の描画口: begin で全面をセーバーの
 * 地色で消し、以後 draw が指定位置に大きな時刻を描く
 * (前回分は地色で消してから描く) */
void lcd_saver2_begin(void);
void lcd_saver2_draw(uint64_t seconds, int x, int y);

#endif
