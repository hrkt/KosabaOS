/* traplog.h -- M シムが検出したトラップの記録を
 * カーネルログへ移す口 (共通インタフェース)
 *
 * 実体はボード層: k210 は boards/k210/mshim.c (シムが
 * 記録し、この関数が drain する)、virt は
 * boards/virt/traplog.c (何もしない)。設計は
 * doc/trap-recovery-design.md。i2c.h と同じ受け方。
 */
#ifndef KOSABAOS_TRAPLOG_H
#define KOSABAOS_TRAPLOG_H

/* シムの記録領域に新しい記録があれば klog へ移す。
 * trap.c がトラップ入口で呼ぶ (割り込み・例外のどちらでも) */
void board_drain_trap_log(void);

#endif
