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
#include "board.h"
#include "uart.h"
#include "trap.h"
#include "vm.h"
#include "page.h"
#include "proc.h"
#include "elf.h"
#include "fs.h"
#if BOARD_FEATURE_LCD
#include "lcd.h"  /* ボード付属 LCD へのステータス表示 (k210 のみ) */

/* LCD の NOW 行: プロジェクトの現在地。ステップが進んだら
 * ここを書き換える (済んだものはステージ行へ移す) */
#define LCD_NOW_TEXT "NOW: STEP5 NISH + EXEC"
#endif

#if BOARD_FEATURE_SMODE_PATH
/* user.S: ユーザーモードへ降り、プログラムが exit したら戻る */
extern void user_launch(unsigned long entry, unsigned long user_stack_top,
                        unsigned long argc, unsigned long argv);
#endif

#if BOARD_FEATURE_SMODE_PATH
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
    entry = elf_load(f);

    if (entry == 0) {
        uart_puts("\n[kernel] failed to load user program\n");
        return;
    }
    uart_puts("\n[kernel] starting ");
    uart_puts(name);
    uart_putc('\n');
    /* カーネルから直接起動する場合の引数は {プログラム名} のみ */
    {
        const char *tokens[1];
        uint64_t argv_va, sp;
        tokens[0] = name;
        sp = user_stack_setup_argv(USER_STACK_PA, 1, tokens, &argv_va);
        user_launch(entry, sp, 1, argv_va);
    }
    uart_puts("\n[kernel] back from user program\n");
}
#endif /* BOARD_FEATURE_SMODE_PATH */

void kmain(void)
{
    uart_init();
#if BOARD_FEATURE_LCD
    /* LCD にタイトルと最初のステージを出す。以後のステージは
     * 各初期化が終わるたびに 1 行ずつ足す。最後に tick 表示を
     * 有効化すると、割り込みハンドラが毎秒数字を描き直す */
    lcd_init();
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, LCD_RGB(24, 24, 24));
    lcd_draw_text(64, 12, "KOSABAOS", 4, LCD_WHITE);
    /* ボード名 + 実測クロック (board_cpu_freq は sysctl 読み戻し) */
    lcd_draw_text(88, 48, "BOARD K210", 2, LCD_RGB(150, 150, 150));
    {
        uint32_t w = lcd_draw_u32(214, 48, board_cpu_freq() / 1000000,
                                  2, LCD_RGB(150, 150, 150));
        lcd_draw_text(214 + w + 6, 48, "MHZ", 2, LCD_RGB(150, 150, 150));
    }
    /* プロジェクトのゴールと現在地 */
    lcd_draw_text(22, 74, "GOAL: HELLO WORLD ON HW", 2, LCD_RGB(140, 240, 140));
    lcd_draw_text(28, 94, LCD_NOW_TEXT, 2, LCD_RGB(255, 210, 110));
    lcd_draw_text(28, 116, "STEP2 BOARD OK", 2, LCD_RGB(200, 200, 200));
#endif
#if BOARD_FEATURE_SMODE_PATH
    trap_init();  /* タイマ割り込みを開始 (1 秒ごとに tick と表示) */
    page_init();  /* ページアロケータ (プールはボード依存) */
    vm_init();    /* Sv39 ページングを有効にする (ステップ 3) */
#endif
#if BOARD_FEATURE_MMODE_TRAP
    mtrap_init(); /* M モードのタイマ割り込み (CLINT 直叩き、k210) */
#endif
#if BOARD_FEATURE_LCD
    lcd_draw_text(28, 132, "STEP3 TRAP TIMER OK", 2, LCD_RGB(200, 200, 200));
#endif
    fs_init();    /* RAM ファイルシステム (ステップ 6) */
#if BOARD_FEATURE_LCD
    lcd_draw_text(28, 148, "STEP4 USER MODE OK", 2, LCD_RGB(200, 200, 200));
    lcd_draw_text(28, 164, "FS READY", 2, LCD_RGB(200, 200, 200));
    /* 機械の状態: hart1 は駐車中、FS のファイル数 */
    lcd_draw_text(28, 198, "HART1 PARK FS", 2, LCD_RGB(150, 150, 150));
    {
        uint32_t w = lcd_draw_u32(196, 198, (uint64_t)fs_count(),
                                  2, LCD_RGB(150, 150, 150));
        lcd_draw_text(196 + w + 6, 198, "FILES", 2, LCD_RGB(150, 150, 150));
    }
#endif
    uart_puts("hello from KosabaOS " BOARD_HELLO_SUFFIX "\n");

#if BOARD_FEATURE_SMODE_PATH
    /* シェル Nish を最初に起動する (主役化)。
     * Nish で exit すると下のカーネルコンソールに落ちる */
    start_user_named("nish.elf");
    uart_puts("[kernel] kernel console "
              "(u: uprog, s: Nish, m: multitask, l: files, "
              "x: fault, p: pages)\n");
#endif

#if BOARD_FEATURE_LCD
    lcd_draw_text(28, 180, "CONSOLE READY", 2, LCD_RGB(200, 200, 200));
    lcd_draw_text(28, 216, "TICK", 3, LCD_CYAN);
    lcd_tick_setup(118, 216, 3, LCD_CYAN, LCD_RGB(24, 24, 24));
    lcd_tick_draw(0);
#endif

    for (;;) {
        char c = uart_getc();  /* 1 文字届くまで待って受け取る */
#if BOARD_FEATURE_SMODE_PATH
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
        if (c == 'p') {
            page_demo();       /* 'p' キーでページアロケータのデモ */
            continue;
        }
#endif
        if (c == 'l') {
            uart_putc('\n');
            fs_list();         /* 'l' キーでファイル一覧 */
            continue;
        }
        uart_putc(c);          /* そのまま打ち返す */
    }
}
