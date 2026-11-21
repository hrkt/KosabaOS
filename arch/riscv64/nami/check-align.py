#!/usr/bin/env python3
# check-align.py -- nami.eo の配置境界を検査する
#
# nori はセクションを詰めて並べるだけで境界に寄せない
# ので、文字列プール (.iwac.strings) の大きさ次第で後ろ
# の配列 (.iwac.arrays) と、さらに後ろに置かれる g_mp3
# (COMMON) のアドレスが境界からずれる。ずれると実機
# (K210) で misaligned トラップの洪水になり、デコード
# が桁違いに遅くなる (2026-11-21 の実測が発端)。
# ここでは最終アドレスを
#   配列:   0x40200000 + strings_size + .eo 内オフセット
#   g_mp3:  0x40200000 + strings_size + arrays_size
# と再現して、int32 配列は 4 バイト境界、g_mp3 は 8
# バイト境界に乗っていることを確かめる。落ちたら
# build-nami.sh の g_tpad か nami.c の g_mpad の
# 大きさを調整すること。
import struct
import sys

DATA_BASE = 0x40200000


def main(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2  # ELF64
    (shoff,) = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)

    secs = []
    for i in range(shnum):
        off = shoff + i * shentsize
        name, typ, _flags, _addr, offset, size, link = \
            struct.unpack_from("<IIQQQQI", d, off)
        secs.append({"name": name, "type": typ, "off": offset,
                     "size": size, "link": link})

    strsec = secs[shstrndx]

    def sec_name(s):
        end = d.index(b"\0", strsec["off"] + s["name"])
        return d[strsec["off"] + s["name"]:end].decode()

    by_name = {sec_name(s): s for s in secs}
    strings_size = by_name[".iwac.strings"]["size"]
    arrays = by_name[".iwac.arrays"]
    arrays_size = arrays["size"]

    symtab = by_name[".symtab"]
    strtab = secs[symtab["link"]]
    syms = {}
    for off in range(symtab["off"], symtab["off"] + symtab["size"], 24):
        name, _info, _other, shndx, value, _size = \
            struct.unpack_from("<IBBHQQ", d, off)
        if name == 0:
            continue
        end = d.index(b"\0", strtab["off"] + name)
        syms[d[strtab["off"] + name:end].decode()] = (shndx, value)

    ng = []

    def check(label, addr, align):
        r = addr % align
        print(f"{label}: 0x{addr:x} (mod {align} = {r})")
        if r != 0:
            ng.append(label)

    for name in ("synth_d", "synth_row", "cbrt_q24"):
        shndx, value = syms[name]
        check(name, DATA_BASE + strings_size + value, 4)
    check("g_mp3 base", DATA_BASE + strings_size + arrays_size, 8)

    if ng:
        print("MISALIGNED:", ", ".join(ng))
        print("g_tpad (build-nami.sh) か g_mpad (nami.c) の"
              "大きさを調整すること")
        return 1
    print("alignment ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "nami.eo"))
