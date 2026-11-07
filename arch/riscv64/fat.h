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
    /* 書き込み用 (doc/sd-write-design.md 段2)。目録項の位置は
     * 読み取りで開いたときも埋まる (unlink の照合に使う)。
     * tail / capacity は書き込みの続きを保つためのもので、
     * 作成 (fat_create) からの一連の書き込みでのみ使う */
    uint32_t dir_cluster;   /* 目録項のあるディレクトリの先頭クラスタ */
    uint64_t dir_off;       /* その中での目録項のバイト位置 */
    uint32_t tail_cluster;  /* 鎖の最後のクラスタ */
    uint64_t capacity;      /* 鎖が収めるバイト数 */
    int has_dir;            /* 目録項の位置が有効か */
    int dirty;              /* close 時の目録書き戻しが要るか */
};

/* デバイスを掴んでマウントする (最初の /sd アクセス用)。
 * 既にマウント済みなら何もしない。成功 0、失敗 -1 */
int fat_ensure_mounted(void);

/* 溜まった FAT の変更を全ての面へ書き戻す (書き込み対応
 * 段1、doc/sd-write-design.md)。成功 0、失敗 -1 */
int fat_sync(void);

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

/* --- 書き込み (doc/sd-write-design.md 段2) ------------------ */

/* ファイルを作る。同名のファイルがあれば空に切り捨てる
 * (RAM FS の fs_create と同じ意味づけ)。対象はルートと
 * 既存ディレクトリの中、名前は 8.3 のみ。成功 0、失敗 -1。
 * 目録項はこの時点で作り (サイズ 0)、サイズと開始クラスタは
 * fat_close_write で書き戻す */
int fat_create(const char *path, struct fat_file *out);

/* 末尾へ書く (fs_write_data と同じ意味づけ)。鎖を育てながら
 * データを先に書き、FAT のリンクはキャッシュに溜める。
 * 書けたバイト数を返す */
uint64_t fat_write_data(struct fat_file *f,
                        const unsigned char *src, uint64_t n);

/* 書き込みの締め: FAT を書き戻し、目録項へサイズと開始
 * クラスタと日時を書き、FSInfo の空き数を更新する。
 * 成功 0、失敗 -1 */
int fat_close_write(struct fat_file *f);

/* ファイルを消す (目録項を削除済みにし、鎖を返す)。
 * 成功 0、失敗 -1 */
int fat_unlink(const char *path);

/* ディレクトリを作る (中身は "." と ".." だけ)。
 * 成功 0、失敗 -1 */
int fat_mkdir(const char *path);

/* 空のディレクトリを消す ("." と ".." 以外の項があれば
 * 失敗)。項の無効化 → 鎖の解放 → FSInfo の順は
 * fat_unlink と同じ。成功 0、失敗 -1 */
int fat_rmdir(const char *path);

#endif
