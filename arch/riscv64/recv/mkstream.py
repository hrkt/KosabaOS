#!/usr/bin/env python3
# mkstream.py -- test.sh シナリオ AL 用の送信ストリーム生成
#
# 決定的な payload (2500 バイト) を組み、フレーム列を
# 標準出力 (バイナリ) へ書く。途中に「重複フレーム」と
# 「CRC 不正フレーム + 再送」を 1 回ずつ混ぜ、受け側の
# 規則 (書かずに ACK / NAK して再送を受ける) まで見る。
# 期待する size と fnv は標準エラーへ出す。
import sys

PAYLOAD_LEN = 2500


def crc16(data: bytes) -> int:
    # CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def frame(start: int, seq: int, payload: bytes,
          bad_crc: bool = False) -> bytes:
    ln = len(payload)
    head = bytes([seq, ln & 0xFF, (ln >> 8) & 0xFF])
    crc = crc16(head + payload)
    if bad_crc:
        crc ^= 0x00FF
    # CRC フィールドは下位バイト先 (LE)
    return (bytes([start]) + head + payload
            + bytes([crc & 0xFF, (crc >> 8) & 0xFF]))


def main() -> None:
    payload = bytes(((i * 37 + 11) & 0xFF)
                    for i in range(PAYLOAD_LEN))
    f0 = frame(0x01, 0, payload[0:1024])
    f1 = frame(0x01, 1, payload[1024:2048])
    f2bad = frame(0x01, 2, payload[2048:2500], bad_crc=True)
    f2 = frame(0x01, 2, payload[2048:2500])
    etx = frame(0x03, 3, b"")
    stream = f0 + f1 + f1 + f2bad + f2 + etx
    sys.stdout.buffer.write(stream)

    fnv = 0xCBF29CE484222325
    for b in payload:
        fnv = ((fnv ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    print(f"size={PAYLOAD_LEN} fnv={fnv:016x}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
