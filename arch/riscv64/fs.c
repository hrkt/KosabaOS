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

#define FS_MAX_FILES 16
#define FS_ARENA_SIZE BOARD_FS_ARENA_SIZE

#define FS_BLOCK_SIZE 4096ULL
#define FS_BLOCK_COUNT (FS_ARENA_SIZE / FS_BLOCK_SIZE)
#define BLOCK_FREE 0xFFFFFFFFu  /* block_next の値: 空き */
#define BLOCK_END  0xFFFFFFFEu  /* block_next の値: 鎖の終わり */

static struct fs_file files[FS_MAX_FILES];
static int file_count = 0;
static unsigned char arena[FS_ARENA_SIZE];
static uint32_t block_next[FS_BLOCK_COUNT];
static uint64_t blocks_used = 0;

/* embed.S が用意する埋め込み ELF たち */
extern unsigned char user_prog_start[];
extern unsigned char user_prog_end[];
extern unsigned char nish_prog_start[];
extern unsigned char nish_prog_end[];

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
    struct fs_file *f;
    int n = 0;

    if (file_count >= FS_MAX_FILES) {
        return 0;
    }
    f = &files[file_count];
    while (name[n] != '\0' && n < 31) {
        f->name[n] = name[n];
        n++;
    }
    f->name[n] = '\0';
    f->first_block = FS_NO_BLOCK;
    f->size = 0;
    file_count++;
    return f;
}

struct fs_file *fs_create(const char *name)
{
    struct fs_file *f = fs_find(name);

    if (f != 0) {
        /* 同名があれば空にする (ブロックは全部返す) */
        chain_free(f->first_block);
        f->first_block = FS_NO_BLOCK;
        f->size = 0;
        return f;
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
    for (i = 0; i < file_count; i++) {
        if (name_eq(files[i].name, name)) {
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
    for (i = 0; i < file_count; i++) {
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
}

int fs_count(void)
{
    return file_count;
}

uint64_t fs_used(void)
{
    return blocks_used * FS_BLOCK_SIZE;
}

struct fs_file *fs_at(int index)
{
    if (index < 0 || index >= file_count) {
        return 0;
    }
    return &files[index];
}
