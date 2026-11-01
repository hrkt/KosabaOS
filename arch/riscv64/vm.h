#ifndef KOSABAOS_VM_H
#define KOSABAOS_VM_H

/* 仮想メモリ (Sv39 ページング)。実装は vm.c。 */

void vm_init(void);        /* ページテーブルを作り、変換を有効にする */
void vm_fault_demo(void);  /* わざと未マップ住所を触ってフォルトさせる */

/* ユーザープログラムの置き場所 (ステップ 4)。
 * 仮想アドレスは user.ld の約束、物理アドレスは RAM の中の
 * カーネルと重ならない場所。 */
#define USER_CODE_VA       0x40000000UL  /* コード (2MB megapage) */
#define USER_CODE_PA       0x80400000UL
#define USER_STACK_VA      0x40200000UL  /* スタック (2MB megapage) */
#define USER_STACK_PA      0x80600000UL
#define USER_STACK_TOP_VA  0x40400000UL  /* スタック領域の末尾 */

#endif
