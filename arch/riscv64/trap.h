#ifndef KOSABAOS_TRAP_H
#define KOSABAOS_TRAP_H

#include <stdint.h>

/* トラップ (例外・割り込み) の初期化。実装は trap.c、
 * 入口のアセンブラは trap.S。 */

void trap_init(void);  /* stvec を設定し、タイマ割り込みを有効にする */
uint64_t trap_tick_count(void);  /* 起動以来の秒数 (klog の接頭辞用) */
uint64_t trap_uptime_ms(void);   /* 起動以来のミリ秒 (uptime 用、実分解能 100ms) */

/* スクリーンセーバー (doc/screensaver-design.md) へ走者の
 * 状態を渡す読み口。段2 (全面) に入ってよいかの判定材料 */
#define TRAP_SAVER_JOBS_PRESENT  1  /* ジョブが 1 つでもある (回収待ち含む) */
#define TRAP_SAVER_FG_SHELL_WAIT 2  /* 前面がシェル (根の段) の入力待ち */
int trap_saver_env(void);

#endif
