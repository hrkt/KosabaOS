#ifndef KOSABAOS_SAVER_H
#define KOSABAOS_SAVER_H

#include <stdint.h>

/* saver.h -- スクリーンセーバーのカーネル内 API。
 * 設計は doc/screensaver-design.md。
 *
 * OS の仕事は「無入力の検出」と「切り替えの合図」だけ
 * (doc/userland-design.md 決定 4)。表示はユーザー
 * プログラム saver.elf が担う。カーネルは無入力時間と
 * 環境 (通常のジョブが無いこと) を見て、ルートプログラム
 * (シェル) からの問い合わせに「起こしてよい」と返す。
 * シェルはそれを受けて saver.elf をジョブとして起こし、
 * 入力活動が戻ればカーネル側の状態が解ける。 */

/* 入力活動の記録。呼ぶのは 2 箇所だけ:
 *   - trap.c の SYS_GETC が文字を返したとき
 *   - kernel.c の console_getc が文字を返したとき
 * セーバー動作中なら、その場で状態を解く */
void saver_note_activity(void);

/* 時計タスク (proc.c) から毎秒呼ばれる。アイドル計測の
 * 起点を置くだけで、描画は一切しない */
void saver_tick(void);

/* syscall 52 の実体。無入力が閾値を超え、環境が合えば
 * 1 を返して要求を消費する (klog に "saver: on")。
 * それ以外は 0 */
int saver_query(void);

#endif
