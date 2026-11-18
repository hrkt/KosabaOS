/* i2c.c (virt) -- I2C のダミー (ボード層)
 *
 * virt には I2C デバイスが無い。共通層 (ssd1315.c) の検証が
 * virt でも通るよう、送信は常に成功として扱い、記録は
 * 共通層側の klog に任せる (gfx / audio の virt ダミーと
 * 同じ流儀)。設計は doc/oled-design.md。
 */

#include <stdint.h>
#include "board.h"
#include "i2c.h"

int board_i2c_write(uint8_t addr, const uint8_t *buf, uint32_t len)
{
    (void)addr;
    (void)buf;
    (void)len;
    return 0;
}
