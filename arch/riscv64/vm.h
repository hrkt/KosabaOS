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

/* exec で子プログラムを置く予備の領域 (仮想アドレスは親と
 * 同じ場所を差し替えて使う) */
#define CHILD_CODE_PA      0x80800000UL
#define CHILD_STACK_PA     0x80A00000UL

/* ユーザー領域が今どの物理メモリを向いているかを差し替える
 * (exec の親子切り替え用) */
void vm_switch_user_region(uint64_t code_pa, uint64_t stack_pa);
uint64_t vm_current_code_pa(void);
uint64_t vm_current_stack_pa(void);

/* ユーザーの仮想アドレスを、カーネルから触れる物理アドレスに
 * 訳す (現在の割り当ての範囲外なら 0) */
uint64_t user_va_to_pa(uint64_t va);

#endif
