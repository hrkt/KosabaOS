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

/* LCD は無い (ステータス表示はシリアルだけ)。 */
#define BOARD_FEATURE_LCD 0

/* ページフォルトは現行仕様どおり独立番号 (12/13/15) で届く */
#define BOARD_DRAFT_FAULT_CODES 0

/* vm.c の追加マップ用フックは無い (LCD 用 MMIO のような
 * ボード都合の割り当てが無いため)。 */
#define BOARD_FEATURE_VM_HOOK 0

/* vm.c の TLB フラッシュ: 現行仕様の sfence.vma */
#define BOARD_SFENCE_VMA() __asm__ volatile("sfence.vma zero, zero")

/* trap.c のタイマ: QEMU virt の CLINT は 10MHz */
#define BOARD_TIMER_HZ 10000000ULL

/* ユーザープログラムの物理配置 (vm.h が参照する)。
 * virt の RAM は 0x80000000 から 128MB あるので余裕がある。
 * カーネル (0x80200000〜) には 16MB 空けてある: FS の arena を
 * 8MB に広げたので、カーネルの BSS が大きくなるため。
 * データ領域は 4MB: IwaC のセルフホスト (コンパイル済み iwac の
 * 静的データ約 2.9MB) を載せるため (2026-11-03 に案A として決定)。
 * スタックの仮想アドレス末尾も合わせて 0x40800000 に移る。 */
#define BOARD_USER_CODE_PA   0x81200000UL
#define BOARD_USER_DATA_PA   0x81400000UL
#define BOARD_USER_STACK_PA  0x81800000UL
#define BOARD_CHILD_CODE_PA  0x81A00000UL
#define BOARD_CHILD_DATA_PA  0x81C00000UL
#define BOARD_CHILD_STACK_PA 0x82000000UL
#define BOARD_CODE_SIZE      (2ULL * 1024 * 1024)
#define BOARD_DATA_SIZE      (4ULL * 1024 * 1024)
#define BOARD_STACK_SIZE     (2ULL * 1024 * 1024)
/* スタック領域の仮想アドレス末尾 (データ領域の直後) */
#define BOARD_STACK_TOP_VA   0x40800000UL

/* RAM FS の大きさ (fs.c が参照する)。virt は IwaC のセルフホスト
 * (約 3MB の ELF を読み書きする) に合わせて広く取る:
 * arena 8MB / 作成時の予約 4MB (IwaC 側の検証で確認した値) */
#define BOARD_FS_ARENA_SIZE    (8ULL * 1024 * 1024)
#define BOARD_FS_CREATE_RESERVE (4ULL * 1024 * 1024)

#endif
