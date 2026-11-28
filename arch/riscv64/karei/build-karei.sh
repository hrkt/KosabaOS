#!/bin/sh
# build-karei.sh -- karei と smfplay を IwaC でビルドする
#
#   sh build-karei.sh     → karei.elf / smfplay.elf (このディレクトリ)
#
# 経路は iwac --kosaba --elf (ELF オブジェクト .eo) →
# nori (リンク)。nami/build-nami.sh と同じ流儀。
# ツールチェーンの場所は IWAC_DIR で差し替え可能
# (既定は ~/workspace/iwac/iwac)。
#
# 出来た karei.elf が OS 側の「素材」で、build.sh が
# あるときだけ -DKAREI_EMBED で取り込む
# (doc/karei-design.md)。
set -e
cd "$(dirname "$0")"

IWAC_DIR=${IWAC_DIR:-../../../../../iwac/iwac}
if [ ! -x "$IWAC_DIR/iwac" ]; then
    (cd "$IWAC_DIR" && ./build.sh)
fi
if [ ! -x "$IWAC_DIR/nori/nori" ]; then
    (cd "$IWAC_DIR/nori" && ./build.sh)
fi

"$IWAC_DIR/iwac" --kosaba --elf karei.c > karei.eo
"$IWAC_DIR/nori/nori" karei.eo > karei.elf
rm -f karei.eo
"$IWAC_DIR/iwac" --kosaba --elf smfplay.c > smfplay.eo
"$IWAC_DIR/nori/nori" smfplay.eo > smfplay.elf
rm -f smfplay.eo
ls -l karei.elf smfplay.elf
