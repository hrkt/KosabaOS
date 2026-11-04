/* blkdev.c -- K210 のブロックデバイスの置き場所
 *
 * SD ドライバ (doc/sd-roadmap.md の段1 スパイク → 段2) が
 * 実装されたら、ここで SD の blkdev を返す。今はまだ無い
 * ので 0 を返し、/sd へのアクセスは「デバイス無し」で
 * 失敗する (起動や他の機能には影響しない)
 */

#include "blkdev.h"

struct blkdev *board_blkdev(void)
{
    return 0;
}
