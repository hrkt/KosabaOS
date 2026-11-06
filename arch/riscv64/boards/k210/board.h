#ifndef KOSABAOS_BOARD_H
#define KOSABAOS_BOARD_H

/* board.h (k210) -- Sipeed Maix Dock (Kendryte K210) の特性。
 * 約束事は boards/virt/board.h の冒頭コメントを見よ。
 *
 * K210 の事情:
 *   - kflash -s で SRAM 0x80000000 に生バイナリが置かれ、M モードで
 *     直接ジャンプしてくる (ファームウェアの介在なし)。
 *   - シリアルは CH340 経由で UARTHS (16550 ではない、SiFive 系)。
 *   - 特権モードは step 4 の実機実験で「薄い M シム + S モード維持」
 *     に決定 (spike-k210/step4-usermode/)。M シム (mshim.c) が
 *     ページングの点火・SBI TIME・STIP 転送・rdtime を担い、
 *     カーネル本体は virt と同じ S モードのコードが動く。
 */

#define BOARD_NAME "k210"
#define BOARD_HELLO_SUFFIX "(k210)"
#define BOARD_FEATURE_SMODE_PATH 1

/* M モードのトラップがコンソールのタイマを兼ねる形 (step 3 まで)
 * は使わない。M はシムとして常駐するが、タイマ割り込みは
 * S 側の trap.c が SBI TIME 経由で受け取る。 */
#define BOARD_FEATURE_MMODE_TRAP 0

/* 付属 LCD (ST7789) へ起動の進み具合を表示するか。
 * ドライバは boards/k210/lcd.c、公開部分は lcd.h。
 * 経緯は doc/k210-lcd-driver.md。 */
#define BOARD_FEATURE_LCD 1

/* ページフォルトはドラフト仕様では一般のアクセスフォルト
 * (1/5/7) として届く (RustSBI-k210 の medeleg 選択と同じ事情) */
#define BOARD_DRAFT_FAULT_CODES 1

/* vm.c の追加マップ用フックを使う (LCD 用 MMIO をカーネルの
 * ページテーブルにも割り当てるため。vmhook.c)。 */
#define BOARD_FEATURE_VM_HOOK 1

/* vm.c の TLB フラッシュ: K210 に sfence.vma は無いので
 * ドラフト仕様の sfence.vm (x0 = 全フラッシュ) をワードで書く。
 * S モードで直接実行できることは step 4 の実験 3 で確認済み。 */
#define BOARD_SFENCE_VMA() __asm__ volatile(".word 0x10400073")

/* trap.c のタイマ: K210 の CLINT mtime は CPU の 1/50
 * (390MHz 時 約 7.8MHz、step 3 で実測) */
#define BOARD_TIMER_HZ 7800000ULL

/* ユーザープログラムの領域の大きさ (vm.h が参照する)。
 * 汎用 SRAM は 0x80000000 から 6MB。カーネル (1MB) の後ろは
 * 固定の領域セットではなく、下のページプールとして exec
 * 多段化の実需割り当てに回す。小さいプログラムの連鎖なら
 * 数段いける計算 (Nish 約 0.1MB + 子は数十 KB 級)。
 * iwac 本体級 (データ 1MB 超) は従来どおり入らない */
#define BOARD_CODE_SIZE      (1ULL * 1024 * 1024)
#define BOARD_DATA_SIZE      (1ULL * 1024 * 1024)
#define BOARD_STACK_SIZE     (512ULL * 1024)
/* スタック領域の仮想アドレス末尾 (virt と違い、こちらは従来どおり) */
#define BOARD_STACK_TOP_VA   0x40600000UL

/* ページアロケータ (page.c) のプール範囲。カーネルの後ろ
 * 0x80100000 から汎用 SRAM の末尾 0x80600000 まで (5MB)。
 * 旧来の親子 2 セット (各 2.5MB) を解体してプール化した */
#define BOARD_PAGE_POOL_BASE 0x80100000UL
#define BOARD_PAGE_POOL_END  0x80600000UL

/* RAM FS の大きさ (fs.c が参照する)。K210 は SRAM の都合で
 * 小さく (セルフホストは K210 の目標ではない)。ファイルは
 * ブロックチェーン式で書いた分だけ育つ (予約は無い) */
#define BOARD_FS_ARENA_SIZE    (256ULL * 1024)

/* vm.c から呼ばれるボード固有の追加マップ (vmhook.c) */
void board_vm_hook(void);

/* uart.c: sysctl 読み戻しによる CPU クロック (Hz) */
unsigned long board_cpu_freq(void);

#endif
