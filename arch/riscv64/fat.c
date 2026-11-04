/* fat.c -- FAT32 読み取り専用ドライバ (doc/sd-design.md)
 *
 * ボリュームの構造 (BPB から読む):
 *   [予約領域][FAT × 個数][データ領域 (クラスタの並び)]
 * ファイルもディレクトリも「クラスタの鎖」でできていて、
 * FAT が各クラスタの「次」を持つ。RAM FS のブロック
 * チェーン (fs.c) と同じ考え方を、ディスクの形式のまま
 * たどるのがこのドライバの仕事。
 *
 * 割り切り (設計どおり): FAT32 のみ、読み取り専用、
 * 名前は 8.3 形式のみ (LFN は読み飛ばす)、同時に相手に
 * するのは 1 ボリューム。マウントは遅延 (最初の利用時)。
 *
 * 読みは位置指定 (fat_read_at) に統一し、開いたファイルの
 * 状態は {先頭クラスタ, サイズ} だけ。キャッシュは FAT 用
 * 1 セクタと部分読み用のバウンス 1 セクタだけ。
 */

#include <stdint.h>
#include "fat.h"
#include "blkdev.h"

#define SECTOR 512

static struct blkdev *dev = 0;
static int mounted = 0;
static uint64_t fat_start;   /* FAT 先頭の LBA (絶対) */
static uint64_t data_start;  /* データ領域先頭の LBA (絶対) */
static uint32_t root_cluster;
static uint32_t sec_per_cluster;
static uint64_t total_clusters;

static unsigned char fat_cache[SECTOR];
static uint64_t fat_cache_lba = 0xFFFFFFFFFFFFFFFFULL;
static unsigned char bounce[SECTOR];

/* --- 小物 ------------------------------------------------------------ */

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_sector(uint64_t lba, unsigned char *buf)
{
    return dev->read(dev, lba, buf, 1);
}

/* クラスタ番号 → データ領域の先頭 LBA */
static uint64_t cluster_lba(uint32_t cluster)
{
    return data_start + (uint64_t)(cluster - 2) * sec_per_cluster;
}

/* FAT の鎖で「次」を引く (終わりは 0x0FFFFFF8 以上) */
static uint32_t fat_next(uint32_t cluster)
{
    uint64_t off = (uint64_t)cluster * 4;
    uint64_t lba = fat_start + off / SECTOR;

    if (lba != fat_cache_lba) {
        if (read_sector(lba, fat_cache) != 0) {
            return 0x0FFFFFF8;  /* 読めなければ終わりとして扱う */
        }
        fat_cache_lba = lba;
    }
    return rd32(fat_cache + (off % SECTOR)) & 0x0FFFFFFF;
}

/* 鎖をたどって pos から n バイト読む。limit はファイルの
 * サイズ (ディレクトリは「鎖の終わりまで」として大きな値を
 * 渡す)。読みはセクタ単位で、端数だけバウンスを経由する */
static uint64_t chain_read(uint32_t first_cluster, uint64_t pos,
                           unsigned char *dst, uint64_t n,
                           uint64_t limit)
{
    uint64_t cluster_bytes = (uint64_t)sec_per_cluster * SECTOR;
    uint64_t done = 0;
    uint32_t cluster = first_cluster;
    uint64_t idx, guard = total_clusters + 2;

    if (pos >= limit) {
        return 0;
    }
    if (n > limit - pos) {
        n = limit - pos;
    }
    /* pos が属するクラスタまで鎖を進める */
    idx = pos / cluster_bytes;
    while (idx > 0) {
        if (cluster < 2 || cluster >= 0x0FFFFFF8 || guard-- == 0) {
            return 0;
        }
        cluster = fat_next(cluster);
        idx--;
    }
    while (done < n) {
        uint64_t in_cluster = (pos + done) % cluster_bytes;
        uint64_t lba, in_sector, chunk, j;

        if (cluster < 2 || cluster >= 0x0FFFFFF8) {
            break;  /* 鎖の終わり */
        }
        lba = cluster_lba(cluster) + in_cluster / SECTOR;
        in_sector = in_cluster % SECTOR;
        chunk = SECTOR - in_sector;
        if (chunk > n - done) {
            chunk = n - done;
        }
        if (chunk > cluster_bytes - in_cluster) {
            chunk = cluster_bytes - in_cluster;
        }
        if (in_sector == 0 && chunk == SECTOR) {
            /* セクタ丸ごとは直接読む */
            if (read_sector(lba, dst + done) != 0) {
                break;
            }
        } else {
            if (read_sector(lba, bounce) != 0) {
                break;
            }
            for (j = 0; j < chunk; j++) {
                dst[done + j] = bounce[in_sector + j];
            }
        }
        done += chunk;
        if ((pos + done) % cluster_bytes == 0 && done < n) {
            cluster = fat_next(cluster);
            if (guard-- == 0) {
                break;  /* 鎖が循環していたら諦める */
            }
        }
    }
    return done;
}

/* --- マウント -------------------------------------------------------- */

static int fat_mount(struct blkdev *d)
{
    uint64_t bpb_lba;
    uint32_t total32, fatsz32, reserved;

    if (read_sector(0, bounce) != 0) {
        return -1;
    }
    if (bounce[82] == 'F' && bounce[83] == 'A' && bounce[84] == 'T') {
        bpb_lba = 0;  /* スーパーフロッピー形式 (BPB が先頭) */
    } else {
        /* MBR のパーティション表エントリ 1 を見る */
        uint8_t type;
        if (bounce[510] != 0x55 || bounce[511] != 0xAA) {
            return -1;
        }
        type = bounce[446 + 4];
        if (type != 0x0B && type != 0x0C) {
            return -1;  /* FAT32 のパーティションではない */
        }
        bpb_lba = rd32(bounce + 446 + 8);
        if (read_sector(bpb_lba, bounce) != 0) {
            return -1;
        }
    }
    if (rd16(bounce + 11) != SECTOR) {
        return -1;  /* セクタ 512 前提 */
    }
    sec_per_cluster = bounce[13];
    if (sec_per_cluster == 0 ||
        (sec_per_cluster & (sec_per_cluster - 1)) != 0) {
        return -1;  /* 2 の冪でなければおかしい */
    }
    reserved = rd16(bounce + 14);
    if (bounce[16] < 1) {
        return -1;  /* FAT が最低 1 つ */
    }
    fatsz32 = rd32(bounce + 36);
    root_cluster = rd32(bounce + 44);
    total32 = rd32(bounce + 32);
    if (fatsz32 == 0 || root_cluster < 2 || total32 == 0) {
        return -1;  /* FAT32 の形をしていない */
    }
    fat_start = bpb_lba + reserved;
    data_start = fat_start + (uint64_t)bounce[16] * fatsz32;
    total_clusters =
        (total32 - (data_start - bpb_lba)) / sec_per_cluster;
    mounted = 1;
    return 0;
}

int fat_ensure_mounted(void)
{
    if (mounted) {
        return 0;
    }
    dev = board_blkdev();
    if (dev == 0) {
        return -1;  /* ブロックデバイスが無い (カード未対応等) */
    }
    return fat_mount(dev);
}

/* --- ディレクトリ ---------------------------------------------------- */

struct dir_entry {
    uint32_t first_cluster;
    uint64_t size;
    int is_dir;
};

/* パスから要素を 1 つ取り出す。区切り '/' と '.' 要素は
 * 読み飛ばす。要素があれば 1 (out は NUL 終端)、無ければ 0 */
static int next_component(const char **cursor, char *out, int max)
{
    const char *p = *cursor;
    int n = 0;

    for (;;) {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            *cursor = p;
            return 0;
        }
        n = 0;
        while (*p != '\0' && *p != '/') {
            if (n < max - 1) {
                out[n++] = *p;
            }
            p++;
        }
        out[n] = '\0';
        *cursor = p;
        if (!(n == 1 && out[0] == '.')) {
            return 1;  /* '.' 要素は飛ばして次へ */
        }
    }
}

/* 要素名を 8.3 の 11 バイト (空白詰め、大文字) にする。
 * 形が合わなければ -1 */
static int to_83(const char *comp, char out[11])
{
    int i = 0, j;

    for (j = 0; j < 11; j++) {
        out[j] = ' ';
    }
    j = 0;
    while (comp[i] != '\0' && comp[i] != '.') {
        char c = comp[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (j >= 8) {
            return -1;
        }
        out[j++] = c;
        i++;
    }
    if (j == 0) {
        return -1;
    }
    if (comp[i] == '.') {
        i++;
        j = 8;
        while (comp[i] != '\0') {
            char c = comp[i];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            if (j >= 11) {
                return -1;
            }
            out[j++] = c;
            i++;
        }
    }
    return 0;
}

/* ディレクトリの鎖から、8.3 名が一致する項目を探す */
static int find_in_dir(uint32_t dir_cluster, const char name83[11],
                       struct dir_entry *out)
{
    unsigned char e[32];
    uint64_t pos = 0;

    for (;;) {
        int i, same = 1;
        if (chain_read(dir_cluster, pos, e, 32,
                       0xFFFFFFFFFFFFFFFFULL) < 32) {
            return -1;  /* 鎖の終わりまで無かった */
        }
        pos += 32;
        if (e[0] == 0x00) {
            return -1;  /* ここから先は未使用 */
        }
        if (e[0] == 0xE5) {
            continue;  /* 削除済み */
        }
        if (e[11] == 0x0F || (e[11] & 0x08)) {
            continue;  /* LFN とボリュームラベルは相手にしない */
        }
        for (i = 0; i < 11; i++) {
            if (e[i] != (unsigned char)name83[i]) {
                same = 0;
                break;
            }
        }
        if (!same) {
            continue;
        }
        out->first_cluster =
            ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
        out->size = rd32(e + 28);
        out->is_dir = (e[11] & 0x10) != 0;
        return 0;
    }
}

/* パスのディレクトリ部分をたどって、対象ディレクトリの
 * 先頭クラスタを返す。path が "" ならルート。
 * ファイルを指すパスに対しては使えない */
static int resolve_dir(const char *path, uint32_t *dir_cluster)
{
    uint32_t cur = root_cluster;
    const char *cursor = path;
    char comp[16];
    int depth = 0;

    while (next_component(&cursor, comp, sizeof(comp))) {
        char name83[11];
        struct dir_entry e;
        if (++depth > 8) {
            return -1;  /* 深さ上限 (設計どおり) */
        }
        if (to_83(comp, name83) != 0 ||
            find_in_dir(cur, name83, &e) != 0 || !e.is_dir) {
            return -1;
        }
        cur = e.first_cluster;
    }
    *dir_cluster = cur;
    return 0;
}

int fat_find(const char *path, struct fat_file *out)
{
    /* パスを要素に割って持ち、最後の要素だけをファイル
     * として探し、それより前はディレクトリとしてたどる */
    char buf[128];
    char *comps[10];
    int ncomp = 0, i, n = 0;
    uint32_t cur = root_cluster;

    if (fat_ensure_mounted() != 0) {
        return -1;
    }
    while (path[n] != '\0' && n < (int)sizeof(buf) - 1) {
        buf[n] = path[n];
        n++;
    }
    buf[n] = '\0';
    i = 0;
    while (buf[i] != '\0' && ncomp < 10) {
        while (buf[i] == '/') {
            i++;
        }
        if (buf[i] == '\0') {
            break;
        }
        comps[ncomp++] = &buf[i];
        while (buf[i] != '\0' && buf[i] != '/') {
            i++;
        }
        if (buf[i] == '/') {
            buf[i] = '\0';
            i++;
        }
    }
    /* '.' 要素を詰める (next_component と同じ扱い) */
    {
        int r, w = 0;
        for (r = 0; r < ncomp; r++) {
            if (!(comps[r][0] == '.' && comps[r][1] == '\0')) {
                comps[w++] = comps[r];
            }
        }
        ncomp = w;
    }
    if (ncomp == 0) {
        return -1;  /* ルート自体はファイルではない */
    }
    for (i = 0; i < ncomp; i++) {
        char name83[11];
        struct dir_entry e;
        if (to_83(comps[i], name83) != 0 ||
            find_in_dir(cur, name83, &e) != 0) {
            return -1;
        }
        if (i == ncomp - 1) {
            if (e.is_dir) {
                return -1;  /* ディレクトリは開けない */
            }
            out->first_cluster = e.first_cluster;
            out->size = e.size;
            return 0;
        }
        if (!e.is_dir) {
            return -1;  /* 途中がファイルだった */
        }
        cur = e.first_cluster;
    }
    return -1;
}

uint64_t fat_read_at(struct fat_file *f, uint64_t pos,
                     unsigned char *dst, uint64_t n)
{
    return chain_read(f->first_cluster, pos, dst, n, f->size);
}

int fat_list(const char *dir_path, int index, char *name,
             uint64_t *size_out, int *is_dir)
{
    uint32_t dir_cluster;
    unsigned char e[32];
    uint64_t pos = 0;
    int seen = 0;

    if (fat_ensure_mounted() != 0) {
        return -1;
    }
    if (resolve_dir(dir_path, &dir_cluster) != 0) {
        return -1;
    }
    for (;;) {
        int i, n = 0;
        if (chain_read(dir_cluster, pos, e, 32,
                       0xFFFFFFFFFFFFFFFFULL) < 32) {
            return 0;
        }
        pos += 32;
        if (e[0] == 0x00) {
            return 0;
        }
        if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) {
            continue;
        }
        if (e[0] == '.') {
            continue;  /* '.' と '..' は一覧に出さない */
        }
        if (seen++ != index) {
            continue;
        }
        /* 8.3 名を "NAME.EXT" の文字列に戻す */
        for (i = 0; i < 8 && e[i] != ' '; i++) {
            name[n++] = (char)e[i];
        }
        if (e[8] != ' ') {
            name[n++] = '.';
            for (i = 8; i < 11 && e[i] != ' '; i++) {
                name[n++] = (char)e[i];
            }
        }
        name[n] = '\0';
        *size_out = rd32(e + 28);
        *is_dir = (e[11] & 0x10) != 0;
        return 1;
    }
}
