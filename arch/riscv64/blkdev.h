#ifndef KOSABAOS_BLKDEV_H
#define KOSABAOS_BLKDEV_H

#include <stdint.h>

/* ブロックデバイスの抽象 (doc/sd-design.md)。
 * 共通コード (FAT ドライバ等) はこの口だけを見て、
 * 実物 (SD カード、テスト用の RAM イメージ) はボード側が
 * 用意する。ブロックの大きさは 512 バイト固定 */
struct blkdev {
    const char *name;
    uint64_t block_count;  /* 総ブロック数 */
    /* lba から count ブロックを buf へ読む。成功 0、失敗は負数 */
    int (*read)(struct blkdev *dev, uint64_t lba,
                unsigned char *buf, uint32_t count);
};

/* ボードが持つブロックデバイスを返す (無ければ 0)。
 * virt: RAM イメージ (ramdisk.c)、k210: SD (将来) */
struct blkdev *board_blkdev(void);

#endif
