#ifndef KOSABAOS_GFX_H
#define KOSABAOS_GFX_H

#include <stdint.h>

/* gfx.h -- 画面表示 (描画 syscall と窓) のカーネル内 API。
 * 設計は doc/gfx-design.md、仕様は doc/syscalls.md の 43〜47。
 *
 * 窓は走者 (runner) ごとに 1 つで、geometry は走者番号から
 * 決まる固定レイアウト。描画 syscall の座標は「自分の窓の
 * 内容領域の中の相対座標」で、原点の加算とクリップは
 * ここが担う。時計帯 (画面下部 24px) には一切触らない。
 *
 * LCD の無い環境 (virt) では実描画をせず、呼び出しと窓の
 * 出来事を klog への要約行だけで残す (オーディオダミーと
 * 同じ流儀。要約行は LCD のある環境でも残るので、dmesg
 * 経由の検証は両環境で同じになる)。 */

/* 論理画面の大きさ。LCD のある環境ではボード層 (lcd.h) の
 * 値が正本で、無い環境では現在の唯一のパネルに合わせた
 * 仮の値を返す (プログラムから見た geometry を環境で
 * 変えないため) */
int gfx_screen_w(void);
int gfx_screen_h(void);

/* syscall の実体 (trap.c の do_syscall から呼ばれる)。
 * runner は呼び出し元の走者番号 (0 = 前面、1〜 = ジョブ) */
int gfx_info(int runner, uint32_t out[4]);  /* out: 画面 w/h, 窓の内容 w/h */
void gfx_clear(int runner, uint32_t color);
void gfx_rect(int runner, int x, int y, int w, int h, uint32_t color);
void gfx_text(int runner, int x, int y, const char *s, int scale,
              uint32_t color);
void gfx_line(int runner, int x0, int y0, int x1, int y1,
              uint32_t color);

/* 窓の出来事 (trap.c から通知される。段B) */
void gfx_init(void);  /* 起動時に 1 回。前面の窓を開く */
void gfx_window_open(int runner, const char *title);   /* ジョブ開始 */
void gfx_window_done(int runner);                     /* ジョブ終了 */
void gfx_window_close(int runner);                    /* 回収・破棄 */
void gfx_set_focus(int runner);  /* コンソールの持ち主の変更 */
void gfx_fg_title(const char *title);  /* 前面の窓のタイトル更新 */

#endif
