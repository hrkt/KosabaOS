/* saver.c -- スクリーンセーバーの検出と切り替えの合図。
 * 設計は doc/screensaver-design.md。
 *
 * 置き時計として点けっぱなしにしたときの LCD の残像
 * 対策。OS の仕事は無入力の検出と切り替えだけに痩せ、
 * 表示はユーザープログラム saver.elf の仕事になった
 * (doc/userland-design.md 決定 4)。かつての二段構え
 * (帯の漂い・全面の大きな時刻) は、カーネルが時計表示を
 * 持っていた時代の形である。
 *
 * 入力活動は saver_note_activity が受ける (SYS_GETC の
 * 成功とカーネルコンソールの console_getc の成功の
 * 2 箇所)。アイドル計測の物差しは生の time CSR。trap.c
 * の tick は入力待ちで割り込みが滞ると一緒に滞るが、
 * セーバーが効くべき場面はまさにその入力待ちだから
 * である (スケジューラ段1 の sleep と同じ判断)。
 *
 * LCD の無い環境 (virt) でも状態機械は同じに動き、
 * 遷移の klog 行で両環境の検証を揃える。 */

#include <stdint.h>
#include "board.h"   /* BOARD_TIMER_HZ */
#include "saver.h"
#include "klog.h"    /* lkputs (遷移の要約行) */
#include "trap.h"    /* trap_saver_env */

/* 閾値 (無入力の秒数) */
#ifdef SAVER_TEST
#define SAVER_SEC 4             /* テスト用 (シナリオ AD) */
#else
#define SAVER_SEC 60
#endif

/* 生の時刻 (time CSR)。proc.c / trap.c と同じ小物 */
static uint64_t r_time(void)
{
    uint64_t x;
    asm volatile("csrr %0, time" : "=r"(x));
    return x;
}

static uint64_t now_sec(void)
{
    return r_time() / BOARD_TIMER_HZ;
}

static int active;             /* 要求を渡し、セーバー動作中 */
static uint64_t last_activity; /* 最後の入力活動 (now_sec の刻み) */
static int activity_seen;      /* 起動後の最初の tick で起点を置く */

void saver_note_activity(void)
{
    last_activity = now_sec();
    activity_seen = 1;
    if (active) {
        active = 0;
        lkputs("saver: off\n");
    }
}

void saver_tick(void)
{
    if (!activity_seen) {
        /* 起動直後の最初の起床で起点を置く (起動からの
         * 無入力も、そのままアイドルとして数える) */
        last_activity = now_sec();
        activity_seen = 1;
    }
}

int saver_query(void)
{
    uint64_t idle;

    if (active || !activity_seen) {
        return 0;
    }
    idle = now_sec() - last_activity;
    if (idle < SAVER_SEC) {
        return 0;
    }
    /* 環境: 通常のジョブが走っている間は起こさない
     * (帯の窓の持ち主 = 時計は trap_saver_env の側で
     * 数から外れている) */
    if (trap_saver_env() & TRAP_SAVER_JOBS_PRESENT) {
        return 0;
    }
    active = 1;
    lkputs("saver: on\n");
    return 1;
}
