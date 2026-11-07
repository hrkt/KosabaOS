/* fat.c -- FAT32 読み取り専用ドライバ (doc/sd-design.md)
 *
 * ボリュームの構造 (BPB から読む):
 *   [予約領域][FAT × 個数][データ領域 (クラスタの並び)]
 * ファイルもディレクトリも「クラスタの鎖」でできていて、
 * FAT が各クラスタの「次」を持つ。RAM FS のブロック
 * チェーン (fs.c) と同じ考え方を、ディスクの形式のまま
 * たどるのがこのドライバの仕事。
 *
 * 割り切り (設計どおり): FAT32 のみ、名前は 8.3 形式のみ
 * (LFN は読み飛ばす)、同時に相手にするのは 1 ボリューム。
 * マウントは遅延 (最初の利用時)。
 *
 * 書き込み対応 (doc/sd-write-design.md): 段1 で blkdev の
 * write と FAT キャッシュの書き戻し (dirty 管理) と FSInfo
 * の読みを用意し、段2 で確保・作成・削除の本体を足した。
 * 書き込みの順序は「データ → FAT → ディレクトリ (サイズは
 * close 時)」で、FAT の変更はキャッシュに溜めてから
 * 全面へ書き戻す。
 *
 * 読みは位置指定 (fat_read_at) に統一し、開いたファイルの
 * 状態は {先頭クラスタ, サイズ} だけ。キャッシュは FAT 用
 * 1 セクタと部分読み用のバウンス 1 セクタだけ。
 */

#include <stdint.h>
#include "fat.h"
#include "blkdev.h"
#include "klog.h"

#define SECTOR 512

static struct blkdev *dev = 0;
static int mounted = 0;
static uint64_t fat_start;   /* FAT 先頭の LBA (絶対) */
static uint64_t data_start;  /* データ領域先頭の LBA (絶対) */
static uint32_t root_cluster;
static uint32_t sec_per_cluster;
static uint64_t total_clusters;
static uint32_t num_fats;         /* FAT の面数 (通常 2) */
static uint32_t fat_sectors;      /* FAT 1 面のセクタ数 */
static uint64_t fsinfo_lba;       /* FSInfo の LBA (絶対)。無ければ 0 */
static uint64_t free_clusters;    /* 空きクラスタ数 (FSInfo 由来。
                                   * 不明なら 0xFFFFFFFFFFFFFFFF) */
static uint32_t alloc_hint;       /* 空き探索の開始クラスタ */

static unsigned char fat_cache[SECTOR];
static uint64_t fat_cache_lba = 0xFFFFFFFFFFFFFFFFULL;
static int fat_cache_dirty;  /* キャッシュに未書き戻しの変更がある */
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

static int write_sector(uint64_t lba, const unsigned char *buf)
{
    if (dev->write == 0) {
        return -1;  /* 書き込みに対応しないデバイス */
    }
    return dev->write(dev, lba, buf, 1);
}

/* 書きかけの FAT セクタを全ての面へ書き戻す。FAT は複数面が
 * 同じ内容でなければならないので、キャッシュの 1 セクタは
 * 全面に同じ形で出す。成功したら dirty を落とす */
static int fat_cache_flush(void)
{
    uint64_t off;
    uint32_t i;

    if (!fat_cache_dirty) {
        return 0;
    }
    off = fat_cache_lba - fat_start;  /* FAT 先頭からのセクタ数 */
    for (i = 0; i < num_fats; i++) {
        if (write_sector(fat_start + (uint64_t)i * fat_sectors + off,
                         fat_cache) != 0) {
            return -1;  /* dirty は残し、呼び出し側に失敗を返す */
        }
    }
    fat_cache_dirty = 0;
    return 0;
}

/* 公開口: 溜まった FAT の変更を全部書き戻す
 * (doc/sd-write-design.md 段1。段2 の close 等が使う) */
int fat_sync(void)
{
    return fat_cache_flush();
}

/* キャッシュに lba の FAT セクタを載せる。載せ替えの前に
 * 書きかけがあれば書き戻す。失敗は -1 */
static int fat_cache_ensure(uint64_t lba)
{
    if (lba == fat_cache_lba) {
        return 0;
    }
    if (fat_cache_flush() != 0) {
        return -1;
    }
    if (read_sector(lba, fat_cache) != 0) {
        fat_cache_lba = 0xFFFFFFFFFFFFFFFFULL;
        return -1;
    }
    fat_cache_lba = lba;
    return 0;
}

/* クラスタ番号 → データ領域の先頭 LBA */
static uint64_t cluster_lba(uint32_t cluster)
{
    return data_start + (uint64_t)(cluster - 2) * sec_per_cluster;
}

/* FAT の 1 項を読む (読めなければ鎖の終わりとして扱う) */
static uint32_t fat_entry_get(uint32_t cluster)
{
    uint64_t off = (uint64_t)cluster * 4;
    uint64_t lba = fat_start + off / SECTOR;

    if (fat_cache_ensure(lba) != 0) {
        return 0x0FFFFFF8;
    }
    return rd32(fat_cache + (off % SECTOR)) & 0x0FFFFFFF;
}

/* FAT の 1 項を書く。実デバイスへの反映はキャッシュの
 * 書き戻し (fat_cache_flush) まで遅らせる。最上位 4 ビットは
 * 予約分として元の値を保つ */
static int fat_entry_set(uint32_t cluster, uint32_t value)
{
    uint64_t off = (uint64_t)cluster * 4;
    uint64_t lba = fat_start + off / SECTOR;
    unsigned char *p;
    uint32_t v;

    if (fat_cache_ensure(lba) != 0) {
        return -1;
    }
    p = fat_cache + (off % SECTOR);
    v = (rd32(p) & 0xF0000000u) | (value & 0x0FFFFFFFu);
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
    fat_cache_dirty = 1;
    return 0;
}

/* FAT の鎖で「次」を引く (終わりは 0x0FFFFFF8 以上) */
static uint32_t fat_next(uint32_t cluster)
{
    return fat_entry_get(cluster);
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
    num_fats = bounce[16];
    fat_sectors = fatsz32;
    data_start = fat_start + (uint64_t)num_fats * fatsz32;
    total_clusters =
        (total32 - (data_start - bpb_lba)) / sec_per_cluster;

    /* FSInfo (空きクラスタ数と探索ヒント)。署名が正しい
     * ときだけ信用する。壊れている・無いボリュームでは
     * 「不明」のままにし、書き込み側も FSInfo を触らない */
    fsinfo_lba = 0;
    free_clusters = 0xFFFFFFFFFFFFFFFFULL;
    alloc_hint = 2;
    {
        uint32_t fsinfo_sec = rd16(bounce + 48);
        if (fsinfo_sec != 0 &&
            read_sector(bpb_lba + fsinfo_sec, bounce) == 0 &&
            rd32(bounce + 0) == 0x41615252u &&
            rd32(bounce + 484) == 0x61417272u &&
            rd32(bounce + 508) == 0xAA550000u) {
            uint32_t free_n = rd32(bounce + 488);
            uint32_t next_free = rd32(bounce + 492);
            fsinfo_lba = bpb_lba + fsinfo_sec;
            if (free_n != 0xFFFFFFFFu &&
                (uint64_t)free_n <= total_clusters) {
                free_clusters = free_n;
            }
            if (next_free >= 2 &&
                (uint64_t)next_free <= total_clusters + 1) {
                alloc_hint = next_free;
            }
        }
    }
    mounted = 1;
    /* マウントは遅延 (最初の /sd アクセス時) なので、起きた
     * ことをログに残す (コンソールにも出る) */
    kputs("[fat] /sd mounted\n");
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
