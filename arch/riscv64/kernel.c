/* kernel.c -- KosabaOS カーネル本体 (riscv64)
 *
 * start.S がスタックと .bss の準備をしてから呼ぶ、C 側の入口。
 *   1. UART を初期化する
 *   2. トラップ (例外・割り込み) の受け口を有効にし、
 *      タイマ割り込みで 1 秒ごとに tick と出す (ステップ 2)
 *   3. Sv39 ページングを有効にする (恒等マッピング)
 *   4. hello を出す
 *   5. 打たれたキーをそのまま打ち返す (echo) を繰り返す。
 *      例外として 'x' キーはページフォルトのデモ、'u' キーは
 *      ユーザープログラムの実行 (ステップ 4) を起動する
 * echo のループはタイマ割り込みに何度も割り込まれるが、
 * レジスタの退避と復元のおかげで何事もなく続きが動く。
 */

#include <stdint.h>
#include "uart.h"
#include "trap.h"
#include "vm.h"
#include "proc.h"
#include "elf.h"
#include "fs.h"

/* user.S: ユーザーモードへ降り、プログラムが exit したら戻る */
extern void user_launch(unsigned long entry, unsigned long user_stack_top);

/* ユーザープログラムを名前で実行する (ステップ 4 で導入、
 * ELF 化とファイルシステム経由の取得はステップ 6)。
 * ファイルシステムから名前で引いて、ローダ (elf.c) で配置
 * してから降りる。プログラムが exit するか保護違反で
 * 終わると、ここへ戻ってくる。 */
static void start_user_named(const char *name)
{
    struct fs_file *f = fs_find(name);
    uint64_t entry;

    if (f == 0) {
        uart_puts("\n[kernel] file not found\n");
        return;
    }
    entry = elf_load(f->data);

    if (entry == 0) {
        uart_puts("\n[kernel] failed to load user program\n");
        return;
    }
    uart_puts("\n[kernel] starting ");
    uart_puts(name);
    uart_putc('\n');
    user_launch(entry, USER_STACK_TOP_VA);
    uart_puts("\n[kernel] back from user program\n");
}

void kmain(void)
{
    uart_init();
    trap_init();  /* タイマ割り込みを開始 (1 秒ごとに tick と表示) */
    vm_init();    /* Sv39 ページングを有効にする (ステップ 3) */
    fs_init();    /* RAM ファイルシステム (ステップ 6) */
    uart_puts("hello from KosabaOS (S-mode)\n");

    /* シェル Nish を最初に起動する (主役化)。
     * Nish で exit すると下のカーネルコンソールに落ちる */
    start_user_named("nish.elf");
    uart_puts("[kernel] kernel console "
              "(u: uprog, s: Nish, m: multitask, l: files, x: fault)\n");

    for (;;) {
        char c = uart_getc();  /* 1 文字届くまで待って受け取る */
        if (c == 'x') {
            vm_fault_demo();   /* 'x' キーでページフォルトのデモ */
        }
        if (c == 'u') {
            start_user_named("uprog.elf");  /* 'u' キーで実行 */
            continue;
        }
        if (c == 's') {
            start_user_named("nish.elf");  /* 's' キーで Nish */
            continue;
        }
        if (c == 'm') {
            proc_demo();       /* 'm' キーでマルチタスクのデモ */
            continue;
        }
        if (c == 'l') {
            uart_putc('\n');
            fs_list();         /* 'l' キーでファイル一覧 */
            continue;
        }
        uart_putc(c);          /* そのまま打ち返す */
    }
}
