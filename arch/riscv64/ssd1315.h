/* ssd1315.h -- SSD1315 OLED (128x64, I2C) の共通インタフェース
 * 設計は doc/oled-design.md。輸送は i2c.h の board_i2c_write。
 */
#ifndef KOSABAOS_SSD1315_H
#define KOSABAOS_SSD1315_H

/* kmain から呼ぶ: 初期化して起動表示を出す。
 * OLED が無くても失敗として扱わない (klog に 1 行残るだけ) */
void oled_bootstrap(void);

/* syscall 48 (oled_text): 消去してから文字列を描いて転送する。
 * 単語 "|" は改行の合図。戻り値: 0 / -1 (OLED が無い) */
int oled_text_multi(const char *s);

/* syscall 49 (oled_clear): 消去して転送する。0 / -1 */
int oled_clear_flush(void);

#endif
