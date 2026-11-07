/* klog.c -- カーネルログ (リングバッファ)
 *
 * カーネル自身の出来事 (起動時の基本情報、exec の失敗理由、
 * プログラムの異常終了、SD のマウントなど) を、固定長の
 * リングバッファにテキストで残す部品。Linux の printk が
 * リングバッファに溜めて dmesg で読ませる形の、小さい版。
 * 仕様は doc/syscalls.md のカーネルログの節。
 *
 * 大きさは 16KB: 1 行 50 バイト前後として約 300 行ぶん。
 * 起動から一連の作業までの出来事は十分に収まり、K210 でも
 * カーネルの置き場 (プールの手前の 1MB) に無理なく入る。
 * いっぱいになったら古い分から上書きする。
 *
 * 位置の数え方: 「起動以来の通し位置」で数える。絶対位置 p の
 * バイトは buf[p % KLOG_SIZE] にある。読み出し (klog_read) は
 * 絶対位置で指定し、上書き済みの位置は最古の残存位置に丸める。
 *
 * 行頭の接頭辞: 改行の次の 1 文字目を書くとき、[秒] を先に
 * 残す (秒 = trap.c の tick、起動以来の秒数)。接頭辞はログの
 * 中だけで、コンソールの見た目は従来と変えない。
 *
 * 並行性: ログを書くのはカーネルの文脈だけ (タイマ割り込みの
 * tick 表示はコンソール専用で、ここを通らない) なので、
 * ロックは持たない。
 */

#include <stdint.h>
#include "klog.h"
#include "uart.h"
#include "trap.h"  /* trap_tick_count: 起動以来の秒数 */

#define KLOG_SIZE 16384

static char klog_buf[KLOG_SIZE];
static uint64_t klog_written = 0;   /* 起動以来に書いた総バイト数 */
static int at_line_start = 1;       /* 次の 1 文字が行頭かどうか */

/* リングへ 1 バイト書く (接頭辞の判定はしない、生の書き込み) */
static void ring_putc(char c)
{
    klog_buf[klog_written % KLOG_SIZE] = c;
    klog_written++;
}

static void ring_put_dec(uint64_t x)
{
    char buf[20];
    int n = 0;

    if (x == 0) {
        ring_putc('0');
        return;
    }
    while (x > 0) {
        buf[n++] = (char)('0' + (x % 10));
        x /= 10;
    }
    while (n > 0) {
        ring_putc(buf[--n]);
    }
}

/* 1 文字を残す。console が 1 なら UART にも同時に出す */
static void log_char(char c, int console)
{
    if (at_line_start) {
        /* 行頭: [秒] の接頭辞をログにだけ先に入れる */
        ring_putc('[');
        ring_put_dec(trap_tick_count());
        ring_putc(']');
        ring_putc(' ');
        at_line_start = 0;
    }
    ring_putc(c);
    if (console) {
        uart_putc(c);  /* コンソールにも同時に出す (見え方は不変) */
    }
    if (c == '\n') {
        at_line_start = 1;
    }
}

void kputc(char c)
{
    log_char(c, 1);
}

void kputs(const char *s)
{
    while (*s != '\0') {
        kputc(*s);
        s++;
    }
}

/* ログ専用の記録口 (コンソールには出さない)。毎秒の定期記録
 * のように、画面に出すと邪魔だが dmesg で追いたいもの用
 * (時計タスクの起床記録など、スケジューラ段2) */
void lkputc(char c)
{
    log_char(c, 0);
}

void lkputs(const char *s)
{
    while (*s != '\0') {
        lkputc(*s);
        s++;
    }
}

void lkput_dec(uint64_t x)
{
    char buf[20];
    int n = 0;

    if (x == 0) {
        lkputc('0');
        return;
    }
    while (x > 0) {
        buf[n++] = (char)('0' + (x % 10));
        x /= 10;
    }
    while (n > 0) {
        lkputc(buf[--n]);
    }
}

void kput_dec(uint64_t x)
{
    char buf[20];
    int n = 0;

    if (x == 0) {
        kputc('0');
        return;
    }
    while (x > 0) {
        buf[n++] = (char)('0' + (x % 10));
        x /= 10;
    }
    while (n > 0) {
        kputc(buf[--n]);
    }
}

void kput_hex(uint64_t x)
{
    int i;
    kputs("0x");
    for (i = 60; i >= 0; i -= 4) {
        int d = (int)((x >> i) & 0xF);
        kputc((char)(d < 10 ? '0' + d : 'a' + (d - 10)));
    }
}

uint64_t klog_total(void)
{
    return klog_written;
}

uint64_t klog_oldest(void)
{
    return klog_written > KLOG_SIZE ? klog_written - KLOG_SIZE : 0;
}

uint64_t klog_read(uint64_t offset, char *dst, uint64_t max)
{
    uint64_t oldest = klog_oldest();
    uint64_t n, i;

    if (offset < oldest) {
        offset = oldest;  /* 古い分は上書きされて消えている */
    }
    if (offset >= klog_written) {
        return 0;  /* その先はまだ無い */
    }
    n = klog_written - offset;
    if (n > max) {
        n = max;
    }
    for (i = 0; i < n; i++) {
        dst[i] = klog_buf[(offset + i) % KLOG_SIZE];
    }
    return n;
}
