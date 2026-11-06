#ifndef KOSABAOS_VM_H
#define KOSABAOS_VM_H

#include <stdint.h>
#include "board.h"  /* 物理配置はボード層が決める */

/* exec の深さの sanity 上限 (暴走防止のための打ち止め)。
 * 実際の深さはページプールの残量で決まる (doc/exec-multilevel.md)。
 * trap.c の退避スタックと fd テーブル、ここの段ごとのページ台帳は
 * この上限で大きさを決める */
#define EXEC_MAX_DEPTH 16

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

/* ユーザースタックの末尾に引数を置く: 文字列群を末尾から積み、
 * その下に argv 配列 (文字列へのポインタ列 + NULL 終端) を置く。
 * stack_pa は置き先のスタック領域の物理アドレス (親用でも
 * exec の子用でもよい)。tokens はカーネル側の文字列 (argc 個、
 * 16 個まで)。戻り値は新しい sp (仮想アドレス) で、argv 配列の
 * 仮想アドレスが *argv_va に返る。プログラムの入口では
 * a0 = argc、a1 = argv になる (doc/syscalls.md の入口の約束) */
uint64_t user_stack_setup_argv(uint64_t stack_pa, int argc,
                               const char *const *tokens,
                               uint64_t *argv_va);

#endif
