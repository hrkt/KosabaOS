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

#include "uart.h"
#include "trap.h"
#include "vm.h"

/* embed.S が用意する、埋め込みユーザープログラムの範囲 */
extern unsigned char user_prog_start[];
extern unsigned char user_prog_end[];

/* user.S: ユーザーモードへ降り、プログラムが exit したら戻る */
extern void user_launch(unsigned long entry, unsigned long user_stack_top);

/* ユーザープログラムを実行する (ステップ 4)。
 * 埋め込みバイナリを所定の物理メモリへコピーしてから降りる。
 * プログラムが exit するか保護違反で終わると、ここへ戻ってくる。 */
static void start_user(void)
{
    unsigned long size = (unsigned long)(user_prog_end - user_prog_start);
    unsigned char *dst = (unsigned char *)USER_CODE_PA;
    unsigned long i;

    for (i = 0; i < size; i++) {
        dst[i] = user_prog_start[i];
    }
    /* 書いた命令を CPU の命令キャッシュ側にも反映させる */
    asm volatile("fence.i");
    uart_puts("\n[kernel] starting user program ('q' quits it)\n");
    user_launch(USER_CODE_VA, USER_STACK_TOP_VA);
    uart_puts("\n[kernel] back from user program\n");
}

void kmain(void)
{
    uart_init();
    trap_init();  /* タイマ割り込みを開始 (1 秒ごとに tick と表示) */
    vm_init();    /* Sv39 ページングを有効にする (ステップ 3) */
    uart_puts("hello from KosabaOS (S-mode)\n");

    for (;;) {
        char c = uart_getc();  /* 1 文字届くまで待って受け取る */
        if (c == 'x') {
            vm_fault_demo();   /* 'x' キーでページフォルトのデモ */
        }
        if (c == 'u') {
            start_user();      /* 'u' キーでユーザープログラム実行 */
            continue;
        }
        uart_putc(c);          /* そのまま打ち返す */
    }
}
