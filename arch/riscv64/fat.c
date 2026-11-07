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
#include "time.h"

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

static void wr16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* 壁時計を FAT の日時形式にする (doc/sd-write-design.md の
 * 決め事4)。RTC 未設定などで読めなければ 0 (1980-01-01) */
static void fat_now(uint16_t *date, uint16_t *tim)
{
    struct datetime dt;

    if (time_read(&dt) != 0 || dt.year < 1980 || dt.year > 2107) {
        *date = 0;
        *tim = 0;
        return;
    }
    *date = (uint16_t)(((uint32_t)(dt.year - 1980) << 9) |
                       ((uint32_t)dt.month << 5) | (uint32_t)dt.day);
    *tim = (uint16_t)(((uint32_t)dt.hour << 11) |
                      ((uint32_t)dt.minute << 5) |
                      ((uint32_t)dt.second >> 1));
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

/* ディレクトリの鎖から、8.3 名が一致する項目を探す。
 * off_out があれば、見つけた項のバイト位置も返す
 * (書き込み側がその項を書き戻すために使う) */
static int find_in_dir2(uint32_t dir_cluster, const char name83[11],
                        struct dir_entry *out, uint64_t *off_out)
{
    unsigned char e[32];
    uint64_t pos = 0;

    for (;;) {
        int i, same = 1;
        uint64_t cur = pos;
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
        if (off_out != 0) {
            *off_out = cur;
        }
        return 0;
    }
}

static int find_in_dir(uint32_t dir_cluster, const char name83[11],
                       struct dir_entry *out)
{
    return find_in_dir2(dir_cluster, name83, out, 0);
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
    uint32_t cur;

    if (fat_ensure_mounted() != 0) {
        return -1;
    }
    /* cur の初期値はマウントの後で読む。マウント前は
     * root_cluster がまだ 0 で、最初の呼び出しが fat_find
     * だと根を外す (trap.c 経由では先にマウントされるため
     * 隠れていた。ホスト検証で発見、2026-11-07) */
    cur = root_cluster;
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
        uint64_t off = 0;
        if (to_83(comps[i], name83) != 0 ||
            find_in_dir2(cur, name83, &e, &off) != 0) {
            return -1;
        }
        if (i == ncomp - 1) {
            if (e.is_dir) {
                return -1;  /* ディレクトリは開けない */
            }
            out->first_cluster = e.first_cluster;
            out->size = e.size;
            /* 目録項の位置も記録する (書き込み側と unlink の
             * 照合用。読み取り専用の利用では使わない) */
            out->dir_cluster = cur;
            out->dir_off = off;
            out->tail_cluster = 0;
            out->capacity = 0;
            out->has_dir = 1;
            out->dirty = 0;
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

/* --- 書き込み (doc/sd-write-design.md 段2) ----------------------------
 *
 * 書き込みの順序 (設計の決め事2): データを先に書き、FAT の
 * リンクはキャッシュに溜め、close で「FAT → 目録項 (サイズ
 * 最後)」の順に確定させる。電源断の被害は書いていた
 * ファイルに寄る形にし、ジャーナルは持たない (割り切り)。 */

/* 鎖へ pos から n バイト書く (鎖の容量の範囲内が前提)。
 * 書けたバイト数を返す。部分セクタは読み戻してから直す
 * (chain_read の鏡像) */
static uint64_t chain_write_bytes(uint32_t first_cluster, uint64_t pos,
                                  const unsigned char *src, uint64_t n)
{
    uint64_t cluster_bytes = (uint64_t)sec_per_cluster * SECTOR;
    uint64_t done = 0;
    uint32_t cluster = first_cluster;
    uint64_t idx, guard = total_clusters + 2;

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
            if (write_sector(lba, src + done) != 0) {
                break;
            }
        } else {
            if (read_sector(lba, bounce) != 0) {
                break;
            }
            for (j = 0; j < chunk; j++) {
                bounce[in_sector + j] = src[done + j];
            }
            if (write_sector(lba, bounce) != 0) {
                break;
            }
        }
        done += chunk;
        if ((pos + done) % cluster_bytes == 0 && done < n) {
            cluster = fat_next(cluster);
            if (guard-- == 0) {
                break;
            }
        }
    }
    return done;
}

/* 目録項 1 つ (32 バイト) の読み書き */
static int entry_read(uint32_t dir_cluster, uint64_t off,
                      unsigned char e[32])
{
    return chain_read(dir_cluster, off, e, 32,
                      0xFFFFFFFFFFFFFFFFULL) == 32 ? 0 : -1;
}

static int entry_write(uint32_t dir_cluster, uint64_t off,
                       const unsigned char e[32])
{
    return chain_write_bytes(dir_cluster, off, e, 32) == 32 ? 0 : -1;
}

/* 鎖の最後のクラスタ (と、数えればクラスタ数) */
static uint32_t chain_last(uint32_t first, uint64_t *count_out)
{
    uint32_t c = first, last = first;
    uint64_t n = 0, guard = total_clusters + 2;

    while (c >= 2 && c < 0x0FFFFFF8 && guard-- > 0) {
        last = c;
        n++;
        c = fat_next(c);
    }
    if (count_out != 0) {
        *count_out = n;
    }
    return last;
}

/* 空きクラスタを [from, to) から探す。無ければ 0 */
static uint32_t scan_free(uint32_t from, uint32_t to)
{
    uint32_t c;
    for (c = from; c < to; c++) {
        if (fat_entry_get(c) == 0) {
            return c;
        }
    }
    return 0;
}

/* 空きクラスタを 1 つ確保し、鎖の終端印を打つ。無ければ 0。
 * 探索は FSInfo 由来のヒントから始め、末尾まで行ったら
 * 先頭へ回る (大きなカードでの素朴な対策。実測は未決事項) */
static uint32_t alloc_cluster(void)
{
    uint32_t limit = (uint32_t)(total_clusters + 2);
    uint32_t c = scan_free(alloc_hint, limit);

    if (c == 0) {
        c = scan_free(2, alloc_hint);
    }
    if (c == 0) {
        return 0;  /* 空きが無い */
    }
    if (fat_entry_set(c, 0x0FFFFFFFu) != 0) {
        return 0;
    }
    if (free_clusters != 0xFFFFFFFFFFFFFFFFULL) {
        free_clusters--;
    }
    alloc_hint = c + 1;
    if (alloc_hint >= limit) {
        alloc_hint = 2;
    }
    return c;
}

/* 鎖をまるごと空きに戻す。返した数を返す */
static uint64_t free_chain(uint32_t first)
{
    uint64_t freed = 0, guard = total_clusters + 2;
    uint32_t c = first;

    while (c >= 2 && c < 0x0FFFFFF8 && guard-- > 0) {
        uint32_t next = fat_next(c);
        if (fat_entry_set(c, 0) != 0) {
            break;
        }
        freed++;
        c = next;
    }
    if (free_clusters != 0xFFFFFFFFFFFFFFFFULL) {
        free_clusters += freed;
    }
    return freed;
}

/* FSInfo の空き数とヒントを実態に合わせて書き戻す。
 * FSInfo が無い・空き数が不明なボリュームでは何もしない */
static int fsinfo_sync(void)
{
    if (fsinfo_lba == 0 ||
        free_clusters == 0xFFFFFFFFFFFFFFFFULL) {
        return 0;
    }
    if (read_sector(fsinfo_lba, bounce) != 0) {
        return -1;
    }
    wr32(bounce + 488, (uint32_t)free_clusters);
    wr32(bounce + 492, alloc_hint);
    return write_sector(fsinfo_lba, bounce);
}

/* パスを「親ディレクトリのパス」と「最後の要素」に分ける。
 * 親は "" (ルート) になり得る。成功 0、形が悪ければ -1 */
static int split_parent(const char *path, char *dir, int dir_max,
                        char *last, int last_max)
{
    int i = 0, slash = -1, n;

    while (path[i] != '\0') {
        if (path[i] == '/') {
            slash = i;
        }
        i++;
    }
    if (slash < 0) {
        n = 0;
        dir[0] = '\0';
    } else {
        if (slash >= dir_max) {
            return -1;
        }
        for (n = 0; n < slash; n++) {
            dir[n] = path[n];
        }
        dir[n] = '\0';
    }
    n = 0;
    i = slash + 1;
    while (path[i] != '\0') {
        if (n >= last_max - 1) {
            return -1;
        }
        last[n++] = path[i++];
    }
    last[n] = '\0';
    if (n == 0) {
        return -1;  /* 最後の要素が無い */
    }
    return 0;
}

/* ディレクトリの鎖の中の空きスロット (0x00 = ここから先は
 * 未使用、0xE5 = 削除済み) を探す。見つかれば 0 で位置を、
 * 鎖が満杯なら 1 で「末尾の位置」(= 新しいクラスタの先頭に
 * なる位置) を返す */
static int find_free_slot(uint32_t dir_cluster, uint64_t *off_out)
{
    unsigned char e[32];
    uint64_t pos = 0;

    for (;;) {
        if (chain_read(dir_cluster, pos, e, 32,
                       0xFFFFFFFFFFFFFFFFULL) < 32) {
            *off_out = pos;
            return 1;  /* 鎖の終わり = 満杯 */
        }
        if (e[0] == 0x00 || e[0] == 0xE5) {
            *off_out = pos;
            return 0;
        }
        pos += 32;
    }
}

/* クラスタを 0 で埋める (ディレクトリ用) */
static int zero_cluster(uint32_t cluster)
{
    uint32_t s;
    int i;

    for (i = 0; i < SECTOR; i++) {
        bounce[i] = 0;
    }
    for (s = 0; s < sec_per_cluster; s++) {
        if (write_sector(cluster_lba(cluster) + s, bounce) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ディレクトリの鎖を 1 クラスタ伸ばす (0 埋めしてから繋ぐ) */
static int extend_dir_chain(uint32_t dir_cluster)
{
    uint32_t last = chain_last(dir_cluster, 0);
    uint32_t nc = alloc_cluster();

    if (nc == 0) {
        return -1;
    }
    if (zero_cluster(nc) != 0) {
        return -1;
    }
    if (fat_entry_set(last, nc) != 0) {
        return -1;
    }
    return 0;
}

/* 目録項を組み立てる */
static void build_entry(unsigned char ent[32], const char name83[11],
                        uint8_t attr, uint32_t cluster, uint32_t size,
                        uint16_t date, uint16_t tim)
{
    int i;

    for (i = 0; i < 32; i++) {
        ent[i] = 0;
    }
    for (i = 0; i < 11; i++) {
        ent[i] = (unsigned char)name83[i];
    }
    ent[11] = attr;
    wr16(ent + 14, tim);   /* 作成時刻 */
    wr16(ent + 16, date);  /* 作成日 */
    wr16(ent + 18, date);  /* アクセス日 */
    wr16(ent + 20, (uint16_t)(cluster >> 16));
    wr16(ent + 22, tim);   /* 更新時刻 */
    wr16(ent + 24, date);  /* 更新日 */
    wr16(ent + 26, (uint16_t)(cluster & 0xFFFF));
    wr32(ent + 28, size);
}

/* 親ディレクトリと名前から、書き込み先の目録項の場所を
 * 決める。同名ファイルがあれば *exist にその内容を入れて
 * 0 を返す (呼び出し側が切り捨て等を判断する)。無ければ
 * 空きスロット (必要なら鎖を伸ばす) の位置を返して 1。
 * 失敗は -1 */
static int prepare_slot(const char *path, uint32_t *dir_cluster_out,
                        char name83[11], uint64_t *off_out,
                        struct dir_entry *exist)
{
    char dirpart[128], last[40];
    uint32_t dir_cluster;

    if (split_parent(path, dirpart, sizeof(dirpart),
                     last, sizeof(last)) != 0 ||
        to_83(last, name83) != 0 ||
        resolve_dir(dirpart, &dir_cluster) != 0) {
        return -1;
    }
    *dir_cluster_out = dir_cluster;
    if (find_in_dir2(dir_cluster, name83, exist, off_out) == 0) {
        return 0;  /* 既にある */
    }
    if (find_free_slot(dir_cluster, off_out) == 1) {
        /* 満杯: 1 クラスタ伸ばして、その先頭を使う */
        if (extend_dir_chain(dir_cluster) != 0) {
            return -1;
        }
    }
    return 1;
}

int fat_create(const char *path, struct fat_file *out)
{
    char name83[11];
    uint32_t dir_cluster;
    uint64_t off;
    struct dir_entry exist;
    uint16_t date, tim;
    int r;

    if (fat_ensure_mounted() != 0) {
        return -1;
    }
    r = prepare_slot(path, &dir_cluster, name83, &off, &exist);
    if (r < 0) {
        return -1;
    }
    fat_now(&date, &tim);
    if (r == 0) {
        unsigned char ent[32];
        if (exist.is_dir) {
            return -1;  /* ディレクトリは作り直せない */
        }
        /* 切り捨て: 鎖を返してから項を空にする (FAT → 目録
         * の順)。作成日時は元の項を保ち、更新日時だけ進める */
        free_chain(exist.first_cluster);
        if (fat_cache_flush() != 0 ||
            entry_read(dir_cluster, off, ent) != 0) {
            return -1;
        }
        wr16(ent + 20, 0);
        wr16(ent + 26, 0);
        wr32(ent + 28, 0);
        wr16(ent + 22, tim);
        wr16(ent + 24, date);
        if (entry_write(dir_cluster, off, ent) != 0) {
            return -1;
        }
    } else {
        unsigned char ent[32];
        build_entry(ent, name83, 0x20, 0, 0, date, tim);
        if (entry_write(dir_cluster, off, ent) != 0) {
            return -1;
        }
    }
    out->first_cluster = 0;
    out->size = 0;
    out->dir_cluster = dir_cluster;
    out->dir_off = off;
    out->tail_cluster = 0;
    out->capacity = 0;
    out->has_dir = 1;
    out->dirty = 0;
    return 0;
}

/* ファイルの鎖を need バイト収まるまで育てる */
static int chain_grow(struct fat_file *f, uint64_t need)
{
    uint64_t cluster_bytes = (uint64_t)sec_per_cluster * SECTOR;

    while (f->capacity < need) {
        uint32_t c = alloc_cluster();
        if (c == 0) {
            return -1;  /* 空きが尽きた */
        }
        if (f->first_cluster == 0) {
            f->first_cluster = c;
        } else if (fat_entry_set(f->tail_cluster, c) != 0) {
            return -1;
        }
        f->tail_cluster = c;
        f->capacity += cluster_bytes;
    }
    return 0;
}

uint64_t fat_write_data(struct fat_file *f,
                        const unsigned char *src, uint64_t n)
{
    uint64_t pos, written;

    if (!f->has_dir || fat_ensure_mounted() != 0 || n == 0) {
        return 0;
    }
    if (f->capacity == 0 && f->first_cluster != 0) {
        /* 作成を経ていないファイル (防御): 鎖を数え直す */
        uint64_t cnt = 0;
        f->tail_cluster = chain_last(f->first_cluster, &cnt);
        f->capacity = cnt * (uint64_t)sec_per_cluster * SECTOR;
    }
    pos = f->size;
    if (chain_grow(f, pos + n) != 0 && f->capacity <= pos) {
        if (f->first_cluster != 0) {
            f->dirty = 1;  /* 確保だけ進んだ分は close で残す */
        }
        return 0;
    }
    if (pos + n > f->capacity) {
        n = f->capacity - pos;  /* 育った分まで */
    }
    written = chain_write_bytes(f->first_cluster, pos, src, n);
    if (written > 0) {
        f->size = pos + written;
        f->dirty = 1;
    }
    return written;
}

int fat_close_write(struct fat_file *f)
{
    unsigned char ent[32];
    uint16_t date, tim;

    if (!f->has_dir) {
        return -1;
    }
    if (!f->dirty) {
        return 0;  /* 書き戻すものが無い */
    }
    /* FAT → 目録項 → FSInfo の順に確定させる */
    if (fat_cache_flush() != 0) {
        return -1;
    }
    if (entry_read(f->dir_cluster, f->dir_off, ent) != 0) {
        return -1;
    }
    if (ent[0] == 0x00 || ent[0] == 0xE5) {
        return -1;  /* 項が消えている (unlink と競合した等) */
    }
    wr16(ent + 20, (uint16_t)(f->first_cluster >> 16));
    wr16(ent + 26, (uint16_t)(f->first_cluster & 0xFFFF));
    wr32(ent + 28, (uint32_t)f->size);
    fat_now(&date, &tim);
    wr16(ent + 18, date);
    wr16(ent + 22, tim);
    wr16(ent + 24, date);
    if (entry_write(f->dir_cluster, f->dir_off, ent) != 0) {
        return -1;
    }
    if (fsinfo_sync() != 0) {
        return -1;
    }
    f->dirty = 0;
    return 0;
}

int fat_unlink(const char *path)
{
    char dirpart[128], last[40];
    char name83[11];
    uint32_t dir_cluster;
    struct dir_entry e;
    uint64_t off;
    unsigned char ent[32];

    if (fat_ensure_mounted() != 0 ||
        split_parent(path, dirpart, sizeof(dirpart),
                     last, sizeof(last)) != 0 ||
        to_83(last, name83) != 0 ||
        resolve_dir(dirpart, &dir_cluster) != 0 ||
        find_in_dir2(dir_cluster, name83, &e, &off) != 0) {
        return -1;
    }
    if (e.is_dir) {
        return -1;  /* ディレクトリは unlink しない */
    }
    /* 項を先に消す。中断したときの被害は「鎖が迷子になる」
     * 側に寄せ、生きている項が解放済みクラスタを指す形は
     * 作らない */
    if (entry_read(dir_cluster, off, ent) != 0) {
        return -1;
    }
    ent[0] = 0xE5;
    if (entry_write(dir_cluster, off, ent) != 0) {
        return -1;
    }
    free_chain(e.first_cluster);
    if (fat_cache_flush() != 0) {
        return -1;
    }
    return fsinfo_sync();
}

int fat_rmdir(const char *path)
{
    char dirpart[128], last[40];
    char name83[11];
    uint32_t dir_cluster;
    struct dir_entry e;
    uint64_t off;
    unsigned char ent[32];
    uint64_t pos;

    if (fat_ensure_mounted() != 0 ||
        split_parent(path, dirpart, sizeof(dirpart),
                     last, sizeof(last)) != 0 ||
        to_83(last, name83) != 0 ||
        resolve_dir(dirpart, &dir_cluster) != 0 ||
        find_in_dir2(dir_cluster, name83, &e, &off) != 0) {
        return -1;
    }
    if (!e.is_dir) {
        return -1;  /* ファイルは rmdir しない (unlink の担当) */
    }
    /* 空であること: "." と ".." 以外の生きている項が 1 つ
     * でもあれば消せない。削除済み (0xE5) と LFN とボリューム
     * ラベルは数えない (fat_list の読み飛ばしと同じ扱い)。
     * 中のファイルが開かれている間はその項が存在するので、
     * この点検が「開いているものを含むディレクトリは消せ
     * ない」を兼ねる (ディレクトリ自体を開く fd は現行ない) */
    pos = 0;
    for (;;) {
        unsigned char chk[32];
        if (chain_read(e.first_cluster, pos, chk, 32,
                       0xFFFFFFFFFFFFFFFFULL) < 32) {
            break;  /* 鎖の終わりまで見た */
        }
        pos += 32;
        if (chk[0] == 0x00) {
            break;  /* ここから先は未使用 */
        }
        if (chk[0] == 0xE5 || chk[11] == 0x0F || (chk[11] & 0x08)) {
            continue;
        }
        if (chk[0] == '.') {
            continue;  /* "." と ".." */
        }
        return -1;  /* 中身がある */
    }
    /* 順序は fat_unlink と同じ: 項を先に消してから鎖を返し、
     * 最後に FAT の書き戻しと FSInfo の更新 */
    if (entry_read(dir_cluster, off, ent) != 0) {
        return -1;
    }
    ent[0] = 0xE5;
    if (entry_write(dir_cluster, off, ent) != 0) {
        return -1;
    }
    free_chain(e.first_cluster);
    if (fat_cache_flush() != 0) {
        return -1;
    }
    return fsinfo_sync();
}

int fat_mkdir(const char *path)
{
    char name83[11];
    uint32_t dir_cluster, nc;
    uint64_t off;
    struct dir_entry exist;
    uint16_t date, tim;
    unsigned char dots[64];
    unsigned char ent[32];
    int r;

    if (fat_ensure_mounted() != 0) {
        return -1;
    }
    r = prepare_slot(path, &dir_cluster, name83, &off, &exist);
    if (r != 1) {
        return -1;  /* 既にある (r == 0) か、親がたどれない */
    }
    nc = alloc_cluster();
    if (nc == 0) {
        return -1;
    }
    if (zero_cluster(nc) != 0) {
        return -1;
    }
    fat_now(&date, &tim);
    /* "." は自分、".." は親 (親がルートなら 0 が約束)。
     * 名前は空白詰めの 11 バイトを組み立てて渡す */
    {
        char dot1[11], dot2[11];
        int i;
        for (i = 0; i < 11; i++) {
            dot1[i] = ' ';
            dot2[i] = ' ';
        }
        dot1[0] = '.';
        dot2[0] = '.';
        dot2[1] = '.';
        build_entry(dots, dot1, 0x10, nc, 0, date, tim);
        build_entry(dots + 32, dot2, 0x10,
                    dir_cluster == root_cluster ? 0 : dir_cluster,
                    0, date, tim);
    }
    if (chain_write_bytes(nc, 0, dots, 64) != 64) {
        return -1;
    }
    build_entry(ent, name83, 0x10, nc, 0, date, tim);
    if (entry_write(dir_cluster, off, ent) != 0) {
        return -1;
    }
    if (fat_cache_flush() != 0) {
        return -1;
    }
    return fsinfo_sync();
}
