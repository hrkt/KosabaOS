#ifndef KOSABAOS_FS_H
#define KOSABAOS_FS_H

#include <stdint.h>

/* RAM ファイルシステムのファイル (fs.c)。
 * データの実体は arena の中のブロックの鎖で持つ
 * (doc/fs-design.md の案A)。読み書きは fs_read_at /
 * fs_write_data 経由で行い、データの生ポインタは外に出さない */
struct fs_file {
    char name[32];
    uint32_t first_block;  /* 鎖の最初のブロック (空なら FS_NO_BLOCK) */
    uint64_t size;
};

#define FS_NO_BLOCK 0xFFFFFFFFu

void fs_init(void);                          /* 初期ファイルを用意する */
struct fs_file *fs_find(const char *name);   /* 名前で探す (無ければ 0) */
int fs_add(const char *name, const unsigned char *src, uint64_t size);
void fs_list(void);                          /* 一覧をコンソールに出す */
int fs_count(void);                          /* ファイルの数 */
uint64_t fs_used(void);                      /* arena の使用バイト数 */
struct fs_file *fs_at(int index);            /* 番号で引く (readdir 用) */
struct fs_file *fs_create(const char *name); /* 作成 (同名は空にする) */
uint64_t fs_read_at(struct fs_file *f, uint64_t pos,
                    unsigned char *dst, uint64_t n);  /* 位置を指定して読む */
uint64_t fs_write_data(struct fs_file *f, const unsigned char *src,
                       uint64_t n);          /* 末尾へ書く (育てながら) */

#endif
