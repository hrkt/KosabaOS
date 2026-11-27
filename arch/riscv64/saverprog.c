/* saverprog.c -- スクリーンセーバー (ユーザープログラム)。
 * 埋め込み名は saver.elf。doc/screensaver-design.md、
 * doc/userland-design.md 決定 4。
 *
 * OS は無入力の検出と切り替えだけを持ち、表示はこの
 * プログラムの仕事。シェルが saver_query (52) の合図で
 * ジョブとして起こし、job_wait で前面に渡す。起動すると
 * gfx_layout (51) で全画面を要求し、暗転させてブロック
 * を 1 つゆっくり漂わせる (残像対策なので静止させない)。
 * キーが押されたら、そのキーはカーネル側の入力活動に
 * 記録され (セーバー状態が解ける)、このプログラムは
 * 終了してシェルに制御が戻る。
 */

#define SYS_PUTC 1
#define SYS_GETC 2
#define SYS_EXIT 3
#define SYS_GFX_CLEAR 44
#define SYS_GFX_RECT 45
#define SYS_GFX_LAYOUT 51

#define COL(r, g, b) \
    ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define BG COL(0, 0, 0)         /* 暗転 */
#define BLOCK COL(48, 48, 48)   /* 漂うブロック (暗い灰) */

#define WIN_W 320
#define WIN_H 240
#define BLOCK_SZ 24

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

/* 往復する歩の位置 (0..half..0) */
static int pingpong(int step, int half)
{
    int period = half * 2;
    int s = step % period;
    return s <= half ? s : period - s;
}

static void rect(int x, int y, int w, int h, long color)
{
    syscall3(SYS_GFX_RECT, x | (y << 16), w | (h << 16), color);
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    int step = 0;
    int bx = -1, by = -1;

    (void)argc;
    (void)argv;
    if (syscall3(SYS_GFX_LAYOUT, 2, 0, 0) != 0) {
        puts_("saver: fullscreen busy\n");
        exit_(1);
    }
    puts_("saver: start\n");
    syscall3(SYS_GFX_CLEAR, BG, 0, 0);
    for (;;) {
        long c;
        int nx = pingpong(step, 37) * 8;        /* 0..296 */
        int ny = pingpong(step / 3, 27) * 8;    /* 0..216 */

        if (nx != bx || ny != by) {
            if (bx >= 0) {
                rect(bx, by, BLOCK_SZ, BLOCK_SZ, BG);
            }
            rect(nx, ny, BLOCK_SZ, BLOCK_SZ, BLOCK);
            bx = nx;
            by = ny;
        }
        /* 250ms ごとにキーを見る。キーが来たら起きる */
        c = syscall3(SYS_GETC, 0, 250, 0);
        if (c >= 0) {
            exit_(0);
        }
        step++;
    }
}
