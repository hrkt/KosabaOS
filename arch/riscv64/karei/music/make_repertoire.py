#!/usr/bin/env python3
"""レパートリー SMF の生成: Mutopia Project の PD/CC 素材を
smfplay + karei 用の format 1 SMF に変換する。

  python3 make_repertoire.py [出力ディレクトリ]

素材は実行時に Mutopia の FTP から取得する (リポジトリに
他人のバイナリを持ち込まない方針のため、.mid そのものは
コミットせず、このスクリプトと出典メモを正本とする)。
取得できない環境では、同じ URL から手で落としたファイルを
カレントディレクトリに置けばそれを使う。

変換内容:
  - 右手トラック -> ch 1 (ch 番号 0)、左手 -> ch 2 (番号 1)。
    karei のマルチティンバーでパート 1/2 に載る
  - 冒頭に program 0 (GM Grand Piano) を両 ch へ追加
  - ノートとベロシティはそのまま。CC は落とす (K.545 の
    CC7 は LilyPond がヘアピンを描いたもので、パート音量が
    暴れるため。ペダル CC64 は素材に含まれない)
  - テンポ: K.545 は素材の指定 (454545 us = 約 132 BPM) を
    そのまま使う。K.331 は素材が LilyPond 既定の 60 BPM に
    なっていて楽譜の Allegretto と合わないため、編集判断で
    500000 us (120 BPM) に置く (README 参照)

出典 (詳細は同ディレクトリの README.md):
  - K.545 第1楽章: Mutopia ID 998、Alejandro Sierra 浄書、
    Creative Commons Attribution-ShareAlike 3.0
  - K.331 第3楽章: Mutopia ID 108、Rune Zedeler /
    Chris Sawer 浄書、Public Domain
  作曲自体はいずれもパブリックドメイン。
"""

import os
import struct
import sys
import urllib.request

SOURCES = {
    "MOZ545.MID": {
        "url": "https://www.mutopiaproject.org/ftp/MozartWA/KV545/"
               "K545-1/K545-1.mid",
        "cache": "K545-1.mid",
        "tempo": None,  # 素材のテンポを使う
    },
    "MOZ331.MID": {
        "url": "https://www.mutopiaproject.org/ftp/MozartWA/KV331/"
               "KV331_3_RondoAllaTurca/KV331_3_RondoAllaTurca.mid",
        "cache": "KV331_3_RondoAllaTurca.mid",
        "tempo": 500000,  # Allegretto の編集判断 (README 参照)
    },
}


def read_vlq(d, i):
    v = 0
    while True:
        b = d[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            return v, i


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def parse_tracks(d):
    """SMF を読み、(format, division, [track]) を返す。
    track は (tick, kind, a, b) のリスト。kind は
    'on' / 'off' / 'tempo' / 'cc' / 'prog'"""
    assert d[:4] == b"MThd"
    ln, fmt, ntrk, div = struct.unpack(">IHHH", d[4:14])
    i = 8 + ln
    tracks = []
    for _ in range(ntrk):
        assert d[i:i + 4] == b"MTrk"
        tlen = struct.unpack(">I", d[i + 4:i + 8])[0]
        body = d[i + 8:i + 8 + tlen]
        i += 8 + tlen
        evs = []
        tick = 0
        j = 0
        running = None
        while j < len(body):
            delta, j = read_vlq(body, j)
            tick += delta
            b = body[j]
            if b == 0xFF:
                typ = body[j + 1]
                mlen, k = read_vlq(body, j + 2)
                data = body[k:k + mlen]
                j = k + mlen
                running = None
                if typ == 0x51:
                    evs.append((tick, "tempo",
                                int.from_bytes(data, "big"), 0))
                if typ == 0x2F:
                    break
                continue
            if b in (0xF0, 0xF7):
                mlen, k = read_vlq(body, j + 1)
                j = k + mlen
                running = None
                continue
            if b & 0x80:
                running = b
                j += 1
            st = running
            hi = st & 0xF0
            ch = st & 0x0F
            nd = 1 if hi in (0xC0, 0xD0) else 2
            d1 = body[j]
            d2 = body[j + 1] if nd == 2 else 0
            j += nd
            if hi == 0x90 and d2 > 0:
                evs.append((tick, "on", d1, d2))
            elif hi == 0x90 or hi == 0x80:
                evs.append((tick, "off", d1, 0))
            elif hi == 0xB0:
                evs.append((tick, "cc", d1, d2))
            elif hi == 0xC0:
                evs.append((tick, "prog", d1, ch))
        tracks.append(evs)
    return fmt, div, tracks


def track_bytes(events):
    """(tick, order, bytes) のリストから MTrk の中身を作る"""
    events = sorted(events, key=lambda e: (e[0], e[1]))
    out = bytearray()
    last = 0
    for tick, _order, payload in events:
        out += vlq(tick - last)
        last = tick
        out += payload
    out += vlq(0) + bytes([0xFF, 0x2F, 0x00])
    return bytes(out)


def convert(raw, tempo_override):
    _fmt, div, tracks = parse_tracks(raw)
    # トラック 0 はコンダクター (テンポ)、1 が右手 (高音域)、
    # 2 が左手という LilyPond の並びを、音域でも確かめる
    def pitch_range(tr):
        ps = [a for (_t, k, a, _b) in tr if k == "on"]
        return (min(ps), max(ps)) if ps else (999, -1)

    hands = [(pitch_range(tr), idx, tr)
             for idx, tr in enumerate(tracks) if pitch_range(tr)[0] != 999]
    hands.sort(key=lambda h: -h[0][1])  # 最高音が高い方を右手に
    assert len(hands) == 2, f"hand tracks != 2: {len(hands)}"
    rh, lh = hands[0][2], hands[1][2]

    tempos = [e for tr in tracks for e in tr if e[1] == "tempo"]
    tempo = tempo_override or (tempos[0][2] if tempos else 500000)
    cond = [(0, 0, bytes([0xFF, 0x51, 0x03]) +
              tempo.to_bytes(3, "big"))]

    out_tracks = [cond]
    notes_total = 0
    for ch, hand in ((0, rh), (1, lh)):
        evs = [(0, 1, bytes([0xC0 | ch, 0]))]
        for tick, kind, a, b in hand:
            if kind == "on":
                evs.append((tick, 3, bytes([0x90 | ch, a, b])))
                notes_total += 1
            elif kind == "off":
                evs.append((tick, 2, bytes([0x80 | ch, a, 0])))
        out_tracks.append(evs)

    out = bytearray()
    out += b"MThd" + struct.pack(">IHHH", 6, 1, len(out_tracks), div)
    for tr in out_tracks:
        body = track_bytes(tr)
        out += b"MTrk" + struct.pack(">I", len(body)) + body
    return bytes(out), notes_total, tempo, div


def fetch(spec):
    if os.path.exists(spec["cache"]):
        with open(spec["cache"], "rb") as f:
            return f.read(), "local cache " + spec["cache"]
    with urllib.request.urlopen(spec["url"], timeout=60) as r:
        return r.read(), spec["url"]


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    for name, spec in SOURCES.items():
        raw, src = fetch(spec)
        data, notes, tempo, div = convert(raw, spec["tempo"])
        path = outdir.rstrip("/") + "/" + name
        with open(path, "wb") as f:
            f.write(data)
        print(f"{path}: {len(data)} bytes, notes={notes}, "
              f"tempo={tempo} us/qn, division={div}, source={src}")


if __name__ == "__main__":
    main()
