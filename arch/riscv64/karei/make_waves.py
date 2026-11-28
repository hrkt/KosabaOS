#!/usr/bin/env python3
"""make_waves.py -- TX81Z 風オペレータ波形 (OPZ 波形 8 種) の
テーブル生成と倍音構成の照合。

使い方:
  python3 make_waves.py          倍音構成の照合表を表示
  python3 make_waves.py --emit   karei.c へ入れる fill_waves()
                                 の C コードを標準出力へ出す

波形の定義 (位相 theta、s = sin(theta)):
  W1 サイン            s
  W2 二乗サイン        s * |s| (符号保存二乗)
  W3 全波整流          |s|
  W4 半波二乗          前半 s * |s|、後半 0
  W5 半波 2 倍音       前半 sin(2 theta)、後半 0
  W6 半波 2 倍音二乗   W5 の符号保存二乗
  W7 折り返し          前半 s、後半 |cos(theta)|
  W8 半波全波 2 倍音   前半 |sin(2 theta)|、後半 0

出典と確度は doc/karei-design.md の波形節に書いた。
W1 は確定、W2/W5 は公開資料とよく整合するが、W4/W7/W8 は
マニュアル (TX81Z オーナーズマニュアル p.50 の倍音構成) と
矛盾しない候補を選んだ推定である。本スクリプトは生成した
波形の FFT をマニュアルの倍音リストと照合して表示するので、
定義を変えたら照合結果も必ず確認すること。
"""
import sys

import numpy as np

N = 256
I = np.arange(N)
TH = 2 * np.pi * I / N
S = np.sin(TH)


def wave(k):
    """波形 k (0..7) の 1 周期 (float、ピーク 1.0)"""
    if k == 0:
        return S.copy()
    if k == 1:
        return S * np.abs(S)
    if k == 2:
        return np.abs(S)
    if k == 3:
        f = S * np.abs(S)
        f[N // 2:] = 0.0
        return f
    if k == 4:
        f = np.sin(2 * TH)
        f[N // 2:] = 0.0
        return f
    if k == 5:
        f = wave(4)
        return f * np.abs(f)
    if k == 6:
        f = S.copy()
        f[N // 2:] = np.abs(np.cos(TH[N // 2:]))
        return f
    if k == 7:
        f = np.abs(np.sin(2 * TH))
        f[N // 2:] = 0.0
        return f
    raise ValueError(k)


# TX81Z マニュアル p.50 の倍音リスト (基本波 = 1 として、
# リストに挙がっている倍音番号)
MANUAL = {
    0: {1},
    1: {1, 3, 5, 7, 9, 11},
    2: {2, 4, 6, 8, 10, 12},
    3: {2, 3, 5, 7},
    4: {2, 3, 5, 7, 9, 11},
    5: {1, 2, 3, 5, 6, 7, 9, 10, 11},
    6: {3, 4, 5, 7, 8, 9, 11, 12},
    7: {3, 4, 5, 7, 8, 11},
}
NAMES = ["W1 sine", "W2 square-sine", "W3 full-rect", "W4 half-sq",
         "W5 half-2x", "W6 half-2x-sq", "W7 fold", "W8 half-rect2x"]


def check():
    big = 8192
    th = np.linspace(0, 2 * np.pi, big, endpoint=False)
    s = np.sin(th)
    waves = []
    for k in range(8):
        # 256 点定義を補間なしで評価するため大きな格子で作り直す
        if k == 0:
            f = s.copy()
        elif k == 1:
            f = s * np.abs(s)
        elif k == 2:
            f = np.abs(s)
        elif k == 3:
            f = s * np.abs(s)
            f[big // 2:] = 0.0
        elif k == 4:
            f = np.sin(2 * th)
            f[big // 2:] = 0.0
        elif k == 5:
            f = np.sin(2 * th)
            f[big // 2:] = 0.0
            f = f * np.abs(f)
        elif k == 6:
            f = s.copy()
            f[big // 2:] = np.abs(np.cos(th[big // 2:]))
        else:
            f = np.abs(np.sin(2 * th))
            f[big // 2:] = 0.0
        waves.append(f)
    for k, f in enumerate(waves):
        amp = np.abs(np.fft.rfft(f)) / big * 2
        mx = amp[1:13].max()
        ps = {j for j in range(1, 13) if amp[j] > 0.02 * mx}
        rel = " ".join(f"{j}:{amp[j] / amp[1]:.2f}" for j in range(1, 13))
        print(f"{NAMES[k]}: present={sorted(ps)}")
        print(f"   manual={sorted(MANUAL[k])}  amps(vs fund)={rel}")


def emit():
    print("/* make_waves.py が生成 (手で直さない) */")
    print("void fill_waves(void)")
    print("{")
    print("    int i;")
    print("    for (i = 0; i < 256; i++)")
    print("        g_wav[i] = g_sine[i];")
    for k in range(1, 8):
        f = wave(k)
        q = np.round(f * 32767).astype(int)
        base = 256 * k
        print(f"    /* wave {k + 1} */")
        for r in range(0, 256, 4):
            stmts = " ".join(
                f"g_wav[{base + c}] = {q[c]};" for c in range(r, r + 4))
            print(f"    {stmts}")
    print("}")


def emit_pow2():
    """ピッチ EG 用の 2^(i/256) Q15 表 (fill_pow2)"""
    print("/* make_waves.py が生成 (手で直さない) */")
    print("void fill_pow2(void)")
    print("{")
    for r in range(0, 256, 4):
        stmts = " ".join(
            f"g_pow2[{c}] = {int(round(32768 * 2 ** (c / 256)))};"
            for c in range(r, r + 4))
        print(f"    {stmts}")
    print("}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--emit":
        emit()
    elif len(sys.argv) > 1 and sys.argv[1] == "--emit-pow2":
        emit_pow2()
    else:
        check()
