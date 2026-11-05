#ifndef KOSABAOS_K210_SD_H
#define KOSABAOS_K210_SD_H

#include "blkdev.h"

/* SD カード (sd.c)。初期化に成功したら blkdev を返し、
 * カードが無い・応答しない等の失敗なら 0 を返す */
struct blkdev *sd_blkdev(void);

#endif
