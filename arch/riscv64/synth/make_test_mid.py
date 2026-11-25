#!/usr/bin/env python3
"""smfplay の検証用 SMF (Standard MIDI File) を生成する。

  python3 make_test_mid.py [出力ディレクトリ]

SONG0.MID (format 0) と SONG1.MID (format 1) を、同じ曲で
作る。division は 480 PPQ。テンポ変更を 2 回含む
(500000 -> 375000 -> 500000 us/四分音符)、プログラム
チェンジで音色が途中で替わる構成。曲の長さは約 9.8 秒。
イベント列はランニングステータス圧縮して書き出すので、
読み手のランニングステータス処理も同時に試せる。

曲の中身 (全てチャンネル 0):
  メロディ  四分音符 x 22 (C メジャー系)
  和音      2 小節ごとに 3 和音のスタブ
  ベース    和音と同時に低音
  その他    tick 0 で program 0 + CC7=100、tick 4800 で
            program 88 (GM Synth Pad)、最後に CC123
            (all notes off)
"""

import struct
import sys

DIV = 480

TEMPOS = [(0, 500000), (4800, 375000), (9600, 500000)]

MELODY = [72, 76, 79, 84, 79, 76, 74, 76, 77, 81, 79,
          76, 74, 71, 72, 76, 79, 84, 81, 79, 76, 74]
CHORDS = [(60, 64, 67), (57, 60, 65)]
BASS = [48, 53, 55, 48, 53, 48]


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def track_bytes(events):
    """events: (tick, order, payload) のリスト。
    payload は ('ch', bytes) か ('meta', type, bytes)。
    ランニングステータス圧縮ありで MTrk の中身を作る"""
    events = sorted(events, key=lambda e: (e[0], e[1]))
    out = bytearray()
    last_tick = 0
    running = None
    for tick, _order, payload in events:
        out += vlq(tick - last_tick)
        last_tick = tick
        if payload[0] == "meta":
            out += bytes([0xFF, payload[1]]) + vlq(len(payload[2]))
            out += payload[2]
            running = None
        else:
            data = payload[1]
            if running == data[0]:
                out += data[1:]
            else:
                out += data
                running = data[0]
    out += vlq(0) + bytes([0xFF, 0x2F, 0x00])
    return bytes(out)


def chan_events(notes, prog_cc=True):
    """ノート列と program/cc を (tick, order, payload) にする。
    order: 0=tempo 1=prog/cc 2=noteoff 3=noteon"""
    ev = []
    if prog_cc:
        ev.append((0, 1, ("ch", bytes([0xC0, 0]))))
        ev.append((0, 1, ("ch", bytes([0xB0, 7, 100]))))
        ev.append((4800, 1, ("ch", bytes([0xC0, 88]))))
        ev.append((10600, 1, ("ch", bytes([0xB0, 123, 0]))))
    for start, dur, note, vel in notes:
        ev.append((start, 3, ("ch", bytes([0x90, note, vel]))))
        ev.append((start + dur, 2, ("ch", bytes([0x80, note, 0]))))
    return ev


def melody_notes():
    return [(i * 480, 420, n, 100) for i, n in enumerate(MELODY)]


def chord_bass_notes():
    notes = []
    for i, start in enumerate(range(0, 9601, 1920)):
        for n in CHORDS[i % 2]:
            notes.append((start, 1500, n, 80))
        notes.append((start, 900, BASS[i], 90))
    return notes


def tempo_events():
    return [(t, 0, ("meta", 0x51, struct.pack(">I", us)[1:]))
            for t, us in TEMPOS]


def smf(fmt, tracks):
    out = bytearray()
    out += b"MThd" + struct.pack(">IHHH", 6, fmt, len(tracks), DIV)
    for tr in tracks:
        body = track_bytes(tr)
        out += b"MTrk" + struct.pack(">I", len(body)) + body
    return bytes(out)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    mel = melody_notes()
    cb = chord_bass_notes()

    song0 = smf(0, [tempo_events() + chan_events(mel + cb)])
    song1 = smf(1, [tempo_events(),
                    chan_events(mel),
                    chan_events(cb, prog_cc=False)])

    for name, data in (("SONG0.MID", song0), ("SONG1.MID", song1)):
        path = outdir.rstrip("/") + "/" + name
        with open(path, "wb") as f:
            f.write(data)
        print(f"{path}: {len(data)} bytes")


if __name__ == "__main__":
    main()
