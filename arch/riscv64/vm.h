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

/* ユーザープログラムの置き場所 (仮想アドレス)。
 * コードは読み + 実行のみ (W^X)、データとスタックは
 * 読み書き可・実行不可に分ける。物理メモリは固定の領域では
 * なく、段ごとにページ単位で割り当てる (下のページ台帳) */
#define USER_CODE_VA       0x40000000UL  /* コード領域の仮想アドレス */
#define USER_DATA_VA       0x40200000UL  /* データ領域の仮想アドレス */
#define USER_STACK_TOP_VA  BOARD_STACK_TOP_VA  /* スタック領域の末尾 (ボード依存) */
#define USER_STACK_VA      (USER_STACK_TOP_VA - BOARD_STACK_SIZE)

/* 領域ごとの最大ページ数 (領域の大きさはボード依存) */
#define USER_CODE_PAGES    (BOARD_CODE_SIZE / 4096)
#define USER_DATA_PAGES    (BOARD_DATA_SIZE / 4096)
#define USER_STACK_PAGES   (BOARD_STACK_SIZE / 4096)

/* 段 (exec の深さ) ごとのページ台帳 (exec 多段化、doc/
 * exec-multilevel.md)。各領域について、仮想ページ 1 枚ごとの
 * 物理ページ (0 = 未割り当て) を並べたもの。exec のときは
 * ELF が実際に触るページだけをプール (page.c) から取り、
 * スタックは小さく始めて fault で伸ばす。プログラムが終われば
 * 台帳のページは全部プールへ返す。VA の配置は従来どおりで、
 * 変わるのは裏の物理メモリの付き方だけ */
struct user_pages {
    uint64_t code[USER_CODE_PAGES];
    uint64_t data[USER_DATA_PAGES];
    uint64_t stack[USER_STACK_PAGES];
};

struct user_pages *vm_pages(int level);  /* 段の台帳を返す */
void vm_pages_release(int level);  /* 台帳の全ページをプールへ返し、台帳を空にする */
void vm_apply_pages(int level);    /* 台帳をページ表へ反映し、その段を「現在」にする */

/* 台帳の中で仮想アドレスを物理アドレスに訳す
 * (未割り当てのページなら 0) */
uint64_t vm_pages_va_to_pa(const struct user_pages *up, uint64_t va);

/* スタック領域の末尾側 nbytes 分のページを用意する
 * (プログラム起動時の初期分)。失敗したら -1 */
int vm_stack_prepare(struct user_pages *up, uint64_t nbytes);

/* 現在の段について、va を含むスタックページが未割り当てなら
 * 割り当ててページ表にも写す。伸ばせたら 1、対象外 /
 * 既に写し済み / プール枯渇なら 0 (trap.c の fault 振り分け用) */
int vm_stack_grow(uint64_t va);

/* ボード固有のデバイス用追加マップ: 物理アドレス pa を含む
 * 2MB ブロックを VA = PA でカーネル専用 RW に割り当てる
 * (vm_init の中から board_vm_hook が使う) */
void vm_map_device(uint64_t pa);

/* ユーザーの仮想アドレスを、カーネルから触れる物理アドレスに
 * 訳す (現在の段で割り当てがないページなら 0) */
uint64_t user_va_to_pa(uint64_t va);

/* ユーザースタックの末尾に引数を置く: 文字列群を末尾から積み、
 * その下に argv 配列 (文字列へのポインタ列 + NULL 終端) を置く。
 * 置き先は台帳 up (どの段のプログラムでもよい)。tokens は
 * カーネル側の文字列 (argc 個、16 個まで)。戻り値は新しい sp
 * (仮想アドレス) で、argv 配列の仮想アドレスが *argv_va に返る。
 * プログラムの入口では a0 = argc、a1 = argv になる
 * (doc/syscalls.md の入口の約束)。置けなければ 0 */
uint64_t user_stack_setup_argv(struct user_pages *up, int argc,
                               const char *const *tokens,
                               uint64_t *argv_va);

#endif
