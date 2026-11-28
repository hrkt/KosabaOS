/* vtcon.c -- LCD 側の端末エミュレータ (VT 週末版 W2)。
 * doc/syscalls.md 54/55 番。
 *
 * getty が時計と並ぶスタートアップジョブとして起こす。
 * セッションの出力の行き先 (54) を見回り、LCD (または
 * 両方) の間だけ全画面を取って (51)、LCD キューに溜まった
 * putc 出力を 55 で drain して文字グリッドに描く。
 * グリッドは 53 桁 x 30 行 (5x7 scale 1、1 字 6x8px)。
 * \n \r \b を処理し、ESC シーケンスは最終バイトまで
 * 読み飛ばす。満行でスクロールする。
 *
 * フォントは当面 gfx_text (46) のカーネル側 5x7 を使う
 * (clock.elf と同じ立場。libdashi のフォントマネージャが
 * 来たら乗り換える)。
 *
 * virt では LCD が見えないので、観測の口は「LCD 期間に
 * drain したバイト数」: serial に戻ったときに
 * "vtcon: drained <n>" を 1 行だけ出す (test.sh の AS)。
 * 実機での絵の確認は W5。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_SLEEP_MS 34
#define SYS_GFX_RECT 45
#define SYS_GFX_TEXT 46
#define SYS_GFX_LAYOUT 51
#define SYS_CONSOLE_SET_OUT 54
#define SYS_CONSOLE_READ 55

#define COL(r, g, b) \
    ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define BG COL(16, 16, 16)      /* 端末の地色 */
#define FG COL(220, 220, 220)   /* 文字色 */
#define CUR COL(255, 255, 255)  /* カーソル */

#define COLS 53
#define ROWS 30
#define CELL_W 6
#define CELL_H 8

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

static void put_dec(long v)
{
    char rev[24];
    int n = 0;
    if (v == 0) {
        putc_('0');
        return;
    }
    while (v > 0) {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        putc_(rev[--n]);
    }
}

static char grid[ROWS][COLS];
static int dirty[ROWS];
static int cur_row = 0;
static int cur_col = 0;
static int esc = 0;             /* ESC シーケンスの途中 */
static long drained = 0;        /* 今回の LCD 期間の読み出し数 */
static int claimed = 0;         /* 全画面を取っているか */
static int prev_row = 0;        /* カーソルがあった行 */
static int prev_col = 0;

static void mark_all_dirty(void)
{
    int r;
    for (r = 0; r < ROWS; r++) {
        dirty[r] = 1;
    }
}

static void scroll_up(void)
{
    int r, c;
    for (r = 0; r < ROWS - 1; r++) {
        for (c = 0; c < COLS; c++) {
            grid[r][c] = grid[r + 1][c];
        }
    }
    for (c = 0; c < COLS; c++) {
        grid[ROWS - 1][c] = ' ';
    }
    mark_all_dirty();
}

static void feed_char(char c)
{
    if (esc) {
        /* ESC シーケンス: 最終バイト (0x40〜0x7E) まで読む */
        if (c >= 0x40 && c <= 0x7E) {
            esc = 0;
        }
        return;
    }
    if (c == 0x1B) {
        esc = 1;
        return;
    }
    if (c == '\n') {
        cur_col = 0;
        cur_row++;
        if (cur_row >= ROWS) {
            scroll_up();
            cur_row = ROWS - 1;
        }
        dirty[cur_row] = 1;
        return;
    }
    if (c == '\r') {
        cur_col = 0;
        return;
    }
    if (c == 0x08 || c == 0x7F) {
        if (cur_col > 0) {
            cur_col--;
            grid[cur_row][cur_col] = ' ';
            dirty[cur_row] = 1;
        }
        return;
    }
    if (c >= 0x20 && c < 0x7F) {
        grid[cur_row][cur_col] = c;
        dirty[cur_row] = 1;
        cur_col++;
        if (cur_col >= COLS) {
            cur_col = 0;
            cur_row++;
            if (cur_row >= ROWS) {
                scroll_up();
                cur_row = ROWS - 1;
            }
        }
        return;
    }
    /* その他の制御文字は捨てる */
}

static void draw_row(int r)
{
    char line[COLS + 1];
    int c;
    for (c = 0; c < COLS; c++) {
        line[c] = grid[r][c];
    }
    line[COLS] = '\0';
    /* 空白は字形を描かないので、行全体を地色で消してから描く */
    syscall3(SYS_GFX_RECT, 0 | ((long)(r * CELL_H) << 16),
             (long)(COLS * CELL_W) | ((long)CELL_H << 16), BG);
    syscall3(SYS_GFX_TEXT, 0 | ((long)(r * CELL_H) << 16),
             (long)line, FG | (1 << 16));
}

static void draw_cursor(void)
{
    /* 下線ブロック (5x1) をカーソル位置に置く */
    syscall3(SYS_GFX_RECT,
             (long)(cur_col * CELL_W) | ((long)(cur_row * CELL_H + 7) << 16),
             (long)5 | ((long)1 << 16), CUR);
}

static void drain_and_draw(void)
{
    char buf[256];
    int i;
    long n;

    for (;;) {
        n = syscall3(SYS_CONSOLE_READ, (long)buf, 256, 0);
        if (n <= 0) {
            break;
        }
        drained += n;
        for (i = 0; i < n; i++) {
            feed_char(buf[i]);
        }
    }
    /* カーソルがいた行も描き直す (下線の残りを消すため) */
    dirty[prev_row] = 1;
    for (i = 0; i < ROWS; i++) {
        if (dirty[i]) {
            draw_row(i);
            dirty[i] = 0;
        }
    }
    draw_cursor();
    prev_row = cur_row;
    prev_col = cur_col;
}

__attribute__((section(".text.start")))
void _start(void)
{
    int r, c;
    long mode;

    for (r = 0; r < ROWS; r++) {
        for (c = 0; c < COLS; c++) {
            grid[r][c] = ' ';
        }
    }
    puts_("vtcon: start\n");
    for (;;) {
        mode = syscall3(SYS_CONSOLE_SET_OUT, -1, 0, 0);
        if (mode == 1 || mode == 2) {
            if (!claimed) {
                if (syscall3(SYS_GFX_LAYOUT, 2, 0, 0) == 0) {
                    claimed = 1;
                    mark_all_dirty();
                }
            }
            if (claimed) {
                drain_and_draw();
            }
        } else {
            if (claimed) {
                syscall3(SYS_GFX_LAYOUT, 0, 0, 0);
                claimed = 0;
            }
            if (drained > 0) {
                puts_("vtcon: drained ");
                put_dec(drained);
                puts_("\n");
                drained = 0;
            }
        }
        syscall3(SYS_SLEEP_MS, 100, 0, 0);
    }
}
