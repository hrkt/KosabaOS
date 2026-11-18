/* i2c.h -- I2C マスタ送信の共通インタフェース
 *
 * 実体はボード層 (boards/<board>/i2c.c)。共通層はこの口だけを
 * 見る (RTC の board_rtc_get と同じ受け方、time.h 参照)。
 * 設計は doc/oled-design.md。
 */
#ifndef KOSABAOS_I2C_H
#define KOSABAOS_I2C_H

#include <stdint.h>

/* addr へ buf を 1 トランザクションで送信する。
 * 戻り値: 0 = 成功、-1 = ACK が無い (デバイス不在) 等 */
int board_i2c_write(uint8_t addr, const uint8_t *buf, uint32_t len);

#endif
