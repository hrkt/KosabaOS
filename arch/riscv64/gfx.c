/* gfx.c -- 画面表示 (描画 syscall と窓) の実体
 *
 * 設計は doc/gfx-design.md、仕様は doc/syscalls.md の
 * 43〜47。役割分担:
 *   - ここ: 窓の geometry (走者番号 → 矩形)、内容領域への
 *     原点加算とクリップ、klog への要約、実描画の呼び出し
 *   - trap.c: ユーザーポインタの検証と syscall の振り分け
 *   - boards/k210/lcd.c: 実描画の部品 (LCD の無い環境では
 *     BOARD_FEATURE_LCD で実描画ごと省く)
 *
 * 時計帯 (画面の下 24px) は時計タスクの領分で、窓は
 * そちらへはみ出さない (クライアント領域はその上まで)。
 * 描画は syscall の処理の中だけで起き、走者の切り替えは
 * トラップの区切りでしか起きないので、1 つの原語の途中
 * で別の走者の描画が混ざることはない。 */

#include <stdint.h>
#include "board.h"
#include "gfx.h"
#include "klog.h"
#include "vm.h"  /* JOB_MAX */
#if BOARD_FEATURE_LCD
#include "lcd.h"
#endif

/* --- geometry ------------------------------------------------------
 * 論理画面は当面 320x240 固定。LCD のある環境では lcd.h の
 * 値が正本。クライアント領域は画面から時計帯 (24px) を
 * 除いた上側で、窓はその中で固定レイアウトに割る:
 * 前面 (走者 0) は全体、ジョブは 2x2 グリッドの 1 マス */

#if BOARD_FEATURE_LCD
#define PANEL_W LCD_WIDTH
#define PANEL_H LCD_HEIGHT
#else
#define PANEL_W 320
#define PANEL_H 240
#endif

#define TICK_STRIP_H 24                 /* 時計帯 (下部) */
#define CLIENT_H (PANEL_H - TICK_STRIP_H)
#define TITLE_H 13                      /* タイトルバーの高さ */
#define BORDER 1                        /* 枠の太さ */
#define DESKTOP_COLOR 0x18c3            /* RGB(24,24,24) 相当 */

/* 窓の矩形 (枠とタイトルバーを含む) を走者番号から決める */
static void window_rect(int runner, int *x, int *y, int *w, int *h)
{
    if (runner <= 0) {
        *x = 0;
        *y = 0;
        *w = PANEL_W;
        *h = CLIENT_H;
    } else {
        int cell_w = PANEL_W / 2;
        int cell_h = CLIENT_H / 2;
        int idx = (runner - 1) % JOB_MAX;
        *x = (idx % 2) * cell_w;
        *y = (idx / 2) * cell_h;
        *w = cell_w;
        *h = cell_h;
    }
}

/* 内容領域 (プログラムが描ける範囲) の原点と大きさ */
static void content_rect(int runner, int *x, int *y, int *w, int *h)
{
    int wx, wy, ww, wh;
    window_rect(runner, &wx, &wy, &ww, &wh);
    *x = wx + BORDER;
    *y = wy + TITLE_H;
    *w = ww - BORDER * 2;
    *h = wh - TITLE_H - BORDER;
}

int gfx_screen_w(void)
{
    return PANEL_W;
}

int gfx_screen_h(void)
{
    return PANEL_H;
}

/* --- klog への要約 (ログ専用。両環境で同じ行が残る) --------------- */

static void lk_hex16(uint32_t v)
{
    int i;
    lkputs("0x");
    for (i = 12; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xF);
        lkputc((char)(d < 10 ? '0' + d : 'a' + (d - 10)));
    }
}

static void log_call(const char *what, int runner)
{
    lkputs("gfx: ");
    lkputs(what);
    lkputs(" r=");
    lkput_dec((uint64_t)runner);
}

static void log_xy(const char *nx, int x, const char *ny, int y)
{
    lkputc(' ');
    lkputs(nx);
    lkputc('=');
    lkput_dec((uint64_t)x);
    lkputc(' ');
    lkputs(ny);
    lkputc('=');
    lkput_dec((uint64_t)y);
}

static void log_color(uint32_t color)
{
    lkputs(" color=");
    lk_hex16(color);
    lkputc('\n');
}

/* --- 実描画の入口 (LCD の無い環境では何もしない) ------------------- */

static void draw_fill(int x, int y, int w, int h, uint32_t color)
{
#if BOARD_FEATURE_LCD
    if (w > 0 && h > 0) {
        lcd_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w,
                      (uint32_t)h, (uint16_t)color);
    }
#else
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
#endif
}

/* 矩形 (絶対座標) を内容領域 (cx, cy, cw, ch) でクリップして
 * 塗る。交わりが無ければ何もしない */
static void fill_clipped(int cx, int cy, int cw, int ch,
                         int x, int y, int w, int h, uint32_t color)
{
    int x0 = x > cx ? x : cx;
    int y0 = y > cy ? y : cy;
    int x1 = x + w < cx + cw ? x + w : cx + cw;
    int y1 = y + h < cy + ch ? y + h : cy + ch;
    if (x1 > x0 && y1 > y0) {
        draw_fill(x0, y0, x1 - x0, y1 - y0, color);
    }
}

/* --- syscall の実体 ------------------------------------------------- */

int gfx_info(int runner, uint32_t out[4])
{
    int cx, cy, cw, ch;
    content_rect(runner, &cx, &cy, &cw, &ch);
    out[0] = (uint32_t)PANEL_W;
    out[1] = (uint32_t)PANEL_H;
    out[2] = (uint32_t)cw;
    out[3] = (uint32_t)ch;
    log_call("info", runner);
    lkputs(" win=");
    lkput_dec((uint64_t)cw);
    lkputc('x');
    lkput_dec((uint64_t)ch);
    lkputc('\n');
    return 0;
}

void gfx_clear(int runner, uint32_t color)
{
    color &= 0xFFFF;
    int cx, cy, cw, ch;
    content_rect(runner, &cx, &cy, &cw, &ch);
    log_call("clear", runner);
    log_color(color);
    draw_fill(cx, cy, cw, ch, color);
}

void gfx_rect(int runner, int x, int y, int w, int h, uint32_t color)
{
    color &= 0xFFFF;
    int cx, cy, cw, ch;
    content_rect(runner, &cx, &cy, &cw, &ch);
    log_call("rect", runner);
    log_xy("x", x, "y", y);
    log_xy("w", w, "h", h);
    log_color(color);
    fill_clipped(cx, cy, cw, ch, cx + x, cy + y, w, h, color);
}

void gfx_text(int runner, int x, int y, const char *s, int scale,
              uint32_t color)
{
    int cx, cy, cw, ch;
    content_rect(runner, &cx, &cy, &cw, &ch);
    color &= 0xFFFF;
    if (scale < 1) {
        scale = 1;
    }
    if (scale > 4) {
        scale = 4;
    }
    log_call("text", runner);
    log_xy("x", x, "y", y);
    lkputs(" \"");
    lkputs(s);
    lkputs("\" scale=");
    lkput_dec((uint64_t)scale);
    log_color(color);
#if BOARD_FEATURE_LCD
    lcd_draw_text_clip((uint32_t)(cx + x), (uint32_t)(cy + y), s,
                       (uint32_t)scale, (uint16_t)color,
                       (uint32_t)cx, (uint32_t)cy, (uint32_t)cw,
                       (uint32_t)ch);
#else
    (void)cx;
    (void)cy;
    (void)cw;
    (void)ch;
#endif
}

/* --- 窓の状態と枠 (段B) ---------------------------------------------
 * 窓の生滅とフォーカスは trap.c の出来事 (ジョブ開始・終了・
 * 回収、コンソールの持ち主の変更) に連れて変わる。枠と
 * タイトルバーは内容領域の外側にだけ描くので、フォーカスの
 * 描き直しでプログラムの絵が消えることはない (フレーム
 * バッファを持たない ST7789 では、これが共存の条件)。
 * タイトルは字形表に小文字が無いので、表示のときだけ
 * 大文字に寄せる (klog には元の名前が残る) */

#define FRAME_FOCUS 0xffff   /* 白 */
#define FRAME_NORMAL 0x8410  /* 灰 RGB(128,128,128) 相当 */
#define FRAME_DONE 0x4208    /* 暗い灰 RGB(64,64,64) 相当 */
#define TITLE_BG_FOCUS 0x0210   /* 濃い青緑 RGB(0,64,128) 相当 */
#define TITLE_BG_NORMAL 0x3186  /* RGB(48,48,48) 相当 */
#define TITLE_BG_DONE 0x18c3    /* デスクトップ色 */
#define TITLE_FG_FOCUS 0xffff
#define TITLE_FG_NORMAL 0xbdd7  /* RGB(184,184,184) 相当 */
#define TITLE_FG_DONE 0x8410

struct gfx_win_state {
    int used;
    int done;
    char title[32];
};

static struct gfx_win_state win_state[JOB_MAX + 1];
static int focus_runner = 0;

static void copy_title(char *dst, const char *src)
{
    int i = 0;
    while (src[i] != '\0' && i < 31) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void log_event(const char *what, int runner)
{
    lkputs("gfx: ");
    lkputs(what);
    lkputs(" r=");
    lkput_dec((uint64_t)runner);
}

/* 枠とタイトルバーを描く (LCD の無い環境では何もしない)。
 * 色は状態 (フォーカス / 通常 / 終了) で決まる */
static void draw_frame(int runner)
{
#if BOARD_FEATURE_LCD
    const struct gfx_win_state *st = &win_state[runner];
    int x, y, w, h;
    uint32_t frame, tbg, tfg;
    char disp[40];
    int i = 0, n = 0;

    if (!st->used) {
        return;
    }
    window_rect(runner, &x, &y, &w, &h);
    if (runner == focus_runner) {
        frame = FRAME_FOCUS;
        tbg = TITLE_BG_FOCUS;
        tfg = TITLE_FG_FOCUS;
    } else if (st->done) {
        frame = FRAME_DONE;
        tbg = TITLE_BG_DONE;
        tfg = TITLE_FG_DONE;
    } else {
        frame = FRAME_NORMAL;
        tbg = TITLE_BG_NORMAL;
        tfg = TITLE_FG_NORMAL;
    }
    draw_fill(x, y, w, BORDER, frame);
    draw_fill(x, y + h - BORDER, w, BORDER, frame);
    draw_fill(x, y, BORDER, h, frame);
    draw_fill(x + w - BORDER, y, BORDER, h, frame);
    draw_fill(x + BORDER, y + BORDER, w - BORDER * 2,
              TITLE_H - BORDER, tbg);
    /* タイトルは大文字に寄せて描く (字形表の都合) */
    while (st->title[i] != '\0' && n < 39) {
        char c = st->title[i++];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        disp[n++] = c;
    }
    if (st->done) {
        const char *sfx = " DONE";
        while (*sfx != '\0' && n < 39) {
            disp[n++] = *sfx++;
        }
    }
    disp[n] = '\0';
    lcd_draw_text_clip((uint32_t)(x + 4), (uint32_t)(y + 3), disp, 1,
                       (uint16_t)tfg, (uint32_t)(x + BORDER),
                       (uint32_t)(y + BORDER),
                       (uint32_t)(w - BORDER * 2),
                       (uint32_t)(TITLE_H - BORDER));
#else
    (void)runner;
#endif
}

void gfx_init(void)
{
    win_state[0].used = 1;
    win_state[0].done = 0;
    copy_title(win_state[0].title, "FG");
    focus_runner = 0;
    log_event("open", 0);
    lkputs(" \"FG\"\n");
    log_event("focus", 0);
    lkputc('\n');
    draw_frame(0);
}

void gfx_window_open(int runner, const char *title)
{
    int cx, cy, cw, ch;
    if (runner < 1 || runner > JOB_MAX) {
        return;
    }
    win_state[runner].used = 1;
    win_state[runner].done = 0;
    copy_title(win_state[runner].title, title);
    log_event("open", runner);
    lkputs(" \"");
    lkputs(win_state[runner].title);
    lkputs("\"\n");
    /* 前の持ち主の残像を残さないよう、内容を消してから枠 */
    content_rect(runner, &cx, &cy, &cw, &ch);
    draw_fill(cx, cy, cw, ch, DESKTOP_COLOR);
    draw_frame(runner);
}

void gfx_window_done(int runner)
{
    if (runner < 1 || runner > JOB_MAX || !win_state[runner].used) {
        return;
    }
    win_state[runner].done = 1;
    log_event("done", runner);
    lkputc('\n');
    draw_frame(runner);
}

void gfx_window_close(int runner)
{
    int x, y, w, h;
    if (runner < 1 || runner > JOB_MAX || !win_state[runner].used) {
        return;
    }
    win_state[runner].used = 0;
    win_state[runner].done = 0;
    log_event("close", runner);
    lkputc('\n');
    /* 枠ごとデスクトップ色に戻す */
    window_rect(runner, &x, &y, &w, &h);
    draw_fill(x, y, w, h, DESKTOP_COLOR);
}

void gfx_set_focus(int runner)
{
    int old;
    if (runner == focus_runner) {
        return;
    }
    old = focus_runner;
    focus_runner = runner;
    log_event("focus", runner);
    lkputc('\n');
    draw_frame(old);
    draw_frame(runner);
}

void gfx_fg_title(const char *title)
{
    copy_title(win_state[0].title, title);
    draw_frame(0);
}

void gfx_line(int runner, int x0, int y0, int x1, int y1,
              uint32_t color)
{
    color &= 0xFFFF;
    int cx, cy, cw, ch;
    content_rect(runner, &cx, &cy, &cw, &ch);
    log_call("line", runner);
    log_xy("x0", x0, "y0", y0);
    log_xy("x1", x1, "y1", y1);
    log_color(color);
#if BOARD_FEATURE_LCD
    /* Bresenham。点は内容領域の中だけを 1x1 で塗る */
    {
        int dx = x1 > x0 ? x1 - x0 : x0 - x1;
        int sx = x0 < x1 ? 1 : -1;
        int dy = y1 > y0 ? y0 - y1 : y1 - y0;  /* -|dy| */
        int sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        int px = x0, py = y0;
        for (;;) {
            if (px >= 0 && px < cw && py >= 0 && py < ch) {
                lcd_fill_rect((uint32_t)(cx + px),
                              (uint32_t)(cy + py), 1, 1,
                              (uint16_t)color);
            }
            if (px == x1 && py == y1) {
                break;
            }
            {
                int e2 = 2 * err;
                if (e2 >= dy) {
                    err += dy;
                    px += sx;
                }
                if (e2 <= dx) {
                    err += dx;
                    py += sy;
                }
            }
        }
    }
#else
    (void)cx;
    (void)cy;
    (void)cw;
    (void)ch;
#endif
}
