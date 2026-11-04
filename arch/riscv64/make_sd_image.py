#!/usr/bin/env python3
"""テスト用の FAT32 イメージ (sd.img) を作る。

virt の RAM ブロックデバイス (boards/virt/ramdisk.c) が
埋め込む相手。MBR + パーティション + FAT32 の実物の形で
作り、FAT ドライバ (fat.c) の MBR 読み・BPB 読み・
クラスタ鎖・サブディレクトリを test.sh で確認できる
ようにする。道具 (mkfs.vfat / mtools) に頼らないため、
中身は全部ここで組み立てる。

中身:
  HELLO.TXT      複数クラスタにまたがるテキスト。
                 末尾に SD-END-MARKER の行がある
  SUB/           サブディレクトリ
  SUB/INNER.TXT  中に SD-INNER-MARKER の行がある
"""

import struct
import sys

SECTOR = 512
TOTAL_SECTORS = 16384          # 8 MiB
PART_START = 2048              # パーティション開始 LBA (1 MiB)
PART_SECTORS = TOTAL_SECTORS - PART_START
SPC = 2                        # セクタ / クラスタ (1 KiB)
RESERVED = 32
NFATS = 2


def build():
    # FAT の大きさ: クラスタ数から逆算して 2 回ほど回す
    fatsz = 16
    for _ in range(4):
        data_sectors = PART_SECTORS - RESERVED - NFATS * fatsz
        clusters = data_sectors // SPC
        need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if need == fatsz:
            break
        fatsz = need
    data_start = PART_START + RESERVED + NFATS * fatsz
    n_clusters = (PART_SECTORS - RESERVED - NFATS * fatsz) // SPC

    img = bytearray(TOTAL_SECTORS * SECTOR)
    fat = [0] * (n_clusters + 2)
    fat[0] = 0x0FFFFFF8
    fat[1] = 0x0FFFFFFF
    fat[2] = 0x0FFFFFFF   # クラスタ 2 はルートディレクトリ (1 クラスタ)
    next_free = [3]

    def alloc_chain(nbytes):
        n = max(1, (nbytes + SPC * SECTOR - 1) // (SPC * SECTOR))
        first = next_free[0]
        for i in range(n):
            c = next_free[0]
            next_free[0] += 1
            fat[c] = 0x0FFFFFFF if i == n - 1 else c + 1
        return first

    def cluster_off(cluster):
        return (data_start + (cluster - 2) * SPC) * SECTOR

    def write_chain(first, data):
        c = first
        pos = 0
        while True:
            off = cluster_off(c)
            chunk = data[pos:pos + SPC * SECTOR]
            img[off:off + len(chunk)] = chunk
            pos += len(chunk)
            if fat[c] == 0x0FFFFFFF:
                break
            c = fat[c]

    def dir_entry(name83, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name83
        e[11] = attr
        struct.pack_into("<H", e, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    # HELLO.TXT: 行を積んで複数クラスタにまたがらせる
    lines = []
    for i in range(200):
        lines.append("SD line %04d: KosabaOS reads the SD card.\n" % i)
    lines.append("SD-END-MARKER\n")
    hello = "".join(lines).encode()
    hello_cluster = alloc_chain(len(hello))
    write_chain(hello_cluster, hello)

    # SUB/INNER.TXT
    inner = b"inner file of the SUB directory.\nSD-INNER-MARKER\n"
    inner_cluster = alloc_chain(len(inner))
    write_chain(inner_cluster, inner)
    sub_cluster = alloc_chain(64)
    sub_entries = b"".join([
        dir_entry(b".          ", 0x10, sub_cluster, 0),
        dir_entry(b"..         ", 0x10, 0, 0),
        dir_entry(b"INNER   TXT", 0x20, inner_cluster, len(inner)),
    ])
    write_chain(sub_cluster, sub_entries)

    # ルートディレクトリ (クラスタ 2)
    root_entries = b"".join([
        dir_entry(b"HELLO   TXT", 0x20, hello_cluster, len(hello)),
        dir_entry(b"SUB        ", 0x10, sub_cluster, 0),
    ])
    write_chain(2, root_entries)

    # FAT 領域へ書く (2 個とも同じ内容)
    fat_bytes = b"".join(struct.pack("<I", v & 0x0FFFFFFF) for v in fat)
    fat_bytes = fat_bytes[:fatsz * SECTOR].ljust(fatsz * SECTOR, b"\x00")
    for i in range(NFATS):
        off = (PART_START + RESERVED + i * fatsz) * SECTOR
        img[off:off + len(fat_bytes)] = fat_bytes

    # BPB (パーティション先頭セクタ)
    b = PART_START * SECTOR
    img[b:b + 3] = b"\xEB\x58\x90"
    img[b + 3:b + 11] = b"KOSABA  "
    struct.pack_into("<H", img, b + 11, SECTOR)     # セクタ長
    img[b + 13] = SPC
    struct.pack_into("<H", img, b + 14, RESERVED)
    img[b + 16] = NFATS
    struct.pack_into("<H", img, b + 17, 0)           # ルート数 (FAT32 は 0)
    struct.pack_into("<H", img, b + 19, 0)           # 総セクタ16
    img[b + 21] = 0xF8                               # メディア
    struct.pack_into("<H", img, b + 22, 0)           # FAT16 サイズ
    struct.pack_into("<H", img, b + 24, 63)          # ジオメトリ (ダミー)
    struct.pack_into("<H", img, b + 26, 255)
    struct.pack_into("<I", img, b + 28, PART_START)  # 隠しセクタ
    struct.pack_into("<I", img, b + 32, PART_SECTORS)
    struct.pack_into("<I", img, b + 36, fatsz)       # FAT32 サイズ
    struct.pack_into("<I", img, b + 44, 2)           # ルートクラスタ
    struct.pack_into("<H", img, b + 48, 1)           # FSInfo
    struct.pack_into("<H", img, b + 50, 6)           # バックアップ
    img[b + 64] = 0x80
    img[b + 66] = 0x29
    img[b + 71:b + 82] = b"KOSABA-SD  "
    img[b + 82:b + 90] = b"FAT32   "
    img[b + 510] = 0x55
    img[b + 511] = 0xAA

    # MBR
    struct.pack_into("<I", img, 446 + 8, PART_START)
    struct.pack_into("<I", img, 446 + 12, PART_SECTORS)
    img[446 + 4] = 0x0C                              # FAT32 (LBA)
    img[510] = 0x55
    img[511] = 0xAA
    return bytes(img)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "sd.img"
    with open(out, "wb") as f:
        f.write(build())
    print("wrote", out)


if __name__ == "__main__":
    main()
