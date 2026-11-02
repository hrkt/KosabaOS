#ifndef KOSABAOS_BOARD_H
#define KOSABAOS_BOARD_H

/* board.h (k210) -- Sipeed Maix Dock (Kendryte K210) の特性。
 * 約束事は boards/virt/board.h の冒頭コメントを見よ。
 *
 * K210 の事情:
 *   - kflash -s で SRAM 0x80000000 に生バイナリが置かれ、M モードで
 *     直接ジャンプしてくる (ファームウェアの介在なし)。
 *   - シリアルは CH340 経由で UARTHS (16550 ではない、SiFive 系)。
 *   - S モード前提の仕組みはまだ無い。ページング (ドラフト Sv39) と
 *     特権モードの扱いは移植ステップ 4 で実験して決めるため、
 *     当面 BOARD_FEATURE_SMODE_PATH は 0 (起動メッセージと echo まで)。
 */

#define BOARD_NAME "k210"
#define BOARD_HELLO_SUFFIX "(k210)"
#define BOARD_FEATURE_SMODE_PATH 0

/* M モードのトラップ (CLINT タイマ割り込み) が使えるか。
 * K210 はファームウェアなしで M のまま動くので、タイマは
 * SBI ではなく CLINT の mtimecmp を直接叩く (mtrap.c)。 */
#define BOARD_FEATURE_MMODE_TRAP 1

/* mtrap.c: M モードトラップとタイマ割り込みの初期化 */
void mtrap_init(void);

/* uart.c: sysctl 読み戻しによる CPU クロック (Hz) */
unsigned long board_cpu_freq(void);

#endif
