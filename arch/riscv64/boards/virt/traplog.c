/* traplog.c (virt) -- トラップ記録 drain のダミー (ボード層)
 *
 * virt には M シムが無い (OpenSBI が下にいる) ので記録は
 * 生まれない。共通層 (trap.c) が無条件に呼べるよう、
 * 空の実装を置く。設計は doc/trap-recovery-design.md。
 */

#include "traplog.h"
#include "panic.h"

void board_drain_trap_log(void)
{
}

/* virt には画面が無いので、クラッシュ表示も何もしない
 * (panic.h。実体は k210 の lcd.c) */
void board_panic_display(uint64_t scause, uint64_t sepc,
                         uint64_t stval)
{
    (void)scause;
    (void)sepc;
    (void)stval;
}
