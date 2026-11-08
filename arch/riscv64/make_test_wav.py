#!/usr/bin/env python3
"""nami (doc/nami-design.md) の検証用テスト WAV を作る。

virt の test.sh シナリオ AC が使う素材。44100Hz・
モノラル 16bit・440Hz サイン波・振幅 8000・2 秒
(88200 サンプル) の最小の WAV (44 バイトヘッダ +
data チャンクだけ)。

期待値 (samples / FNV) はこのファイルからではなく、
テスト側が出来上がった WAV の data チャンクを読んで
計算する (素材そのものを物差しにするため)。build.sh
が virt のときに呼び、make_sd_image.py の取り込み口で
TEST.WAV として SD イメージに置く。
"""

import math
import struct
import sys

RATE = 44100
SECONDS = 2
FREQ = 440
AMPLITUDE = 8000


def build():
    n = RATE * SECONDS
    pcm = bytearray()
    for i in range(n):
        v = int(round(AMPLITUDE * math.sin(2.0 * math.pi * FREQ * i
                                           / RATE)))
        pcm += struct.pack("<h", v)
    hdr = b"".join([
        b"RIFF", struct.pack("<I", 36 + len(pcm)), b"WAVE",
        b"fmt ", struct.pack("<IHHIIHH", 16, 1, 1, RATE,
                             RATE * 2, 2, 16),
        b"data", struct.pack("<I", len(pcm)),
    ])
    return hdr + bytes(pcm)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test.wav"
    with open(out, "wb") as f:
        f.write(build())
    print("wrote", out)


if __name__ == "__main__":
    main()
