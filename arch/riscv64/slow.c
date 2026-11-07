/* slow.c -- ジョブ制御 (スケジューラ段3) の検証用ユーザープログラム
 *
 * ゆっくり進むプログラムの役。uptime を見ながら約 300ms
 * ごとに "slow i/N" と出し、最後に "slow done" と出して
 * 終わる。バックグラウンドで走らせると、出力の合間に
 * シェルのプロンプトが戻り、他のコマンドが打てるはず
 * (test.sh シナリオ X、doc/jobs-design.md)。
 *
 *   run slow.elf [回数] [終了ステータス]
 *     回数の既定は 6、ステータスの既定は 0。
 *
 * 待ちは uptime (31) のポーリング (この OS の流儀)。
 * 実分解能が 100ms なので、間隔は大まかでよい。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_UPTIME 31

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

static void put_dec_(long v)
{
    char buf[20];
    int n = 0;

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

static int atoi_(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static void exit_(long status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

/* uptime が ms 進むまで待つ。値が固まったままの異常時に
 * 備え、回数の上限でも抜ける (壊れたらテスト側で気づく) */
static void wait_ms(long ms)
{
    long t0 = syscall3(SYS_UPTIME, 0, 0, 0);
    long i;
    for (i = 0; i < 20000000L; i++) {
        if (syscall3(SYS_UPTIME, 0, 0, 0) - t0 >= ms) {
            return;
        }
    }
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    long count = argc >= 2 ? atoi_(argv[1]) : 6;
    long status = argc >= 3 ? atoi_(argv[2]) : 0;
    long i;

    if (count < 1) {
        count = 1;
    }
    for (i = 1; i <= count; i++) {
        puts_("slow ");
        put_dec_(i);
        putc_('/');
        put_dec_(count);
        putc_('\n');
        wait_ms(300);
    }
    puts_("slow done\n");
    exit_(status);
}
