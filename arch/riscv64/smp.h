/* smp.h -- hart1 (2 つ目のハート) の起こしと状態
 * (doc/smp-design.md 段1)
 *
 * 段1 は「起こして、生きていることを確かめる」だけの
 * スパイク。hart1 は割り込みを受けず、自分のスタックと
 * このファイルのメールボックスだけに触る。スケジューラ
 * や FS、klog には触らない (複数書き手の前提が無い)。
 * 報告はメールボックスに置き、hart0 が読んで出す。
 */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>

/* ハート別状態。tp が指す (段1 では hart1 だけが使う) */
struct hart_state {
    uint64_t hartid;
    uint64_t stack_top;
};

extern struct hart_state hart1_state;

/* メールボックス (hart1 が書き、hart0 が読む) */
extern volatile uint64_t smp_heartbeat;   /* hart1 が増やし続ける */
extern volatile uint64_t smp_hart1_alive; /* hart1_main に入ったら 1 */
extern volatile uint64_t smp_hart1_fault; /* 事故時の scause (0 = 無し) */

/* hart0 側: ボードの方法で hart1 を起こし、結果を
 * klog に 1 行残す */
void smp_start(void);

/* 自ハートの D キャッシュを流し読みで追い出す (K210 の
 * ハート間受け渡しの作法。virt では無害) */
void smp_evict_cache(void);

/* hart1 側の入口 (start.S の hart1_entry から呼ばれる)。
 * 生存印を立て、ハートビートを回し続ける */
void hart1_main(uint64_t hartid);

/* ボード層: hart1 を起こす。0 = 起きた (または起動を
 * 仕掛けた)、負 = 起こせなかった理由 (SBI のエラー等)。
 * virt: SBI HSM hart_start。K210: M シム側の解放ワード */
int board_start_hart1(void);

/* 段1 の切り分け用: hart1 の進み具合をキャッシュの
 * 効かない場所 (K210 は CLINT の mtimecmp1) に印として
 * 書く / 読む。virt では何もしない (印は常に 0) */
void board_hart1_mark(uint64_t mark);
uint64_t board_hart1_mark_read(void);

#endif
