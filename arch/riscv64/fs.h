#ifndef KOSABAOS_FS_H
#define KOSABAOS_FS_H

#include <stdint.h>

/* RAM ファイルシステムのファイル (fs.c) */
struct fs_file {
    char name[32];
    unsigned char *data;
    uint64_t size;
    uint64_t capacity;  /* arena 内に確保した上限 (書き込み用) */
};

void fs_init(void);                          /* 初期ファイルを用意する */
struct fs_file *fs_find(const char *name);   /* 名前で探す (無ければ 0) */
int fs_add(const char *name, const unsigned char *src, uint64_t size);
void fs_list(void);                          /* 一覧をコンソールに出す */
int fs_count(void);                          /* ファイルの数 */
uint64_t fs_used(void);                      /* arena の使用バイト数 */
struct fs_file *fs_at(int index);            /* 番号で引く (readdir 用) */
struct fs_file *fs_create(const char *name); /* 作成 (同名は空にする) */
uint64_t fs_write_data(struct fs_file *f, const unsigned char *src,
                       uint64_t n);          /* 末尾へ書く */

#endif
