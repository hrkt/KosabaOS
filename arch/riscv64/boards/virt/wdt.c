/* wdt.c (virt) -- ウォッチドッグのダミー (ボード層)
 *
 * virt にはウォッチドッグを付けない (QEMU の終了は
 * テスト側の都合でよい)。共通層 (kernel.c / proc.c) が
 * 無条件に呼べるよう、空の実装を置く。設計は
 * doc/trap-recovery-design.md の段3。
 */

#include "wdt.h"

void board_wdt_start(void)
{
}

void board_wdt_feed(void)
{
}
