#ifndef KOSABAOS_PAGE_H
#define KOSABAOS_PAGE_H

#include <stdint.h>

/* 物理ページのアロケータ (page.c)。
 *
 * RAM のうち「誰のものでもない」範囲 (プール) を 4KB ページ
 * 単位で貸し出す部品。プールの範囲はボードが決める
 * (board.h の BOARD_PAGE_POOL_BASE / END)。借りたページは
 * ゼロで埋めて渡す (ページテーブルや BSS の前提に合う)。
 *
 * 今のところ利用者はデモと将来の exec 多段化
 * (doc/exec-multilevel.md: ユーザープログラムの実需割り当て)。
 * vm.c のページテーブル自体は、起動の最初期に要るため
 * 従来どおり静的な置き場所 (pt_pool) を使う
 */

void page_init(void);            /* 起動時に 1 回 (プールを全部空きに) */
uint64_t page_alloc(void);       /* 1 枚借りる。物理アドレス、無ければ 0 */
void page_free(uint64_t pa);     /* 1 枚返す (範囲外と二重解放は無視) */
uint64_t page_free_count(void);  /* 空きページ数 */
uint64_t page_total_count(void); /* プールの全ページ数 */
void page_demo(void);            /* カーネルコンソールのデモ ('p') */

#endif
