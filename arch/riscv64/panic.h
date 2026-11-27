/* panic.h -- カーネルが例外で止まるときの画面表示の口
 * (共通インタフェース)
 *
 * 実体はボード層: k210 は boards/k210/lcd.c (LCD に最小
 * フォントで PANIC と scause / sepc / stval を描く)、
 * virt は boards/virt/traplog.c (画面が無いので何もしない)。
 * 設計は doc/userland-design.md 決定 3 の例外 (クラッシュ
 * 表示専用の小さなフォントを 1 つだけカーネルに残す)。
 * traplog.h と同じ受け方。
 */
#ifndef KOSABAOS_PANIC_H
#define KOSABAOS_PANIC_H

#include <stdint.h>

/* カーネル例外の停止直前に 1 回呼ばれる。表示できない
 * ボードでは何もしなくてよい */
void board_panic_display(uint64_t scause, uint64_t sepc,
                         uint64_t stval);

#endif
