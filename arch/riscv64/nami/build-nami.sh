#!/bin/sh
# build-nami.sh -- nami と wavgen を IwaC でビルドする
#
#   sh build-nami.sh     → nami.elf / wavgen.elf (この
#                          ディレクトリにできる)
#
# 経路は iwac --kosaba --elf (ELF オブジェクト .eo) →
# nori (リンク) で、IwaC のチェーンそのまま。入口と
# syscall の作法は Ikesu 段6 の main_kosaba.c が前例。
#
# ツールチェーンの場所は IWAC_DIR で差し替え可能
# (既定は兄弟ディレクトリの ~/workspace/iwac/iwac、
# OS 側 build.sh の IKESU_DIR と同じ流儀)。iwac /
# nori のバイナリが無ければ、それぞれの build.sh で
# 作る。
#
# 出来た nami.elf / wavgen.elf が OS 側の「素材」で、
# build.sh がそろっているときだけ -DNAMI_EMBED で
# 取り込む (doc/nami-design.md)。
set -e
cd "$(dirname "$0")"

IWAC_DIR=${IWAC_DIR:-../../../../../iwac/iwac}
if [ ! -x "$IWAC_DIR/iwac" ]; then
    (cd "$IWAC_DIR" && ./build.sh)
fi
if [ ! -x "$IWAC_DIR/nori/nori" ]; then
    (cd "$IWAC_DIR/nori" && ./build.sh)
fi

"$IWAC_DIR/iwac" --kosaba --elf nami.c > nami.eo
"$IWAC_DIR/nori/nori" nami.eo > nami.elf
"$IWAC_DIR/iwac" --kosaba --elf wavgen.c > wavgen.eo
"$IWAC_DIR/nori/nori" wavgen.eo > wavgen.elf
rm -f nami.eo wavgen.eo
ls -l nami.elf wavgen.elf
