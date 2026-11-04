#ifndef KOSABAOS_FAT_H
#define KOSABAOS_FAT_H

#include <stdint.h>

struct blkdev;

/* FAT32 読み取り専用ドライバ (doc/sd-design.md)。
 * fs.c と対称に、ファイルは {先頭クラスタ, サイズ} だけを
 * 持ち、読みは位置指定 (fat_read_at)。パスは mount 点を
 * 外した、ボリュームのルートからの相対パスで受ける
 * (例: "MUSIC/SONG.MP3") */
struct fat_file {
    uint32_t first_cluster;
    uint64_t size;
};

/* デバイスを掴んでマウントする (最初の /sd アクセス用)。
 * 既にマウント済みなら何もしない。成功 0、失敗 -1 */
int fat_ensure_mounted(void);

/* パスでファイルを探す。成功 0、無ければ -1 */
int fat_find(const char *path, struct fat_file *out);

/* 位置を指定して読む (末尾で止まる。fs_read_at と同じ約束) */
uint64_t fat_read_at(struct fat_file *f, uint64_t pos,
                     unsigned char *dst, uint64_t n);

/* ディレクトリ (パス、"" はルート) の index 番目の項目。
 * name に 8.3 形式をドット付き文字列で返す (13 バイト以上)。
 * 項目があれば 1、もう無ければ 0、失敗は -1 */
int fat_list(const char *dir_path, int index, char *name,
             uint64_t *size_out, int *is_dir);

#endif
