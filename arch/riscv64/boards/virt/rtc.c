/* rtc.c -- QEMU virt の Goldfish RTC (doc/time.md)
 *
 * QEMU virt には Goldfish RTC が 0x101000 にあり、ホストの
 * 時刻を Unix epoch からのナノ秒で返す。0x101000 は
 * UART (0x10000000) と同じギガページ (恒等マッピングの
 * root[0]) の中なので、追加の割り当ては要らない。
 *
 *   +0x00 TIME_LOW : 下位 32 ビット。読むとその瞬間の
 *                     時刻がラッチされ、続けて読む
 *                     TIME_HIGH と一貫する
 *   +0x04 TIME_HIGH: 上位 32 ビット
 *
 * 時刻を書く口は無い (ホストが所有している) ので、
 * board_rtc_set は -1 を返す。 */

#include <stdint.h>
#include "time.h"

#define GOLDFISH_RTC_BASE 0x101000ULL
#define RTC_TIME_LOW      0x00
#define RTC_TIME_HIGH     0x04

static uint32_t rtc_reg(uint64_t off)
{
    return *(volatile uint32_t *)(GOLDFISH_RTC_BASE + off);
}

int board_rtc_get(struct datetime *dt)
{
    uint32_t lo = rtc_reg(RTC_TIME_LOW);   /* この読みでラッチされる */
    uint32_t hi = rtc_reg(RTC_TIME_HIGH);
    uint64_t ns = ((uint64_t)hi << 32) | lo;

    time_from_epoch(ns / 1000000000ULL, dt);
    return 0;
}

int board_rtc_set(const struct datetime *dt)
{
    (void)dt;
    return -1;  /* 読むだけの RTC */
}
