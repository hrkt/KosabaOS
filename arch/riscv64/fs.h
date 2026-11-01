#ifndef KOSABAOS_FS_H
#define KOSABAOS_FS_H

#include <stdint.h>

/* RAM ファイルシステムのファイル (fs.c) */
struct fs_file {
    char name[32];
    unsigned char *data;
    uint64_t size;
};

void fs_init(void);                          /* 初期ファイルを用意する */
struct fs_file *fs_find(const char *name);   /* 名前で探す (無ければ 0) */
int fs_add(const char *name, const unsigned char *src, uint64_t size);
void fs_list(void);                          /* 一覧をコンソールに出す */
int fs_count(void);                          /* ファイルの数 */
struct fs_file *fs_at(int index);            /* 番号で引く (readdir 用) */

#endif
