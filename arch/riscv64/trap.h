#ifndef KOSABAOS_TRAP_H
#define KOSABAOS_TRAP_H

#include <stdint.h>

/* トラップ (例外・割り込み) の初期化。実装は trap.c、
 * 入口のアセンブラは trap.S。 */

void trap_init(void);  /* stvec を設定し、タイマ割り込みを有効にする */
uint64_t trap_tick_count(void);  /* 起動以来の秒数 (klog の接頭辞用) */
uint64_t trap_uptime_ms(void);   /* 起動以来のミリ秒 (uptime 用、実分解能 100ms) */

#endif
