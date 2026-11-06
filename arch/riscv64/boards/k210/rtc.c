/* rtc.c -- K210 内蔵 RTC (doc/time.md)
 *
 * レジスタ配置と初期化の手順は Kendryte SDK の rtc
 * ドライバ (reference/kendryte-standalone-sdk) に従う。
 * KosabaOS 流に絞った違いは 2 つ:
 *   - ペリフェラルのリセットはしない (SDK の rtc_init は
 *     リセットから始める)。リセットを跨いで設定が残る
 *     かを実測するためと、温かい再起動で時刻が消えない
 *     方が都合がよいため
 *   - 初期化は最初に触る時まで遅らせる (遅延初期化)
 *
 * RTC は 0x50460000 で、SYSCTL (0x50440000) と同じ
 * 2MB ブロックの中なので、vmhook の既存の割り当てで
 * 届く。クロックは sysctl の APB1 側:
 *   clk_en_cent (0x50440028) の apb1_clk_en = bit4
 *   clk_en_peri (0x5044002C) の rtc_clk_en  = bit29
 * RTC の入力クロック IN0 は 26MHz で、秒あたりの
 * カウント数 (initial_count) にはこれを書く。 */

#include <stdint.h>
#include "time.h"
#include "klog.h"

#define RTC_BASE    0x50460000ULL
#define SYSCTL_BASE 0x50440000ULL

#define REG32(a) (*(volatile uint32_t *)(a))

/* RTC レジスタのオフセット */
#define RTC_DATE          0x00  /* week[2:0] day[12:8] month[19:16] year[31:20] */
#define RTC_TIME          0x04  /* second[15:10] minute[21:16] hour[28:24] */
#define RTC_INITIAL_COUNT 0x10
#define RTC_REGISTER_CTRL 0x1C
#define RTC_EXTENDED      0x28  /* century[4:0] leap_year[5] */

/* register_ctrl のフィールド */
#define CTRL_READ_EN   (1u << 0)
#define CTRL_WRITE_EN  (1u << 1)
#define CTRL_UNPROTECT (0xFEu << 13) | (0xFEu << 21) | \
                       (1u << 29) | (1u << 30)  /* mask 類を全部 1 = 書ける */

#define RTC_INPUT_HZ 26000000u  /* IN0 = 26MHz */

static int rtc_started = 0;

/* RTC 側の同期待ち。SDK はモード切り替えの前に
 * 1/26MHz 秒ほど CPU サイクルで待つ。S モードから
 * サイクルカウンタは読まない方針なので、十分に長い
 * スピンで代替する (390MHz でも数マイクロ秒の話) */
static void rtc_spin(void)
{
    volatile int i;
    for (i = 0; i < 2000; i++)
        ;
}

/* モード切り替え: 読む時は (1, 0)、書く時は (0, 1) */
static void rtc_set_mode(int read_en, int write_en)
{
    uint32_t v = REG32(RTC_BASE + RTC_REGISTER_CTRL);
    v &= ~(CTRL_READ_EN | CTRL_WRITE_EN);
    if (read_en)
        v |= CTRL_READ_EN;
    if (write_en)
        v |= CTRL_WRITE_EN;
    rtc_spin();
    REG32(RTC_BASE + RTC_REGISTER_CTRL) = v;
    rtc_spin();
}

static void rtc_ensure(void)
{
    uint32_t date, tme, ext, init, ctrl;

    if (rtc_started)
        return;

    /* 1. クロックを有効化する */
    REG32(SYSCTL_BASE + 0x28) |= (1u << 4);
    REG32(SYSCTL_BASE + 0x2C) |= (1u << 29);
    rtc_spin();

    /* 2. 見つけた状態を記録に残す (未設定時に何が
     *    読めるかの実測を兼ねる) */
    date = REG32(RTC_BASE + RTC_DATE);
    tme = REG32(RTC_BASE + RTC_TIME);
    ext = REG32(RTC_BASE + RTC_EXTENDED);
    init = REG32(RTC_BASE + RTC_INITIAL_COUNT);
    ctrl = REG32(RTC_BASE + RTC_REGISTER_CTRL);
    kputs("rtc: found date=");
    kput_hex(date);
    kputs(" time=");
    kput_hex(tme);
    kputs(" ext=");
    kput_hex(ext);
    kputs(" init_count=");
    kput_dec(init);
    kputs(" ctrl=");
    kput_hex(ctrl);
    kputc('\n');

    /* 3. 書き込み保護を外す */
    ctrl |= CTRL_UNPROTECT;
    REG32(RTC_BASE + RTC_REGISTER_CTRL) = ctrl;

    /* 4. 秒あたりのカウント数が 0 の時だけ IN0 を書く
     *    (設定済みの値は温かい再起動で残したい) */
    if (init == 0) {
        rtc_set_mode(0, 1);
        REG32(RTC_BASE + RTC_INITIAL_COUNT) = RTC_INPUT_HZ;
    }

    /* 5. RUNNING モードにする */
    rtc_set_mode(1, 0);
    rtc_started = 1;
    kputs("rtc: running\n");
}

int board_rtc_get(struct datetime *dt)
{
    uint32_t date, date2, tme, ext;

    rtc_ensure();

    date = REG32(RTC_BASE + RTC_DATE);
    tme = REG32(RTC_BASE + RTC_TIME);
    date2 = REG32(RTC_BASE + RTC_DATE);
    if (date != date2) {
        /* 日付を跨ぐ瞬間に読んだら、時刻を取り直す */
        date = date2;
        tme = REG32(RTC_BASE + RTC_TIME);
    }
    ext = REG32(RTC_BASE + RTC_EXTENDED);

    dt->year = (int)(ext & 0x1F) * 100 + (int)((date >> 20) & 0xFFF);
    dt->month = (int)((date >> 16) & 0xF);
    dt->day = (int)((date >> 8) & 0x1F);
    dt->hour = (int)((tme >> 24) & 0x1F);
    dt->minute = (int)((tme >> 16) & 0x3F);
    dt->second = (int)((tme >> 10) & 0x3F);
    return 0;
}

int board_rtc_set(const struct datetime *dt)
{
    uint32_t date, tme, ext;

    rtc_ensure();

    /* 年は century (5 ビット) と下 2 桁の合成で持つ */
    if (dt->year < 2000 || dt->year > 2199)
        return -1;

    date = ((uint32_t)time_weekday(dt) & 0x7)
         | ((uint32_t)dt->day << 8)
         | ((uint32_t)dt->month << 16)
         | ((uint32_t)(dt->year % 100) << 20);
    tme = ((uint32_t)dt->second << 10)
        | ((uint32_t)dt->minute << 16)
        | ((uint32_t)dt->hour << 24);
    ext = (uint32_t)(dt->year / 100)
        | (time_is_leap(dt->year) ? (1u << 5) : 0);

    rtc_set_mode(0, 1);  /* SETTING */
    REG32(RTC_BASE + RTC_DATE) = date;
    REG32(RTC_BASE + RTC_TIME) = tme;
    REG32(RTC_BASE + RTC_EXTENDED) = ext;
    rtc_set_mode(1, 0);  /* RUNNING */
    return 0;
}
