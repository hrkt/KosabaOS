#ifndef KOSABAOS_FS_H
#define KOSABAOS_FS_H

#include <stdint.h>

/* パス全体の上限 (バイト、NUL 含まず)。要素ごとの上限は
 * 31 文字で、そちらはパス解決 (trap.c) で見る */
#define FS_NAME_MAX 128

/* RAM ファイルシステムのファイル (fs.c)。
 * データの実体は arena の中のブロックの鎖で持つ
 * (doc/fs-design.md の案A)。読み書きは fs_read_at /
 * fs_write_data 経由で行い、データの生ポインタは外に出さない。
 * 名前は、先頭の '/' を除いた正規化済み絶対パスで持つ
 * (段4: "hello.txt"、"work/memo.txt" の形。ルート直下は
 * そのままファイル名)。ディレクトリも表の項の 1 つで、
 * 親子は名前で結ばれる */
struct fs_file {
    char name[FS_NAME_MAX];
    uint32_t first_block;  /* 鎖の最初のブロック (空なら FS_NO_BLOCK) */
    uint64_t size;
    int used;    /* 表の項が生きているか (段2 の削除対応で導入) */
    int is_dir;  /* ディレクトリか (段4) */
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
int fs_unlink(const char *name);             /* 削除 (無ければ -1) */
int fs_mkdir(const char *name);  /* ディレクトリを作る (失敗 -1) */
int fs_rmdir(const char *name);  /* 空のディレクトリを消す (失敗 -1) */
/* 名前がディレクトリか ("" はルートで、常にディレクトリ) */
int fs_is_dir(const char *name);
/* dir (正規化済み名前、"" = ルート) の index 番目の子を返す。
 * base に子要素の名前だけを書き、is_dir_out に種別を入れる。
 * 子があれば 1、もう無ければ 0 */
int fs_child(const char *dir, int index, char *base, int base_max,
             int *is_dir_out);
uint64_t fs_read_at(struct fs_file *f, uint64_t pos,
                    unsigned char *dst, uint64_t n);  /* 位置を指定して読む */
uint64_t fs_write_data(struct fs_file *f, const unsigned char *src,
                       uint64_t n);          /* 末尾へ書く (育てながら) */

#endif
