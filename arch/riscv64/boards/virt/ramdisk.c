/* ramdisk.c -- virt 用のテストブロックデバイス
 *
 * FAT32 のイメージ (sd.img、make_sd_image.py が生成) を
 * カーネルに埋め込み、blkdev として出す。FAT ドライバを
 * 実機なしで test.sh に載せるための相手役 (doc/sd-design.md)。
 * 実物の SD ドライバ (K210) が来ても、FAT 側は同じコードを
 * そのまま使う
 *
 * 書き込み対応 (doc/sd-write-design.md 段1): 埋め込み
 * イメージは .rodata にあって書き換えられないので、最初の
 * board_blkdev で RAM 上に写しを作り、読み書きは写しに
 * 対して行う。写しは再起動で消える (テスト用デバイスの
 * 割り切り)。写しの上限は 16MiB で、イメージがそれより
 * 大きいときはデバイス自体を出さない
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

#define RAMDISK_CAP (16 * 1024 * 1024)
static unsigned char store[RAMDISK_CAP];
static uint64_t store_size;
static int store_ready;

static int ramdisk_prepare(void)
{
    uint64_t size = (uint64_t)(sd_image_end - sd_image_start);
    uint64_t i;

    if (store_ready) {
        return 0;
    }
    if (size > RAMDISK_CAP) {
        return -1;
    }
    for (i = 0; i < size; i++) {
        store[i] = sd_image_start[i];
    }
    store_size = size;
    store_ready = 1;
    return 0;
}

static int ramdisk_read(struct blkdev *dev, uint64_t lba,
                        unsigned char *buf, uint32_t count)
{
    uint64_t off = lba * 512;
    uint64_t n = (uint64_t)count * 512;
    uint64_t i;

    (void)dev;
    if (ramdisk_prepare() != 0 || off + n > store_size) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        buf[i] = store[off + i];
    }
    return 0;
}

static int ramdisk_write(struct blkdev *dev, uint64_t lba,
                         const unsigned char *buf, uint32_t count)
{
    uint64_t off = lba * 512;
    uint64_t n = (uint64_t)count * 512;
    uint64_t i;

    (void)dev;
    if (ramdisk_prepare() != 0 || off + n > store_size) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        store[off + i] = buf[i];
    }
    return 0;
}

static struct blkdev ramdisk_dev = {
    "ramdisk",
    0,  /* board_blkdev でイメージの大きさから埋める */
    ramdisk_read,
    ramdisk_write,
};

struct blkdev *board_blkdev(void)
{
    if (ramdisk_prepare() != 0) {
        return 0;
    }
    ramdisk_dev.block_count = store_size / 512;
    return &ramdisk_dev;
}
