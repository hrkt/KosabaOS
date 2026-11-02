#ifndef KOSABAOS_BOARD_H
#define KOSABAOS_BOARD_H

/* board.h (virt) -- QEMU virt ボードの特性を共通コードへ伝える。
 *
 * ボード層の約束: ボードごとに違うもの (入口の特権モード、UART の
 * 種類と住所、配置アドレス) は boards/<名前>/ の中に閉じ込め、
 * 共通コード (kernel.c など) はこの board.h の宣言だけを見る。
 * 新しいボードは boards/ にフォルダを足して BOARD=<名前> で選ぶ。
 */

/* 起動メッセージに出すボード名と特権モードの印 */
#define BOARD_NAME "virt"
#define BOARD_HELLO_SUFFIX "(S-mode)"

/* S モード前提の仕組み (SBI タイマ、Sv39 ページング、ユーザーモード
 * 実行) が使えるか。virt では OpenSBI が S モードで渡してくるので 1。
 * 使えないボードでは 0 にして、kmain がその初期化と起動を飛ばす。 */
#define BOARD_FEATURE_SMODE_PATH 1

/* M モードのトラップは使わない (virt では SBI + S モードの trap)。 */
#define BOARD_FEATURE_MMODE_TRAP 0

#endif
