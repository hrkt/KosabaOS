/* atest.c -- オーディオ syscall (40〜42) の検証用ユーザープログラム
 *
 * ダミーデバイスに対する契約をひととおり確かめる:
 *   - 開く前の write / close は -1
 *   - チャンネル数が 1 でも 2 でもない open は -1
 *   - 開いている間の再 open は -1
 *   - サンプル数 0 の write は 0、範囲外ポインタの write は -1
 *   - 分けて書いた分が全部受け付けられる (300 + 300 + 400)
 *   - 閉じたあとの close は -1
 * 書くサンプルは決定的なパターン (i * 37 + 11 の下位 16bit、
 * 合計 1000 サンプル) で、ホスト側 (test.sh) が同じ列から
 * FNV-1a 64bit を計算し、dmesg に出る "audio: close" 行の
 * samples / fnv と照合する。
 *
 * 各段階が済むごとに "atest: ... ok" を出し、全部済んだら
 * "atest: done" を出してステータス 0 で終わる。途中で期待と
 * 違う戻り値が来たら "atest: FAIL" を出してステータス 1。
 */

#include <stdint.h>

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_AUDIO_OPEN 40
#define SYS_AUDIO_WRITE 41
#define SYS_AUDIO_CLOSE 42

#define TOTAL_SAMPLES 1000

static long syscall3(long num, long a0, long a1, long a2)
{
    register long r0 asm("a0") = a0;
    register long r1 asm("a1") = a1;
    register long r2 asm("a2") = a2;
    register long r7 asm("a7") = num;
    asm volatile("ecall"
                 : "+r"(r0)
                 : "r"(r1), "r"(r2), "r"(r7)
                 : "memory");
    return r0;
}

static void putc_(char c) { syscall3(SYS_PUTC, c, 0, 0); }

static void puts_(const char *s)
{
    while (*s != '\0') {
        putc_(*s);
        s++;
    }
}

static void put_long(long v)
{
    char buf[24];
    int n = 0;
    if (v < 0) {
        putc_('-');
        v = -v;
    }
    if (v == 0) {
        putc_('0');
        return;
    }
    while (v > 0) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        putc_(buf[--n]);
    }
}

static void exit_(long status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

static void fail(const char *what, long got)
{
    puts_("atest: FAIL ");
    puts_(what);
    putc_('=');
    put_long(got);
    putc_('\n');
    exit_(1);
}

static void expect(const char *what, long got, long want)
{
    if (got != want) {
        fail(what, got);
    }
}

static uint16_t samples[TOTAL_SAMPLES];

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    int i;
    long r;
    (void)argc;
    (void)argv;

    for (i = 0; i < TOTAL_SAMPLES; i++) {
        samples[i] = (uint16_t)(i * 37 + 11);
    }

    /* 開く前の write / close は -1、変なチャンネル数も -1 */
    r = syscall3(SYS_AUDIO_WRITE, (long)samples, 4, 0);
    expect("write-before-open", r, -1);
    r = syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
    expect("close-before-open", r, -1);
    r = syscall3(SYS_AUDIO_OPEN, 44100, 3, 0);
    expect("open-channels-3", r, -1);
    puts_("atest: errors ok\n");

    /* 開く。再 open は -1 */
    r = syscall3(SYS_AUDIO_OPEN, 44100, 1, 0);
    expect("open", r, 0);
    r = syscall3(SYS_AUDIO_OPEN, 44100, 1, 0);
    expect("reopen", r, -1);
    puts_("atest: open ok\n");

    /* 書く: 0 個は 0、範囲外は -1、本体は 300 + 300 + 400 */
    r = syscall3(SYS_AUDIO_WRITE, (long)samples, 0, 0);
    expect("write-0", r, 0);
    r = syscall3(SYS_AUDIO_WRITE, (long)0x60000000, 4, 0);
    expect("write-bad-pointer", r, -1);
    r = syscall3(SYS_AUDIO_WRITE, (long)(samples + 0), 300, 0);
    expect("write-1", r, 300);
    r = syscall3(SYS_AUDIO_WRITE, (long)(samples + 300), 300, 0);
    expect("write-2", r, 300);
    r = syscall3(SYS_AUDIO_WRITE, (long)(samples + 600), 400, 0);
    expect("write-3", r, 400);
    puts_("atest: writes ok\n");

    /* 閉じる。閉じたあとの close は -1 */
    r = syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
    expect("close", r, 0);
    r = syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
    expect("reclose", r, -1);
    puts_("atest: close ok\n");

    puts_("atest: done\n");
    exit_(0);
}
