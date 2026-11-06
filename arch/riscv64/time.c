/* time.c -- 壁時計の共通層 (doc/time.md)
 *
 * 日時と epoch の変換は「civil 暦の日数」を経由する。
 * 400 年周期 (146097 日) で割り算すると、うるう年の
 * 規則 (4 年・100 年・400 年) が端数の形に全部収まる
 * ので、月ごとの日数表を引く必要が無くなる。 */

#include "time.h"

int time_is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month)
{
    static const int dim[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month == 2)
        return dim[month - 1] + (time_is_leap(year) ? 1 : 0);
    return dim[month - 1];
}

int time_valid(const struct datetime *dt)
{
    if (dt->year < 1970 || dt->year > 2199)
        return 0;
    if (dt->month < 1 || dt->month > 12)
        return 0;
    if (dt->day < 1 || dt->day > days_in_month(dt->year, dt->month))
        return 0;
    if (dt->hour < 0 || dt->hour > 23)
        return 0;
    if (dt->minute < 0 || dt->minute > 59)
        return 0;
    if (dt->second < 0 || dt->second > 59)
        return 0;
    return 1;
}

/* 年月日 -> 1970-01-01 から数えた日数。
 * 3 月を年の始まりにずらすと、うるう日 (2/29) が年の
 * 最後の日になり、式が素直になる */
static int64_t days_from_civil(int y, int m, int d)
{
    int64_t era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (int64_t)y - era * 400;                              /* 0〜399 */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;      /* 0〜365 */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;  /* 719468 = 0000-03-01 起点の補正 */
}

/* 日数 -> 年月日 (days_from_civil の逆) */
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    int64_t era, doe, yoe, yy, doy, mp;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;                                    /* 0〜146096 */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  /* 0〜399 */
    yy = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              /* 0〜365 */
    mp = (5 * doy + 2) / 153;                                  /* 0〜11 */
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);                  /* 1〜31 */
    *m = (int)(mp + (mp < 10 ? 3 : -9));                       /* 1〜12 */
    *y = (int)(yy + (*m <= 2));
}

uint64_t time_to_epoch(const struct datetime *dt)
{
    int64_t days = days_from_civil(dt->year, dt->month, dt->day);
    return (uint64_t)(days * 86400 + dt->hour * 3600 +
                      dt->minute * 60 + dt->second);
}

void time_from_epoch(uint64_t epoch, struct datetime *dt)
{
    int64_t days = (int64_t)(epoch / 86400);
    uint64_t rem = epoch % 86400;

    civil_from_days(days, &dt->year, &dt->month, &dt->day);
    dt->hour = (int)(rem / 3600);
    dt->minute = (int)((rem % 3600) / 60);
    dt->second = (int)(rem % 60);
}

int time_weekday(const struct datetime *dt)
{
    int64_t days = days_from_civil(dt->year, dt->month, dt->day);
    /* 1970-01-01 (日数 0) は木曜日 = 4 */
    return (int)((days + 4) % 7);
}

static void put2(char *p, int v)
{
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}

void time_format(const struct datetime *dt, char out[20])
{
    out[0] = (char)('0' + (dt->year / 1000) % 10);
    out[1] = (char)('0' + (dt->year / 100) % 10);
    out[2] = (char)('0' + (dt->year / 10) % 10);
    out[3] = (char)('0' + dt->year % 10);
    out[4] = '-';
    put2(out + 5, dt->month);
    out[7] = '-';
    put2(out + 8, dt->day);
    out[10] = ' ';
    put2(out + 11, dt->hour);
    out[13] = ':';
    put2(out + 14, dt->minute);
    out[16] = ':';
    put2(out + 17, dt->second);
    out[19] = 0;
}

/* 数字 1 文字分を読み進める。数字でなければ -1 */
static int take_digit(const char **sp, int *out)
{
    char c = **sp;
    if (c < '0' || c > '9')
        return -1;
    *out = *out * 10 + (c - '0');
    (*sp)++;
    return 0;
}

static int take_char(const char **sp, char want)
{
    if (**sp != want)
        return -1;
    (*sp)++;
    return 0;
}

static int take_num(const char **sp, int digits, int *out)
{
    int i;
    *out = 0;
    for (i = 0; i < digits; i++) {
        if (take_digit(sp, out) != 0)
            return -1;
    }
    return 0;
}

int time_parse(const char *s, struct datetime *dt)
{
    const char *p = s;

    if (take_num(&p, 4, &dt->year) != 0 ||
        take_char(&p, '-') != 0 ||
        take_num(&p, 2, &dt->month) != 0 ||
        take_char(&p, '-') != 0 ||
        take_num(&p, 2, &dt->day) != 0 ||
        take_char(&p, ' ') != 0 ||
        take_num(&p, 2, &dt->hour) != 0 ||
        take_char(&p, ':') != 0 ||
        take_num(&p, 2, &dt->minute) != 0 ||
        take_char(&p, ':') != 0 ||
        take_num(&p, 2, &dt->second) != 0 ||
        *p != 0)
        return -1;
    if (!time_valid(dt))
        return -1;
    return 0;
}

int time_read(struct datetime *dt)
{
    if (board_rtc_get(dt) != 0)
        return -1;
    if (!time_valid(dt))
        return -1;  /* 読めたが値が成り立っていない = 未設定とみなす */
    return 0;
}

int time_set(const struct datetime *dt)
{
    if (!time_valid(dt))
        return -1;
    return board_rtc_set(dt);
}
