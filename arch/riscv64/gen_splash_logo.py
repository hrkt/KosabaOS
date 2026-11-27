#!/usr/bin/env python3
# gen_splash_logo.py -- スプラッシュのロゴ "KOSABAOS" の
# ビットマップ (boards/k210/splash_logo.h) を生成する。
#
# 設計 (doc/userland-design.md 決定 2): スプラッシュの固定
# 内容はフォントで描かずビットマップで持つ。ここでは従来の
# 5x7 字形 (lcd.c の表と同じ値) を scale 4 で描いた結果を
# 1bpp ビットマップとして固め、生成物だけをカーネルへ
# 入れる (カーネルに字形は残らない)。
#
# 使い方: python3 gen_splash_logo.py
#   -> boards/k210/splash_logo.h を書き直す

SCALE = 4
TEXT = "KOSABAOS"
# lcd.c の 5x7 表と同じ列バイト (bit6 = 最上行、bit0 = 最下行)
GLYPHS = {
    "K": [0x7F, 0x08, 0x14, 0x22, 0x41],
    "O": [0x3E, 0x41, 0x41, 0x41, 0x3E],
    "S": [0x31, 0x49, 0x49, 0x49, 0x46],
    "A": [0x3F, 0x48, 0x48, 0x48, 0x3F],
    "B": [0x7F, 0x49, 0x49, 0x49, 0x36],
}

W = len(TEXT) * 6 * SCALE   # 文字送り 6 列ぶん (lcd_draw_text と同じ)
H = 7 * SCALE


def pixel(x, y):
    cx = x // SCALE
    cy = y // SCALE
    ch = TEXT[cx // 6]
    col = cx % 6
    if col >= 5:
        return 0
    return (GLYPHS[ch][col] >> (6 - cy)) & 1


def main():
    rows = []
    for y in range(H):
        bits = [pixel(x, y) for x in range(W)]
        row = bytearray()
        for i in range(0, W, 8):
            b = 0
            for j in range(8):
                b = (b << 1) | bits[i + j]
            row.append(b)
        rows.append(bytes(row))

    out = []
    out.append("/* splash_logo.h -- スプラッシュのロゴ (KOSABAOS) の")
    out.append(" * ビットマップ。gen_splash_logo.py で生成したので、")
    out.append(" * 手で直さずスクリプトを直して再生成すること。")
    out.append(" * 1bpp、行優先、MSB 先行 (doc/userland-design.md 決定 2) */")
    out.append("#ifndef KOSABAOS_SPLASH_LOGO_H")
    out.append("#define KOSABAOS_SPLASH_LOGO_H")
    out.append("")
    out.append("#define SPLASH_LOGO_W %d" % W)
    out.append("#define SPLASH_LOGO_H %d" % H)
    out.append("")
    out.append("static const uint8_t splash_logo_bits[] = {")
    for row in rows:
        out.append("    " + ", ".join("0x%02X" % b for b in row) + ",")
    out.append("};")
    out.append("")
    out.append("#endif")
    path = "boards/k210/splash_logo.h"
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %s (%d x %d, %d bytes)" % (path, W, H, W * H // 8))


if __name__ == "__main__":
    main()
