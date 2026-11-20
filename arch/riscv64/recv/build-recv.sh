#!/bin/sh
# build-recv.sh -- recv を IwaC でビルドする
#
#   sh build-recv.sh     → recv.elf (このディレクトリ)
#
# 経路は iwac --kosaba --elf (ELF オブジェクト .eo) →
# nori (リンク)。nami/build-nami.sh と同じ流儀。
# ツールチェーンの場所は IWAC_DIR で差し替え可能
# (既定は ~/workspace/iwac/iwac)。
#
# 出来た recv.elf が OS 側の「素材」で、build.sh が
# あるときだけ -DRECV_EMBED で取り込む
# (doc/recv-design.md)。
set -e
cd "$(dirname "$0")"

IWAC_DIR=${IWAC_DIR:-../../../../../iwac/iwac}
if [ ! -x "$IWAC_DIR/iwac" ]; then
    (cd "$IWAC_DIR" && ./build.sh)
fi
if [ ! -x "$IWAC_DIR/nori/nori" ]; then
    (cd "$IWAC_DIR/nori" && ./build.sh)
fi

"$IWAC_DIR/iwac" --kosaba --elf recv.c > recv.eo
"$IWAC_DIR/nori/nori" recv.eo > recv.elf
rm -f recv.eo
ls -l recv.elf
