/* blkdev.c -- K210 のブロックデバイス
 *
 * 実体は SD カード (sd.c、doc/sd-design.md の段2)。
 * カードの初期化はここの呼び出しまで遅延し、失敗したら
 * 0 を返す (/sd へのアクセスが「デバイス無し」で失敗
 * するだけで、起動や他の機能には影響しない)
 */

#include "blkdev.h"
#include "sd.h"

struct blkdev *board_blkdev(void)
{
    return sd_blkdev();
}
