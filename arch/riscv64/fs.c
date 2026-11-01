/* fs.c -- 最小の RAM ファイルシステム (ステップ 6)
 *
 * ディスクはまだ無いので、ファイルは全部メモリの中に置く。
 * 構造は一番単純な形: 「名前 → データとサイズ」の表と、
 * データの実体を詰め込む arena (大きなバイト配列)。
 *
 * この段階でやること:
 *   - 初期ファイルを用意する (hello.txt と、埋め込みの
 *     ユーザープログラム uprog.elf)
 *   - 名前でファイルを探せるようにする
 * ユーザープログラムがシステムコールでファイルを読むのは、
 * 次のシェルの段階で足す。今はカーネル内の仕組みと、
 * 「プログラムを名前で引いて実行する」ための土台。
 */

#include <stdint.h>
#include "fs.h"
#include "uart.h"

#define FS_MAX_FILES 16
#define FS_ARENA_SIZE (256 * 1024)

static struct fs_file files[FS_MAX_FILES];
static int file_count = 0;
static unsigned char arena[FS_ARENA_SIZE];
static uint64_t arena_used = 0;

/* embed.S が用意する埋め込み ELF たち */
extern unsigned char user_prog_start[];
extern unsigned char user_prog_end[];
extern unsigned char shell_prog_start[];
extern unsigned char shell_prog_end[];

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

int fs_add(const char *name, const unsigned char *src, uint64_t size)
{
    struct fs_file *f;
    uint64_t i;
    int n = 0;

    if (file_count >= FS_MAX_FILES || arena_used + size > FS_ARENA_SIZE) {
        return -1;
    }
    f = &files[file_count];
    while (name[n] != '\0' && n < 31) {
        f->name[n] = name[n];
        n++;
    }
    f->name[n] = '\0';
    f->data = arena + arena_used;
    for (i = 0; i < size; i++) {
        f->data[i] = src[i];
    }
    f->size = size;
    arena_used += size;
    file_count++;
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

    fs_add("hello.txt", (const unsigned char *)hello, sizeof(hello) - 1);
    fs_add("uprog.elf", user_prog_start, elf_size);
    fs_add("shell.elf", shell_prog_start,
           (uint64_t)(shell_prog_end - shell_prog_start));
}

int fs_count(void)
{
    return file_count;
}

struct fs_file *fs_at(int index)
{
    if (index < 0 || index >= file_count) {
        return 0;
    }
    return &files[index];
}
