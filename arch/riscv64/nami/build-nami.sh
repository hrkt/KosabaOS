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
# nami は MP3 デコーダ (mp3dec.c と表) を取り込んだため、
# 連結した 1 本の TU でビルドする。並びは Ikesu 側の
# mp3dec 連結と同じ (詰め物 → 表 → mp3dec.c → 本体)。
# 先頭の詰め物 g_tpad は、データ先頭に来る文字列プールの
# 後ろで表が 4 バイト境界からずれるのを寄せるためのもの
# (大きさはこの TU の実測に合わせる。doc/nami-design.md)。
# デコーダ状態の境界は nami.c 側の g_mpad が担う。
# どちらもずれると実機で misaligned トラップの洪水に
# なってデコードが桁違いに遅くなる (2026-11-21 の実測)。
# check-align.py がビルド時に両方の境界を検査する。
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

printf 'char g_tpad[8] = { 0 };\n' > nami_tpad.c
cat nami_tpad.c hufftab.c sfbtab.c powtab.c imdcttab.c \
    dcttab.c mp3dec.c nami.c > nami_all.c
"$IWAC_DIR/iwac" --kosaba --elf nami_all.c > nami.eo
python3 check-align.py nami.eo
"$IWAC_DIR/nori/nori" nami.eo > nami.elf
rm -f nami_tpad.c nami_all.c nami.eo
"$IWAC_DIR/iwac" --kosaba --elf wavgen.c > wavgen.eo
"$IWAC_DIR/nori/nori" wavgen.eo > wavgen.elf
rm -f wavgen.eo
ls -l nami.elf wavgen.elf
