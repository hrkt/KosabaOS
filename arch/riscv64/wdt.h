/* wdt.h -- ウォッチドッグの共通インタフェース
 *
 * 実体はボード層: k210 は boards/k210/wdt.c (WDT0)、
 * virt は boards/virt/wdt.c (何もしない)。設計は
 * doc/trap-recovery-design.md の段3。i2c.h / traplog.h
 * と同じ受け方。
 */
#ifndef KOSABAOS_WDT_H
#define KOSABAOS_WDT_H

/* ウォッチドッグを有効化する。スケジューラ開始後、
 * 時計タスクが回り始めてから呼ぶ (kernel.c) */
void board_wdt_start(void);

/* 餌やり。時計タスクの起床 (約 1 秒周期) から呼ぶ。
 * これが止まるとタイムアウト後に基板がリセットされる */
void board_wdt_feed(void);

#endif
