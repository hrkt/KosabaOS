#!/usr/bin/env python3
"""FAT32 イメージの独立検証 (check_sd_image.py)。

make_sd_image.py が作ったイメージや、KosabaOS の FAT
ドライバ (fat.c) をホスト側で駆動して書き込んだイメージを、
FAT32 の形式だけから読み直して整合を確かめる。fat.c とは
独立に書いてあり、両者の食い違いを見つけるのが目的
(fsck.fat が使えない環境での代替、doc/sd-write-design.md)。

検査するもの:
  - MBR / BPB の形、FAT が全ての面で一致すること
  - 全ファイル・ディレクトリの鎖が範囲内・循環なし・
    他の鎖と共有 (クロスリンク) していないこと
  - 割り当て済みクラスタがすべて何かの鎖から辿れること
    (迷子クラスタが無いこと)
  - ファイルのサイズが鎖の容量に収まること
  - FSInfo の空きクラスタ数が実態と一致すること

使い方:
  check_sd_image.py IMAGE [--file PATH=HOSTFILE] [--absent PATH]
  --file はそのファイルの中身が HOSTFILE とバイト一致する
  ことまで見る。PATH はルートからの '/' 区切り (例: SUB/W2.TXT)
"""

import hashlib
import struct
import sys

SECTOR = 512
errors = []


def fail(msg):
    errors.append(msg)
    print("FAIL:", msg)


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Image:
    def __init__(self, data):
        self.data = data
        if data[82:85] == b"FAT":
            self.part = 0  # スーパーフロッピー形式
        else:
            if data[510] != 0x55 or data[511] != 0xAA:
                fail("MBR 署名が無い")
                raise SystemExit(1)
            ptype = data[446 + 4]
            if ptype not in (0x0B, 0x0C):
                fail("パーティション種別が FAT32 ではない: %#x" % ptype)
                raise SystemExit(1)
            self.part = u32(data, 446 + 8)
        b = self.part * SECTOR
        if u16(data, b + 11) != SECTOR:
            fail("セクタ長が 512 ではない")
            raise SystemExit(1)
        self.spc = data[b + 13]
        reserved = u16(data, b + 14)
        self.nfats = data[b + 16]
        total = u32(data, b + 32) or u16(data, b + 19)
        self.fatsz = u32(data, b + 36)
        self.root_cluster = u32(data, b + 44)
        self.fsinfo_sec = u16(data, b + 48)
        self.fat_start = self.part + reserved
        self.data_start = self.fat_start + self.nfats * self.fatsz
        self.total_clusters = (total - (self.data_start - self.part)) \
            // self.spc
        fat0 = self.sector_bytes(self.fat_start, self.fatsz)
        for i in range(1, self.nfats):
            other = self.sector_bytes(self.fat_start + i * self.fatsz,
                                      self.fatsz)
            if other != fat0:
                fail("FAT の面 %d が面 0 と一致しない" % i)
        self.fat = fat0

    def sector_bytes(self, lba, count):
        off = lba * SECTOR
        return self.data[off:off + count * SECTOR]

    def entry(self, cluster):
        return u32(self.fat, cluster * 4) & 0x0FFFFFFF

    def cluster_bytes(self, cluster):
        lba = self.data_start + (cluster - 2) * self.spc
        return self.sector_bytes(lba, self.spc)

    def chain(self, first, owner):
        """鎖をたどって (クラスタ列, 連結バイト列) を返す"""
        clusters = []
        seen = set()
        c = first
        while True:
            if c < 2 or c >= self.total_clusters + 2:
                fail("%s: 鎖が範囲外のクラスタ %d を指す" % (owner, c))
                break
            if c in seen:
                fail("%s: 鎖が循環している (クラスタ %d)" % (owner, c))
                break
            seen.add(c)
            clusters.append(c)
            nxt = self.entry(c)
            if nxt >= 0x0FFFFFF8:
                break
            if nxt == 0:
                fail("%s: 鎖の途中で空きクラスタ %d に出た"
                     % (owner, c))
                break
            c = nxt
        return clusters, b"".join(self.cluster_bytes(c) for c in clusters)


def fmt_time(date, time):
    if date == 0:
        return "0"
    y = 1980 + (date >> 9)
    mo = (date >> 5) & 0xF
    d = date & 0x1F
    h = time >> 11
    mi = (time >> 5) & 0x3F
    s = (time & 0x1F) * 2
    return "%04d-%02d-%02d %02d:%02d:%02d" % (y, mo, d, h, mi, s)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    expect_files = {}
    expect_absent = []
    i = 1
    while i < len(args):
        if args[i] == "--file":
            name, host = args[i + 1].split("=", 1)
            with open(host, "rb") as f:
                expect_files[name] = f.read()
            i += 2
        elif args[i] == "--absent":
            expect_absent.append(args[i + 1])
            i += 2
        else:
            print("unknown arg:", args[i])
            return 2

    with open(path, "rb") as f:
        img = Image(f.read())

    owners = {}   # クラスタ -> 持ち主のパス
    files = {}    # パス -> (size, 中身, mtime 文字列)

    def claim(clusters, owner):
        for c in clusters:
            if c in owners:
                fail("クロスリンク: クラスタ %d が %s と %s で共有"
                     % (c, owners[c], owner))
            owners[c] = owner

    def walk_dir(dir_cluster, prefix):
        clusters, raw = img.chain(dir_cluster, prefix or "/")
        claim(clusters, prefix or "/")
        pos = 0
        while pos + 32 <= len(raw):
            e = raw[pos:pos + 32]
            pos += 32
            if e[0] == 0x00:
                break
            if e[0] == 0xE5:
                continue
            attr = e[11]
            if attr == 0x0F or (attr & 0x08):
                continue  # LFN とボリュームラベルは対象外
            name = e[0:8].decode("ascii", "replace").rstrip()
            ext = e[8:11].decode("ascii", "replace").rstrip()
            if ext:
                name += "." + ext
            fc = (u16(e, 20) << 16) | u16(e, 26)
            size = u32(e, 28)
            mtime = fmt_time(u16(e, 24), u16(e, 22))
            full = prefix + name
            if name in (".", ".."):
                continue
            if attr & 0x10:
                print("DIR  %s/ cluster=%d" % (full, fc))
                if fc == 0:
                    fail("%s: ディレクトリなのにクラスタが無い" % full)
                else:
                    walk_dir(fc, full + "/")
            else:
                if size > 0 and fc == 0:
                    fail("%s: サイズ %d なのにクラスタが無い"
                         % (full, size))
                content = b""
                if fc != 0:
                    c2, raw2 = img.chain(fc, full)
                    claim(c2, full)
                    if size > len(raw2):
                        fail("%s: サイズ %d が鎖の容量 %d を超える"
                             % (full, size, len(raw2)))
                    content = raw2[:size]
                digest = hashlib.sha256(content).hexdigest()[:16]
                print("FILE %s size=%d cluster=%d sha256=%s mtime=%s"
                      % (full, size, fc, digest, mtime))
                files[full] = content

    walk_dir(img.root_cluster, "")

    # 迷子クラスタ: 割り当て済みなのにどの鎖からも辿れない
    for c in range(2, img.total_clusters + 2):
        if img.entry(c) != 0 and c not in owners:
            fail("迷子クラスタ %d (FAT 項 %#x)" % (c, img.entry(c)))

    # FSInfo の空き数と実態の一致
    actual_free = sum(1 for c in range(2, img.total_clusters + 2)
                      if img.entry(c) == 0)
    if img.fsinfo_sec:
        off = (img.part + img.fsinfo_sec) * SECTOR
        d = img.data
        if u32(d, off) == 0x41615252 and u32(d, off + 484) == 0x61417272 \
                and u32(d, off + 508) == 0xAA550000:
            recorded = u32(d, off + 488)
            print("FSInfo free=%d actual=%d" % (recorded, actual_free))
            if recorded != 0xFFFFFFFF and recorded != actual_free:
                fail("FSInfo の空き数 %d が実態 %d と合わない"
                     % (recorded, actual_free))
        else:
            print("FSInfo: 署名なし (記録は信用しない)")

    for name, want in expect_files.items():
        if name not in files:
            fail("期待したファイル %s が無い" % name)
        elif files[name] != want:
            fail("%s: 中身が一致しない (%d バイト vs 期待 %d バイト)"
                 % (name, len(files[name]), len(want)))
        else:
            print("MATCH %s (%d バイト)" % (name, len(want)))
    for name in expect_absent:
        if name in files:
            fail("消えたはずの %s が残っている" % name)
        else:
            print("ABSENT %s" % name)

    if errors:
        print("NG: %d 件の問題" % len(errors))
        return 1
    print("OK: 整合 (%d ファイル, 空き %d クラスタ)"
          % (len(files), actual_free))
    return 0


if __name__ == "__main__":
    sys.exit(main())
