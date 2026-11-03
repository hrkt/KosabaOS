#ifndef KOSABAOS_VM_H
#define KOSABAOS_VM_H

#include <stdint.h>
#include "board.h"  /* 物理配置はボード層が決める */

/* ページングの初期化 (kernel.c から起動時に 1 回呼ぶ) */
void vm_init(void);

/* 未マップ領域を読んでページフォルトを見せるデモ */
void vm_fault_demo(void);

/* ユーザープログラムの置き場所 (仮想 → 物理)。
 * コードは読み + 実行のみ (W^X)、データとスタックは
 * 読み書き可・実行不可に分ける */
#define USER_CODE_VA       0x40000000UL  /* コード領域の仮想アドレス */
#define USER_CODE_PA       BOARD_USER_CODE_PA
#define USER_DATA_VA       0x40200000UL  /* データ領域の仮想アドレス */
#define USER_DATA_PA       BOARD_USER_DATA_PA
#define USER_STACK_TOP_VA  BOARD_STACK_TOP_VA  /* スタック領域の末尾 (ボード依存) */
#define USER_STACK_VA      (USER_STACK_TOP_VA - BOARD_STACK_SIZE)
#define USER_STACK_PA      BOARD_USER_STACK_PA

/* exec で子プログラムを置く予備の領域 (仮想アドレスは親と
 * 同じ場所を差し替えて使う) */
#define CHILD_CODE_PA      BOARD_CHILD_CODE_PA
#define CHILD_DATA_PA      BOARD_CHILD_DATA_PA
#define CHILD_STACK_PA     BOARD_CHILD_STACK_PA

/* ボード固有のデバイス用追加マップ: 物理アドレス pa を含む
 * 2MB ブロックを VA = PA でカーネル専用 RW に割り当てる
 * (vm_init の中から board_vm_hook が使う) */
void vm_map_device(uint64_t pa);

/* ユーザー領域が今どの物理メモリを向いているかを差し替える
 * (exec の親子切り替え用) */
void vm_switch_user_region(uint64_t code_pa, uint64_t data_pa,
                           uint64_t stack_pa);
uint64_t vm_current_code_pa(void);
uint64_t vm_current_data_pa(void);
uint64_t vm_current_stack_pa(void);

/* ユーザーの仮想アドレスを、カーネルから触れる物理アドレスに
 * 訳す (現在の割り当ての範囲外なら 0) */
uint64_t user_va_to_pa(uint64_t va);

#endif
