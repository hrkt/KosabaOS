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


def ch_ev(tick, order, status, *data):
    return (tick, order, ("ch", bytes([status] + list(data))))


def notes_ev(ch, notes):
    ev = []
    for start, dur, note, vel in notes:
        ev.append(ch_ev(start, 3, 0x90 | ch, note, vel))
        ev.append(ch_ev(start + dur, 2, 0x80 | ch, note, 0))
    return ev


def tempo1(us=500000):
    return [(0, 0, ("meta", 0x51, struct.pack(">I", us)[1:]))]


def build_song2():
    """マルチティンバー確認曲 (format 1、4 パート、約 4 秒)。
    ch1 メロディ (prog 0)、ch2 ベース (prog 32)、ch3 和音
    (prog 48)、ch4 リード (prog 80)。パンと音量もパート別"""
    mel = [(i * 480, 420, n, 100) for i, n in enumerate(MELODY[:8])]
    t0 = [ch_ev(0, 1, 0xC0, 0), ch_ev(0, 1, 0xB0, 7, 100),
          ch_ev(0, 1, 0xB0, 10, 64)] + notes_ev(0, mel)
    bass = [(0, 900, 48, 95), (960, 900, 53, 95),
            (1920, 900, 55, 95), (2880, 900, 48, 95)]
    t1 = [ch_ev(0, 1, 0xC1, 32), ch_ev(0, 1, 0xB1, 7, 110),
          ch_ev(0, 1, 0xB1, 10, 32)] + notes_ev(1, bass)
    chords = []
    for n in (60, 64, 67):
        chords.append((0, 1700, n, 75))
    for n in (57, 60, 65):
        chords.append((1920, 1700, n, 75))
    t2 = [ch_ev(0, 1, 0xC2, 48), ch_ev(0, 1, 0xB2, 7, 80),
          ch_ev(0, 1, 0xB2, 10, 96)] + notes_ev(2, chords)
    lead = [(1920, 900, 84, 85), (2880, 900, 81, 85)]
    t3 = [ch_ev(0, 1, 0xC3, 80), ch_ev(0, 1, 0xB3, 7, 90),
          ch_ev(0, 1, 0xB3, 10, 64)] + notes_ev(3, lead)
    return smf(1, [tempo1(), t0, t1, t2, t3])


def build_pan_song(pan):
    """パン確認曲 (format 0、ch1 のみ)。pan=0 で左のみ、
    pan=127 で右のみに信号が出るはず"""
    notes = [(0, 420, 72, 100), (480, 420, 76, 100),
             (960, 420, 79, 100), (1440, 900, 84, 100)]
    ev = tempo1() + [ch_ev(0, 1, 0xC0, 0),
                     ch_ev(0, 1, 0xB0, 7, 100),
                     ch_ev(0, 1, 0xB0, 10, pan)] + notes_ev(0, notes)
    return smf(0, [ev])


def build_song4():
    """DVA 確認曲 (format 1)。ch1..5 が同時に 20 音の和音を
    2 回鳴らす。同時発音の要求 (100) はプール (32) と
    パート上限 (8) を大きく超え、steal が大量に起きる"""
    progs = [0, 32, 48, 80, 88]
    tracks = [tempo1()]
    for ch in range(5):
        ev = [ch_ev(0, 1, 0xC0 | ch, progs[ch]),
              ch_ev(0, 1, 0xB0 | ch, 7, 100)]
        base = 36 + ch * 3
        for rnd, start in ((0, 0), (1, 1920)):
            for i in range(20):
                ev.append(ch_ev(start, 3, 0x90 | ch, base + i, 90))
                ev.append(ch_ev(start + 1440, 2, 0x80 | ch,
                                base + i, 0))
        tracks.append(ev)
    return smf(1, tracks)


def build_song5():
    """ストレス曲 (format 1、約 10 秒)。ステージごとに同時
    発音が 4 -> 8 -> 16 -> 24 -> 32 と増える (各ステージ
    1920 tick = 2 秒、境界は tick 0/1920/3840/5760/7680)。
    全 8 パート使用、ステージ境界でプログラムチェンジ、
    最終ステージでは ch1 に 16 分音符の連打も入る"""
    tracks = [tempo1()]
    # (ステージごとの使用パート数, パートあたりの音数)
    stages = [(4, 1), (4, 2), (8, 2), (8, 3), (8, 4)]
    for ch in range(8):
        ev = [ch_ev(0, 1, 0xB0 | ch, 7, 100),
              ch_ev(0, 1, 0xB0 | ch, 10, 16 + ch * 14)]
        base = 45 + ch * 3
        for s, (nparts, per) in enumerate(stages):
            start = s * 1920
            ev.append(ch_ev(start, 1, 0xC0 | ch, ((s + ch) % 16) * 8))
            if ch < nparts:
                for iv in (0, 4, 7, 12)[:per]:
                    ev.append(ch_ev(start, 3, 0x90 | ch, base + iv, 90))
                    ev.append(ch_ev(start + 1800, 2, 0x80 | ch,
                                    base + iv, 0))
        if ch == 0:
            for i in range(16):
                n = 84 if i % 2 == 0 else 88
                ev.append(ch_ev(7680 + i * 120, 3, 0x90, n, 100))
                ev.append(ch_ev(7680 + i * 120 + 100, 2, 0x80, n, 0))
        tracks.append(ev)
    return smf(1, tracks)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    mel = melody_notes()
    cb = chord_bass_notes()

    song0 = smf(0, [tempo_events() + chan_events(mel + cb)])
    song1 = smf(1, [tempo_events(),
                    chan_events(mel),
                    chan_events(cb, prog_cc=False)])

    songs = (("SONG0.MID", song0), ("SONG1.MID", song1),
             ("SONG2.MID", build_song2()),
             ("SONG3L.MID", build_pan_song(0)),
             ("SONG3R.MID", build_pan_song(127)),
             ("SONG4.MID", build_song4()),
             ("SONG5.MID", build_song5()))
    for name, data in songs:
        path = outdir.rstrip("/") + "/" + name
        with open(path, "wb") as f:
            f.write(data)
        print(f"{path}: {len(data)} bytes")


if __name__ == "__main__":
    main()
