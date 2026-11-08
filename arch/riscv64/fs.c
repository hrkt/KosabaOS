/* fs.c -- 最小の RAM ファイルシステム (ステップ 6、再設計 段1)
 *
 * ディスクはまだ無いので、ファイルは全部メモリの中に置く。
 * データの持ち方はブロックチェーン方式 (doc/fs-design.md の
 * 案A、いわゆる FAT 風):
 *   arena (大きなバイト配列) を 4KB のブロックの並びと見て、
 *   ファイルはブロックの鎖で表す。block_next[] が各ブロックの
 *   「次」を持ち、空きと鎖の終わりは特別な値で表す。
 * これで、作る前に上限いっぱい予約する旧方式と違って、
 * ファイルは書いた分だけ育ち、容量は全ファイルで分け合える。
 *
 * 読み書きは fs_read_at / fs_write_data 経由に統一し、
 * データの生ポインタは外に出さない (ELF ローダも同じ)。
 * コピーは 1 バイトずつ: K210 は境界にそろっていない
 * 読み書きがそのまま例外になるので、その約束を守るため。
 */

#include <stdint.h>
#include "fs.h"
#include "uart.h"
#include "board.h"  /* arena の大きさはボード依存 */

#define FS_MAX_FILES 64
#define FS_ARENA_SIZE BOARD_FS_ARENA_SIZE

#define FS_BLOCK_SIZE 4096ULL
#define FS_BLOCK_COUNT (FS_ARENA_SIZE / FS_BLOCK_SIZE)
#define BLOCK_FREE 0xFFFFFFFFu  /* block_next の値: 空き */
#define BLOCK_END  0xFFFFFFFEu  /* block_next の値: 鎖の終わり */

static struct fs_file files[FS_MAX_FILES];
static unsigned char arena[FS_ARENA_SIZE];
static uint32_t block_next[FS_BLOCK_COUNT];
static uint64_t blocks_used = 0;

/* embed.S が用意する埋め込み ELF たち */
extern unsigned char user_prog_start[];
extern unsigned char user_prog_end[];
extern unsigned char nish_prog_start[];
extern unsigned char nish_prog_end[];
extern unsigned char chain_prog_start[];
extern unsigned char chain_prog_end[];
extern unsigned char eat_prog_start[];
extern unsigned char eat_prog_end[];
extern unsigned char uptime_prog_start[];
extern unsigned char uptime_prog_end[];
extern unsigned char atest_prog_start[];
extern unsigned char atest_prog_end[];
extern unsigned char fptest_prog_start[];
extern unsigned char fptest_prog_end[];
extern unsigned char slow_prog_start[];
extern unsigned char slow_prog_end[];
extern unsigned char crash_prog_start[];
extern unsigned char crash_prog_end[];
extern unsigned char copy_prog_start[];
extern unsigned char copy_prog_end[];
extern unsigned char gdemo_prog_start[];
extern unsigned char gdemo_prog_end[];
extern unsigned char gjob_prog_start[];
extern unsigned char gjob_prog_end[];
#ifdef IKESU_EMBED
/* Ikesu 合流テストの素材 (build.sh が写してきたときだけ) */
extern unsigned char ikesu_prog_start[];
extern unsigned char ikesu_prog_end[];
extern unsigned char player_wasm_start[];
extern unsigned char player_wasm_end[];
extern unsigned char test_mp3_start[];
extern unsigned char test_mp3_end[];
#endif

static int name_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == b[i];
}

/* --- ブロックの確保と解放 ------------------------------------------ */

static uint32_t block_alloc(void)
{
    uint64_t i;
    for (i = 0; i < FS_BLOCK_COUNT; i++) {
        if (block_next[i] == BLOCK_FREE) {
            block_next[i] = BLOCK_END;
            blocks_used++;
            return (uint32_t)i;
        }
    }
    return FS_NO_BLOCK;  /* arena いっぱい */
}

static void chain_free(uint32_t first)
{
    while (first != FS_NO_BLOCK) {
        uint32_t next = block_next[first];
        block_next[first] = BLOCK_FREE;
        blocks_used--;
        first = (next == BLOCK_END) ? FS_NO_BLOCK : next;
    }
}

static unsigned char *block_ptr(uint32_t b)
{
    return arena + (uint64_t)b * FS_BLOCK_SIZE;
}

/* ファイルの pos バイト目が入っているブロックを鎖でたどる。
 * そのブロック番号と、ブロック内オフセットを返す
 * (ブロックが無ければ FS_NO_BLOCK) */
static uint32_t chain_seek(struct fs_file *f, uint64_t pos,
                           uint64_t *offset_in_block)
{
    uint64_t index = pos / FS_BLOCK_SIZE;
    uint32_t b = f->first_block;
    while (index > 0 && b != FS_NO_BLOCK && b != BLOCK_END) {
        b = block_next[b];
        index--;
    }
    if (index > 0 || b == BLOCK_END) {
        return FS_NO_BLOCK;
    }
    *offset_in_block = pos % FS_BLOCK_SIZE;
    return b;
}

/* --- 読み書き ------------------------------------------------------- */

uint64_t fs_read_at(struct fs_file *f, uint64_t pos,
                    unsigned char *dst, uint64_t n)
{
    uint64_t done = 0;

    if (pos >= f->size) {
        return 0;
    }
    if (n > f->size - pos) {
        n = f->size - pos;  /* 終端までで止める */
    }
    while (done < n) {
        uint64_t off, room, j;
        uint32_t b = chain_seek(f, pos + done, &off);
        if (b == FS_NO_BLOCK) {
            break;  /* 鎖が途切れている (通常は来ない) */
        }
        room = FS_BLOCK_SIZE - off;
        if (room > n - done) {
            room = n - done;
        }
        for (j = 0; j < room; j++) {
            dst[done + j] = block_ptr(b)[off + j];
        }
        done += room;
    }
    return done;
}

uint64_t fs_write_data(struct fs_file *f, const unsigned char *src,
                       uint64_t n)
{
    uint64_t done = 0;

    while (done < n) {
        uint64_t pos = f->size;
        uint64_t off, room, j;
        uint32_t b = chain_seek(f, pos, &off);

        if (b == FS_NO_BLOCK) {
            /* 末尾の次が無い = 鎖を 1 ブロック伸ばす */
            uint32_t nb = block_alloc();
            if (nb == FS_NO_BLOCK) {
                break;  /* arena いっぱい。書けた分だけ返す */
            }
            if (f->first_block == FS_NO_BLOCK) {
                f->first_block = nb;
            } else {
                /* 最後のブロックの次につなぐ */
                uint32_t last = f->first_block;
                while (block_next[last] != BLOCK_END) {
                    last = block_next[last];
                }
                block_next[last] = nb;
            }
            b = nb;
            off = 0;
        }
        room = FS_BLOCK_SIZE - off;
        if (room > n - done) {
            room = n - done;
        }
        for (j = 0; j < room; j++) {
            block_ptr(b)[off + j] = src[done + j];
        }
        f->size += room;
        done += room;
    }
    return done;
}

/* --- ファイル表 ----------------------------------------------------- */

static struct fs_file *new_entry(const char *name)
{
    struct fs_file *f = 0;
    int i, n = 0;

    /* 空いている項を探す (削除で空いた項も再利用する) */
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            f = &files[i];
            break;
        }
    }
    if (f == 0) {
        return 0;  /* 表いっぱい */
    }
    while (name[n] != '\0' && n < FS_NAME_MAX - 1) {
        f->name[n] = name[n];
        n++;
    }
    f->name[n] = '\0';
    f->first_block = FS_NO_BLOCK;
    f->size = 0;
    f->used = 1;
    f->is_dir = 0;
    return f;
}

/* 正規化済み名前の親ディレクトリ部分を out に書く。
 * "work/memo.txt" → "work"、"memo.txt" → "" (ルート) */
static void parent_of(const char *name, char *out)
{
    const char *slash = 0;
    const char *p;
    int n = 0;

    for (p = name; *p != '\0'; p++) {
        if (*p == '/') {
            slash = p;
        }
    }
    if (slash == 0) {
        out[0] = '\0';
        return;
    }
    while (name + n < slash) {
        out[n] = name[n];
        n++;
    }
    out[n] = '\0';
}

int fs_is_dir(const char *name)
{
    struct fs_file *f;

    if (name[0] == '\0') {
        return 1;  /* ルートは常にディレクトリ */
    }
    f = fs_find(name);
    return f != 0 && f->is_dir;
}

int fs_unlink(const char *name)
{
    struct fs_file *f = fs_find(name);

    if (f == 0 || f->is_dir) {
        return -1;  /* ディレクトリは rmdir で消す */
    }
    chain_free(f->first_block);  /* ブロックは全部 arena へ返す */
    f->first_block = FS_NO_BLOCK;
    f->size = 0;
    f->name[0] = '\0';
    f->used = 0;
    return 0;
}

int fs_mkdir(const char *name)
{
    char parent[FS_NAME_MAX];
    struct fs_file *f;

    if (name[0] == '\0' || fs_find(name) != 0) {
        return -1;  /* ルート自身、または既にある名前 */
    }
    parent_of(name, parent);
    if (!fs_is_dir(parent)) {
        return -1;  /* 親が無い (または親がファイル) */
    }
    f = new_entry(name);
    if (f == 0) {
        return -1;
    }
    f->is_dir = 1;
    return 0;
}

int fs_rmdir(const char *name)
{
    struct fs_file *f = fs_find(name);
    char parent[FS_NAME_MAX];
    int i;

    if (f == 0 || !f->is_dir) {
        return -1;
    }
    /* 子が 1 つでもあれば消せない (空のときだけ) */
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used || &files[i] == f) {
            continue;
        }
        parent_of(files[i].name, parent);
        if (name_eq(parent, name)) {
            return -1;
        }
    }
    f->name[0] = '\0';
    f->used = 0;
    f->is_dir = 0;
    return 0;
}

/* dir の子を表の順に数え、index 番目の要素名 (名前の最後の
 * 要素だけ) を base に返す。readdir (trap.c) から使う */
int fs_child(const char *dir, int index, char *base, int base_max,
             int *is_dir_out)
{
    char parent[FS_NAME_MAX];
    int i, n = 0;

    if (index < 0) {
        return 0;
    }
    for (i = 0; i < FS_MAX_FILES; i++) {
        const char *b;
        int j;
        if (!files[i].used) {
            continue;
        }
        parent_of(files[i].name, parent);
        if (!name_eq(parent, dir)) {
            continue;
        }
        if (n != index) {
            n++;
            continue;
        }
        /* 最後の '/' の後ろが要素名 */
        b = files[i].name;
        for (j = 0; files[i].name[j] != '\0'; j++) {
            if (files[i].name[j] == '/') {
                b = files[i].name + j + 1;
            }
        }
        for (j = 0; b[j] != '\0' && j < base_max - 1; j++) {
            base[j] = b[j];
        }
        base[j] = '\0';
        *is_dir_out = files[i].is_dir;
        return 1;
    }
    return 0;
}

struct fs_file *fs_create(const char *name)
{
    struct fs_file *f = fs_find(name);
    char parent[FS_NAME_MAX];

    if (f != 0) {
        if (f->is_dir) {
            return 0;  /* ディレクトリは作り直せない */
        }
        /* 同名があれば空にする (ブロックは全部返す) */
        chain_free(f->first_block);
        f->first_block = FS_NO_BLOCK;
        f->size = 0;
        return f;
    }
    /* 親ディレクトリが実在する場合だけ作る (段4) */
    parent_of(name, parent);
    if (!fs_is_dir(parent)) {
        return 0;
    }
    return new_entry(name);  /* 予約はしない。書くたびに育つ */
}

int fs_add(const char *name, const unsigned char *src, uint64_t size)
{
    struct fs_file *f;
    uint64_t need = (size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;

    if (FS_BLOCK_COUNT - blocks_used < need) {
        return -1;  /* 入り切らない (組み込みでは起きない想定) */
    }
    f = new_entry(name);
    if (f == 0) {
        return -1;
    }
    if (fs_write_data(f, src, size) != size) {
        return -1;
    }
    return 0;
}

struct fs_file *fs_find(const char *name)
{
    int i;
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && name_eq(files[i].name, name)) {
            return &files[i];
        }
    }
    return 0;
}

static void put_u64(uint64_t x)
{
    char buf[20];
    int n = 0;

    if (x == 0) {
        uart_putc('0');
        return;
    }
    while (x > 0) {
        buf[n++] = (char)('0' + (x % 10));
        x /= 10;
    }
    while (n > 0) {
        uart_putc(buf[--n]);
    }
}

void fs_list(void)
{
    int i;
    uart_puts("[fs] files:\n");
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            continue;
        }
        uart_puts("  ");
        uart_puts(files[i].name);
        uart_puts("  ");
        put_u64(files[i].size);
        uart_puts(" bytes\n");
    }
}

void fs_init(void)
{
    static const char hello[] =
        "Hello from the KosabaOS RAM filesystem.\n"
        "This file lives only in memory (for now).\n";
    uint64_t elf_size =
        (uint64_t)(user_prog_end - user_prog_start);
    uint64_t i;

    /* ブロック表を全部「空き」に (BSS のゼロは BLOCK 番号 0 と
     * 紛らわしいので、ここで明示的に用意する) */
    for (i = 0; i < FS_BLOCK_COUNT; i++) {
        block_next[i] = BLOCK_FREE;
    }

    fs_add("hello.txt", (const unsigned char *)hello, sizeof(hello) - 1);
    fs_add("uprog.elf", user_prog_start, elf_size);
    fs_add("nish.elf", nish_prog_start,
           (uint64_t)(nish_prog_end - nish_prog_start));
    fs_add("chain.elf", chain_prog_start,
           (uint64_t)(chain_prog_end - chain_prog_start));
    fs_add("eat.elf", eat_prog_start,
           (uint64_t)(eat_prog_end - eat_prog_start));
    fs_add("uptime.elf", uptime_prog_start,
           (uint64_t)(uptime_prog_end - uptime_prog_start));
    fs_add("atest.elf", atest_prog_start,
           (uint64_t)(atest_prog_end - atest_prog_start));
    fs_add("fptest.elf", fptest_prog_start,
           (uint64_t)(fptest_prog_end - fptest_prog_start));
    fs_add("slow.elf", slow_prog_start,
           (uint64_t)(slow_prog_end - slow_prog_start));
    fs_add("crash.elf", crash_prog_start,
           (uint64_t)(crash_prog_end - crash_prog_start));
    fs_add("copy.elf", copy_prog_start,
           (uint64_t)(copy_prog_end - copy_prog_start));
    fs_add("gdemo.elf", gdemo_prog_start,
           (uint64_t)(gdemo_prog_end - gdemo_prog_start));
    fs_add("gjob.elf", gjob_prog_start,
           (uint64_t)(gjob_prog_end - gjob_prog_start));
#ifdef IKESU_EMBED
    fs_add("ikesu.elf", ikesu_prog_start,
           (uint64_t)(ikesu_prog_end - ikesu_prog_start));
    fs_add("player.wasm", player_wasm_start,
           (uint64_t)(player_wasm_end - player_wasm_start));
    fs_add("test.mp3", test_mp3_start,
           (uint64_t)(test_mp3_end - test_mp3_start));
#endif
}

int fs_count(void)
{
    int i, n = 0;
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used) {
            n++;
        }
    }
    return n;
}

uint64_t fs_used(void)
{
    return blocks_used * FS_BLOCK_SIZE;
}

/* index 番目の「生きている」ファイルを返す (readdir 用)。
 * fs_count と対で使う: 0 〜 fs_count()-1 が全部のファイルに
 * 対応する (削除で空いた項は飛ばして数える) */
struct fs_file *fs_at(int index)
{
    int i;
    if (index < 0) {
        return 0;
    }
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            continue;
        }
        if (index == 0) {
            return &files[i];
        }
        index--;
    }
    return 0;
}
