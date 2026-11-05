#ifndef KOSABAOS_K210_SD_HW_H
#define KOSABAOS_K210_SD_HW_H

#include <stdint.h>

/* SD カードのハードウェア層 (K210 段2、doc/sd-design.md)。
 * sd.c (プロトコル) から使う口をここに切り、実物は
 * sd_hw.c (SPI1 + FPIOA + GPIOHS)、ホスト上のモック
 * テストでは差し替え実装が同じ口を提供する */

void sd_hw_init(void);          /* ピン・クロック・SPI1 の初期化 */
void sd_hw_set_fast(void);      /* 初期化完了後にクロックを上げる */
uint8_t sd_xfer(uint8_t out);   /* 1 バイト送受信 */
void sd_cs(int level);          /* CS ピン: 1 = 非選択、0 = 選択 */
void sd_delay_ms(uint32_t ms);  /* 雑な待ち (初期化の間隔用) */

#endif
