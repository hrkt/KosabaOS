/* boards/k210/smp.c -- hart1 の起こし (K210)
 * (doc/smp-design.md 段1)
 *
 * 段1a (virt) の時点ではまだ起こし方を用意していない
 * ので、失敗を返すだけの受け口。段1b で、M シム側の
 * park を「解放ワード待ち」に変え、ここからワードを
 * 立てて起こす形にする。
 */

#include "board.h"
#include "smp.h"

int board_start_hart1(void)
{
    return -1;  /* 段1b で実装する */
}
