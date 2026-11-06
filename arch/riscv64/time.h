#ifndef KOSABAOS_TIME_H
#define KOSABAOS_TIME_H

#include <stdint.h>

/* 壁時計 (RTC) の共通層 (doc/time.md)。
 *
 * ハードウェア RTC から「何時何分か」を読むための仕組みを
 * 3 層に分けた真ん中の層。ここにはボードに依存しない
 * ことだけを置く:
 *   - 日時構造体とその値域チェック (うるう年を含む)
 *   - Unix epoch (UTC) と日時の相互変換
 *   - "YYYY-MM-DD hh:mm:ss" の書式化とパース
 *   - カーネル内 API: time_read / time_set
 * ハードの生読み書きはボード層 (boards/<board>/rtc.c) が
 * 下の board_rtc_get / board_rtc_set で受ける。RTC の無い
 * ボードは -1 を返し、time_read がそのまま失敗になる。
 * syscall は今回足していない (消費者はカーネル自身)。 */

struct datetime {
    int year;    /* 1970〜2199 */
    int month;   /* 1〜12 */
    int day;     /* 1〜31 */
    int hour;    /* 0〜23 */
    int minute;  /* 0〜59 */
    int second;  /* 0〜59 */
};

/* カーネル内 API */
int time_read(struct datetime *dt);        /* 検証済みの読み。失敗は -1 */
int time_set(const struct datetime *dt);   /* 検証してボードへ書く。失敗は -1 */

/* 値域と変換 (ボード層からも使う) */
int      time_valid(const struct datetime *dt);
uint64_t time_to_epoch(const struct datetime *dt);  /* 日時 -> epoch 秒 (UTC) */
void     time_from_epoch(uint64_t epoch, struct datetime *dt);
int      time_weekday(const struct datetime *dt);   /* 0 = 日曜 〜 6 = 土曜 */
int      time_is_leap(int year);

/* 書式化とパース。out は "YYYY-MM-DD hh:mm:ss" + NUL で 20 バイト */
void time_format(const struct datetime *dt, char out[20]);
int  time_parse(const char *s, struct datetime *dt);  /* 失敗は -1 */

/* ボード層の口 (boards/<board>/rtc.c が実装する)。
 * 成功 0、RTC が無い・書けない等は -1 */
int board_rtc_get(struct datetime *dt);
int board_rtc_set(const struct datetime *dt);

#endif
