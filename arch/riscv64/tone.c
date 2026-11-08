/* tone.c -- I2S 実体 (K210) の検証用ユーザープログラム
 *
 * 440Hz のサイン波を 44100Hz / モノラルで 3 秒分
 * (132300 サンプル) オーディオ syscall (40〜42) へ
 * 書き、鳴らす。モノラルなのでドライバの左右複製の
 * 経路も通る。virt ではダミーが即座に全部受けるので
 * 一瞬で終わるが、K210 では再生のペーシングで実時間
 * (約 3 秒) かかるのが正しい動き。
 *
 * サイン波は double の漸化式 (s[n] = 2cosθ・s[n-1]
 * − s[n-2]) で作る。libm は無いので sinθ / cosθ は
 * 小さい θ のテイラー展開で近似する (θ ≈ 0.0627
 * なので十分な精度)。ビルドは fptest と同じ lp64d。
 * 振幅は控えめに 8000 (最大 32767)。
 *
 * 戻り値が期待と違えば "tone: FAIL" を出して
 * ステータス 1、全部済んだら "tone: done" で 0。
 * test-k210.py の K21 が done と dmesg の
 * "audio: i2s frames=132300" を照合する。
 */

#include <stdint.h>

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_AUDIO_OPEN 40
#define SYS_AUDIO_WRITE 41
#define SYS_AUDIO_CLOSE 42

#define RATE 44100
#define SECONDS 3
#define TOTAL_SAMPLES (RATE * SECONDS)  /* 132300 */
#define CHUNK_SAMPLES 2205              /* 50ms 分ずつ書く */
#define AMPLITUDE 8000.0

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

static void exit_(long status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

static void fail(const char *what)
{
    puts_("tone: FAIL ");
    puts_(what);
    putc_('\n');
    exit_(1);
}

static int16_t samples[CHUNK_SAMPLES];

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    long r;
    int sent;
    double theta, sin_t, cos_t, coef;
    double y_prev2, y_prev1;
    (void)argc;
    (void)argv;

    /* θ = 2π × 440 / 44100 ≈ 0.0627 */
    theta = 2.0 * 3.141592653589793 * 440.0 / (double)RATE;
    sin_t = theta - theta * theta * theta / 6.0;
    cos_t = 1.0 - theta * theta / 2.0 +
            theta * theta * theta * theta / 24.0;
    coef = 2.0 * cos_t;
    y_prev2 = 0.0;                    /* s[-2] 相当: s[0] = 0 */
    y_prev1 = AMPLITUDE * sin_t;      /* s[1] */

    puts_("tone: start\n");
    r = syscall3(SYS_AUDIO_OPEN, RATE, 1, 0);
    if (r != 0) {
        fail("open");
    }

    sent = 0;
    while (sent < TOTAL_SAMPLES) {
        int n = TOTAL_SAMPLES - sent;
        int i;
        if (n > CHUNK_SAMPLES) {
            n = CHUNK_SAMPLES;
        }
        for (i = 0; i < n; i++) {
            double y;
            if (sent + i == 0) {
                y = 0.0;
            } else if (sent + i == 1) {
                y = y_prev1;
            } else {
                y = coef * y_prev1 - y_prev2;
                y_prev2 = y_prev1;
                y_prev1 = y;
            }
            samples[i] = (int16_t)(y >= 0.0 ? y + 0.5 : y - 0.5);
        }
        r = syscall3(SYS_AUDIO_WRITE, (long)samples, n, 0);
        if (r != n) {
            fail("write");
        }
        sent += n;
    }

    r = syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
    if (r != 0) {
        fail("close");
    }
    puts_("tone: done\n");
    exit_(0);
}
