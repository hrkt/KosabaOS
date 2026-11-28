/* getty.c -- コンソールの番人 (ユーザープログラム)。
 * doc/userland-design.md 決定 7 (段4)。
 *
 * カーネルが最初に起こすユーザープログラム。仕事は 2 つ:
 *   1. スタートアップ: 常駐プログラム (時計 clock.elf と
 *      LCD 側の端末 vtcon.elf) をジョブとして起こす (決定 5)。
 *      あわせて、SD に /sd/VTFRONT があればセッションの出力
 *      先を最初から「両方」にして、液晶を前面として Nish を
 *      起こす (VT 週末版 W3)。行き先が両方ならシリアルも
 *      保守路として生き続ける。マーカーが無ければ従来どおり
 *      シリアルだけで、切替は Nish の vt builtin で行う
 *   2. シェル Nish を前面で exec し、終わるまで見届ける
 *
 * Nish の終わり方で振る舞いが変わる:
 *   - 正常終了 (exit): getty も一緒に終わり、カーネル
 *     コンソールへ落ちる (個人 OS の利便性のため当面は
 *     この形。ジョブは根の終了と一緒に片付けられる)
 *   - フォルト (-2): Nish だけを起こし直す。常駐ジョブは
 *     生き残る。3 回続けて落ちたら諦めてカーネル
 *     コンソールへ落ちる
 * 将来、必要が出てきたら「正常終了でも起こし直す」形
 * (Unix の getty) へ変える余地を残す。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_OPEN 4
#define SYS_CLOSE 6
#define SYS_EXEC 8
#define SYS_JOB_RUN 20
#define SYS_SLEEP_MS 34
#define SYS_CONSOLE_SET_OUT 54

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

__attribute__((section(".text.start")))
void _start(void)
{
    int faults = 0;

    puts_("getty: start\n");
    /* スタートアップ (決定 5)。時計と LCD 側の端末エミュレータ */
    syscall3(SYS_JOB_RUN, (long)"clock.elf", (long)"clock.elf", 0);
    syscall3(SYS_JOB_RUN, (long)"vtcon.elf", (long)"vtcon.elf", 0);
    /* W3: マーカーがあれば液晶を前面にしてから Nish を起こす */
    {
        long fd = syscall3(SYS_OPEN, (long)"/sd/VTFRONT", 0, 0);
        if (fd >= 0) {
            syscall3(SYS_CLOSE, fd, 0, 0);
            syscall3(SYS_CONSOLE_SET_OUT, 2, 0, 0);
            puts_("getty: lcd front (/sd/VTFRONT)\n");
        }
    }
    for (;;) {
        long r = syscall3(SYS_EXEC, (long)"nish.elf",
                          (long)"nish.elf", 0);
        if (r == -2) {
            /* Nish がフォルトで落ちた: 起こし直す */
            faults++;
            if (faults >= 3) {
                puts_("getty: nish keeps faulting, giving up\n");
                exit_(1);
            }
            puts_("getty: nish faulted, restarting\n");
            syscall3(SYS_SLEEP_MS, 500, 0, 0);
            continue;
        }
        if (r < 0) {
            puts_("getty: cannot start nish.elf\n");
            exit_(1);
        }
        /* 正常終了: getty も終わってカーネルコンソールへ */
        exit_(r);
    }
}
