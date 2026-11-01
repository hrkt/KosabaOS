#ifndef KOSABAOS_TRAP_H
#define KOSABAOS_TRAP_H

/* トラップ (例外・割り込み) の初期化。実装は trap.c、
 * 入口のアセンブラは trap.S。 */

void trap_init(void);  /* stvec を設定し、タイマ割り込みを有効にする */

#endif
