/* ramdisk.c -- virt 用のテストブロックデバイス
 *
 * FAT32 のイメージ (sd.img、make_sd_image.py が生成) を
 * カーネルに埋め込み、blkdev として出す。FAT ドライバを
 * 実機なしで test.sh に載せるための相手役 (doc/sd-design.md)。
 * 実物の SD ドライバ (K210) が来ても、FAT 側は同じコードを
 * そのまま使う
 */

#include <stdint.h>
#include "blkdev.h"

extern unsigned char sd_image_start[];
extern unsigned char sd_image_end[];

__asm__(
    ".section .rodata\n"
    ".globl sd_image_start\n"
    "sd_image_start:\n"
    ".incbin \"sd.img\"\n"
    ".globl sd_image_end\n"
    "sd_image_end:\n"
    ".text\n");

static int ramdisk_read(struct blkdev *dev, uint64_t lba,
                        unsigned char *buf, uint32_t count)
{
    uint64_t size = (uint64_t)(sd_image_end - sd_image_start);
    uint64_t off = lba * 512;
    uint64_t n = (uint64_t)count * 512;
    uint64_t i;

    (void)dev;
    if (off + n > size) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        buf[i] = sd_image_start[off + i];
    }
    return 0;
}

static struct blkdev ramdisk_dev = {
    "ramdisk",
    0,  /* board_blkdev でイメージの大きさから埋める */
    ramdisk_read,
};

struct blkdev *board_blkdev(void)
{
    ramdisk_dev.block_count =
        (uint64_t)(sd_image_end - sd_image_start) / 512;
    return &ramdisk_dev;
}
