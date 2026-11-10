/* saver.c -- スクリーンセーバー (二段構え) の状態機械。
 * 設計は doc/screensaver-design.md。
 *
 * 置き時計として点けっぱなしにしたときの LCD の残像
 * 対策。判定と描画の駆動は時計タスク (proc.c の
 * clock_body) が毎秒呼ぶ saver_tick が担い、入力活動は
 * saver_note_activity が受ける (SYS_GETC の成功と
 * カーネルコンソールの console_getc の成功の 2 箇所)。
 *
 * アイドル計測の物差しは生の time CSR。trap.c の tick
 * は入力待ちで割り込みが滞ると一緒に滞るが、セーバー
 * が効くべき場面はまさにその入力待ちだからである
 * (スケジューラ段1 の sleep と同じ判断)。
 *
 * LCD の無い環境 (virt) では描画は一切起きず、遷移の
 * klog 行だけが残る (gfx の要約行と同じ流儀で、両環境
 * の検証を dmesg で揃える)。 */

#include <stdint.h>
#include "board.h"   /* BOARD_TIMER_HZ, BOARD_FEATURE_LCD */
#include "saver.h"
#include "klog.h"    /* lkputs (遷移の要約行) */
#include "trap.h"    /* trap_saver_env */
#if BOARD_FEATURE_LCD
#include "lcd.h"
#include "gfx.h"
#endif

/* 閾値 (無入力の秒数)。段2 は段1 からの合計 */
#ifdef SAVER_TEST
#define SAVER_STAGE1_SEC 4    /* テスト用 (シナリオ AD/K23) */
#define SAVER_STAGE2_SEC 10
#else
#define SAVER_STAGE1_SEC 60
#define SAVER_STAGE2_SEC 300
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

static int stage;              /* 0 = 切、1 = 帯内、2 = 全面 */
static uint64_t last_activity; /* 最後の入力活動 (now_sec の刻み) */
static int activity_seen;      /* 起動後の最初の tick で起点を置く */
static int console_prompt;     /* カーネルコンソールのキー待ち */
static uint64_t stage1_mark;   /* 段1 に入った秒 (漂いの歩測り用) */
static int drift_step;         /* 段1 のオフセットの歩数 */
static uint64_t stage2_mark;   /* 段2 に入った秒 */
static int s2_step;            /* 段2 の位置の歩数 */
static uint64_t disp_seconds;  /* 直近の表示用秒数 (復帰描画用) */

/* 往復する歩の位置 (0..half..0)。step を入れると
 * 0,1,..half,..1,0 と往復する値を返す */
static int pingpong(int step, int half)
{
    int period = half * 2;
    int s = step % period;
    return s <= half ? s : period - s;
}

/* 段1 のオフセット: 5 秒ごとに 1 歩。dx は 6px 刻みで
 * 0..54 を往復、dy は 0..3 をゆっくり往復する */
static void stage1_apply_offset(int step)
{
#if BOARD_FEATURE_LCD
    int dx = pingpong(step, 9) * 6;
    int dy = pingpong(step / 6, 3);
    lcd_tick_set_offset(dx, dy);
#else
    (void)step;
#endif
}

/* 段2 の描画位置: 4 秒ごとに 1 歩。scale 4 の 8 文字
 * (192px) が 320x216 の中で収まる範囲を往復する */
static void stage2_draw(uint64_t seconds, int step)
{
#if BOARD_FEATURE_LCD
    int x = pingpong(step, 14) * 9;        /* 0..126 */
    int y = pingpong(step / 4, 12) * 5;    /* 0..60 */
    lcd_saver2_draw(seconds, x, y);
#else
    (void)seconds;
    (void)step;
#endif
}

/* セーバーを終わらせる (活動があったとき)。復帰描画は
 * ここで同期的に行う: キー入力で起動されたプログラムが
 * 描くより前に画面を戻すため (設計書の判断) */
static void saver_off(void)
{
    int was = stage;
    stage = 0;
    lkputs("saver: off\n");
#if BOARD_FEATURE_LCD
    if (was == 1) {
        /* 帯は次の tick 描画が移動先を消して定位置に描く */
        lcd_tick_set_offset(0, 0);
    } else if (was == 2) {
        gfx_desktop_repaint();
        lcd_tick_repaint(disp_seconds);
    }
#else
    (void)was;
#endif
}

void saver_note_activity(void)
{
    last_activity = now_sec();
    activity_seen = 1;
    if (stage != 0) {
        saver_off();
    }
}

void saver_set_console_prompt(int on)
{
    console_prompt = on;
}

/* 段2 に入ってよい画面か (設計書の定義): ジョブが無く、
 * 前面がシェルの入力待ちか、カーネルコンソールがキー
 * 待ちのときだけ */
static int stage2_env_ok(void)
{
    int env = trap_saver_env();
    if (env & TRAP_SAVER_JOBS_PRESENT) {
        return 0;
    }
    if (env & TRAP_SAVER_FG_SHELL_WAIT) {
        return 1;
    }
    return console_prompt;
}

int saver_tick(uint64_t seconds)
{
    uint64_t now = now_sec();
    uint64_t idle;

    disp_seconds = seconds;
    if (!activity_seen) {
        /* 起動直後の最初の起床で起点を置く (起動からの
         * 無入力も、そのままアイドルとして数える) */
        last_activity = now;
        activity_seen = 1;
        return 1;
    }
    idle = now - last_activity;

    if (stage == 0) {
        if (idle >= SAVER_STAGE1_SEC) {
            stage = 1;
            stage1_mark = now;
            drift_step = 0;
            lkputs("saver: stage1 on\n");
            stage1_apply_offset(0);
        }
        return 1;
    }

    if (stage == 1) {
        int step = (int)((now - stage1_mark) / 5);
        if (step != drift_step) {
            drift_step = step;
            stage1_apply_offset(step);
        }
        if (idle >= SAVER_STAGE2_SEC && stage2_env_ok()) {
            stage = 2;
            stage2_mark = now;
            s2_step = 0;
            lkputs("saver: stage2 on\n");
#if BOARD_FEATURE_LCD
            lcd_tick_set_offset(0, 0);
            lcd_saver2_begin();
#endif
            stage2_draw(seconds, 0);
            return 0;
        }
        return 1;
    }

    /* 段2: 4 秒ごとに位置を 1 歩進め、毎秒描き直す
     * (秒の更新を兼ねる)。帯の通常描画は抑止する */
    {
        int step = (int)((now - stage2_mark) / 4);
        if (step != s2_step) {
            s2_step = step;
        }
        stage2_draw(seconds, s2_step);
    }
    return 0;
}
