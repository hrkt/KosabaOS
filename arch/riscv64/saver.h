#ifndef KOSABAOS_SAVER_H
#define KOSABAOS_SAVER_H

#include <stdint.h>

/* saver.h -- スクリーンセーバー (二段構え) のカーネル内 API。
 * 設計は doc/screensaver-design.md。
 *
 * 段1: 無入力が続くと時計帯の中で時刻表示がゆっくり
 * 移動する (窓には触らない)。段2: さらに無入力が続き、
 * かつ何も走っていないときだけ、全面に大きな時刻が
 * 漂う。入力活動があれば即座に終わる。 */

/* 入力活動の記録。呼ぶのは 2 箇所だけ:
 *   - trap.c の SYS_GETC が文字を返したとき
 *   - kernel.c の console_getc が文字を返したとき
 * セーバーが入っていれば、その場で終了し、復帰描画
 * まで行う (遅らせると、キーで起動されたプログラムの
 * 絵を復帰描画が消しかねないため) */
void saver_note_activity(void);

/* 時計タスク (proc.c の clock_body) から毎秒呼ばれる。
 * seconds は lcd_tick_draw と同じ表示用の秒数 (起動
 * 秒数)。戻り値 1 = 通常の帯描画をしてよい、0 = 段2 の
 * 間なので帯描画を抑止すること */
int saver_tick(uint64_t seconds);

/* カーネルコンソールのトップレベルのキー待ちの間だけ
 * kernel.c が 1 を立てる (段2 の条件の片方。行入力や
 * コマンド実行中は立てない) */
void saver_set_console_prompt(int on);

#endif
