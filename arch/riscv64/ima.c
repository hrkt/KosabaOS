/* ima.c -- 「今やってること」を LCD の前面窓に大きく出す
 * ユーザープログラム (doc/ima-design.md)
 *
 *   run ima <単語> [単語...]
 *
 * 引数の単語を並べ、自分の窓の内容領域に入る最大の倍率
 * (gfx_text の scale 4→1) で折り返し、中央に描く。
 * 単語がちょうど "|" のときは改行の合図。
 *
 * LCD のフォント (5x7、字送り 6) に字形があるのは大文字
 * A〜Z と数字、! + - . : / > だけ (小文字は空白になる)
 * ので、描く前に大文字へ寄せる。
 *
 * 描いた絵は終了後も前面窓に残る。コンソールには描いた
 * 行を "ima: <行>" と出し、最後に "ima: done" と出す
 * (test.sh のシナリオ AG、test-k210.py の K26 が見る)。
 * 引数が無ければ使い方を出してステータス 2 で終わる。
 */

#define SYS_PUTC 1
#define SYS_EXIT 3
#define SYS_GFX_INFO 43
#define SYS_GFX_CLEAR 44
#define SYS_GFX_TEXT 46

/* RGB565 の色 (gdemo と同じ計算) */
#define COL(r, g, b) \
    ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define BG COL(0, 0, 64)        /* 濃い青 */
#define FG COL(255, 255, 255)   /* 白 */

#define MAX_LINE 48             /* gfx_text の 1 回の上限 */
#define MAX_LINES 12

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

static int strlen_(const char *s)
{
    int n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static char upper_(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

/* 単語列を 1 行 cap 文字までに折り返す。'|' は改行。
 * 行は大文字化して lines に詰める。行数を返す。
 * cap より長い単語は途中で割る。行がいっぱいになったら
 * 以降の単語は落とす (バッファの外へは書かない) */
static int wrap(char **words, int nwords, int cap,
                char lines[][MAX_LINE + 1])
{
    int nl = 0;     /* 確定した行数 */
    int cur = 0;    /* 現在の行の文字数 */
    int i, j;

    for (i = 0; i < nwords; i++) {
        const char *w = words[i];
        int wl = strlen_(w);

        if (nl >= MAX_LINES) {
            break;
        }
        if (wl == 1 && w[0] == '|') {
            lines[nl][cur] = '\0';
            nl++;
            cur = 0;
            continue;
        }
        while (wl > 0) {
            int room, take;

            if (nl >= MAX_LINES) {
                break;
            }
            if (cur > 0 && wl <= cap && cur + 1 + wl > cap) {
                /* 単語が丸ごと入らないので改行してから置く */
                lines[nl][cur] = '\0';
                nl++;
                cur = 0;
                continue;
            }
            room = cap - cur - (cur > 0 ? 1 : 0);
            if (room <= 0) {
                lines[nl][cur] = '\0';
                nl++;
                cur = 0;
                continue;
            }
            take = wl < room ? wl : room;
            if (cur > 0) {
                lines[nl][cur++] = ' ';
            }
            for (j = 0; j < take; j++) {
                lines[nl][cur++] = upper_(w[j]);
            }
            w += take;
            wl -= take;
            if (wl > 0) {
                /* 割った残りは次の行へ */
                lines[nl][cur] = '\0';
                nl++;
                cur = 0;
            }
        }
    }
    if (cur > 0 && nl < MAX_LINES) {
        lines[nl][cur] = '\0';
        nl++;
    }
    if (nl == 0) {
        lines[0][0] = '\0';
        nl = 1;
    }
    return nl;
}

__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    unsigned int info[4];
    static char lines[MAX_LINES][MAX_LINE + 1];
    int winw, winh, scale, nl = 0, i;

    if (argc < 2) {
        puts_("ima: usage: run ima <words...>\n");
        exit_(2);
    }
    if (syscall3(SYS_GFX_INFO, (long)info, 0, 0) != 0) {
        puts_("ima: info failed\n");
        exit_(1);
    }
    winw = (int)info[2];
    winh = (int)info[3];

    /* 入る最大の倍率を探す。字送りは 6*scale、高さは
     * 7*scale、行の間隔は 2*scale 空ける (pitch 9*scale) */
    for (scale = 4; scale >= 1; scale--) {
        int cap = winw / (6 * scale);
        int fit;
        if (cap > MAX_LINE) {
            cap = MAX_LINE;
        }
        fit = winh / (9 * scale);
        if (fit > MAX_LINES) {
            fit = MAX_LINES;
        }
        nl = wrap(argv + 1, argc - 1, cap, lines);
        if (nl <= fit) {
            break;
        }
        if (scale == 1) {
            nl = fit;  /* 入り切らない分は落とす */
        }
    }

    syscall3(SYS_GFX_CLEAR, BG, 0, 0);
    {
        int block_h = nl * 7 * scale + (nl - 1) * 2 * scale;
        int y = (winh - block_h) / 2;

        for (i = 0; i < nl; i++) {
            int wpx = strlen_(lines[i]) * 6 * scale;
            int x = (winw - wpx) / 2;

            syscall3(SYS_GFX_TEXT, x | (y << 16), (long)lines[i],
                     FG | (scale << 16));
            puts_("ima: ");
            puts_(lines[i]);
            putc_('\n');
            y += 9 * scale;
        }
    }
    puts_("ima: done\n");
    exit_(0);
}
