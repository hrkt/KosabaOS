/* embed.S -- ユーザープログラムをカーネルに埋め込む
 * (この行を # で始めると C23 の #embed 指示と誤認されるので注意) */
# ファイルシステムがまだ無い (RAM FS の初期ファイルとして運ぶ
# ため) ので、ユーザープログラムの ELF ファイルはカーネルの
# .rodata の一部として一緒に運ぶ。fs.c がこれらを uprog.elf /
# shell.elf という名前でファイルシステムに登録する。
# どちらも build.sh が先に作る (生成物なので .gitignore で除外)。

    .section .rodata
    .globl user_prog_start
    .globl user_prog_end
user_prog_start:
    .incbin "user.elf"
user_prog_end:

    .globl nish_prog_start
    .globl nish_prog_end
nish_prog_start:
    .incbin "nish.elf"
nish_prog_end:

    .globl chain_prog_start
    .globl chain_prog_end
chain_prog_start:
    .incbin "chain.elf"
chain_prog_end:

    .globl eat_prog_start
    .globl eat_prog_end
eat_prog_start:
    .incbin "eat.elf"
eat_prog_end:

    .globl uptime_prog_start
    .globl uptime_prog_end
uptime_prog_start:
    .incbin "uptime.elf"
uptime_prog_end:

    .globl perf_prog_start
    .globl perf_prog_end
perf_prog_start:
    .incbin "perf.elf"
perf_prog_end:

    .globl ima_prog_start
    .globl ima_prog_end
ima_prog_start:
    .incbin "ima.elf"
ima_prog_end:

    .globl clock_prog_start
    .globl clock_prog_end
clock_prog_start:
    .incbin "clock.elf"
clock_prog_end:

    .globl saver_prog_start
    .globl saver_prog_end
saver_prog_start:
    .incbin "saver.elf"
saver_prog_end:

    .globl getty_prog_start
    .globl getty_prog_end
getty_prog_start:
    .incbin "getty.elf"
getty_prog_end:

    .globl seek_prog_start
    .globl seek_prog_end
seek_prog_start:
    .incbin "seek.elf"
seek_prog_end:

    .globl oled_prog_start
    .globl oled_prog_end
oled_prog_start:
    .incbin "oled.elf"
oled_prog_end:

    .globl vol_prog_start
    .globl vol_prog_end
vol_prog_start:
    .incbin "vol.elf"
vol_prog_end:

    .globl misalign_prog_start
    .globl misalign_prog_end
misalign_prog_start:
    .incbin "misalign.elf"
misalign_prog_end:

    .globl badinsn_prog_start
    .globl badinsn_prog_end
badinsn_prog_start:
    .incbin "badinsn.elf"
badinsn_prog_end:

    .globl atest_prog_start
    .globl atest_prog_end
atest_prog_start:
    .incbin "atest.elf"
atest_prog_end:

    .globl fptest_prog_start
    .globl fptest_prog_end
fptest_prog_start:
    .incbin "fptest.elf"
fptest_prog_end:
    .globl tone_prog_start
    .globl tone_prog_end
tone_prog_start:
    .incbin "tone.elf"
tone_prog_end:

    .globl slow_prog_start
    .globl slow_prog_end
slow_prog_start:
    .incbin "slow.elf"
slow_prog_end:

    .globl crash_prog_start
    .globl crash_prog_end
crash_prog_start:
    .incbin "crash.elf"
crash_prog_end:

    .globl nap_prog_start
    .globl nap_prog_end
nap_prog_start:
    .incbin "nap.elf"
nap_prog_end:

    .globl keywait_prog_start
    .globl keywait_prog_end
keywait_prog_start:
    .incbin "keywait.elf"
keywait_prog_end:

    .globl top_prog_start
    .globl top_prog_end
top_prog_start:
    .incbin "top.elf"
top_prog_end:

    .globl copy_prog_start
    .globl copy_prog_end
copy_prog_start:
    .incbin "copy.elf"
copy_prog_end:
    .globl gdemo_prog_start
    .globl gdemo_prog_end
gdemo_prog_start:
    .incbin "gdemo.elf"
gdemo_prog_end:

    .globl gjob_prog_start
    .globl gjob_prog_end
gjob_prog_start:
    .incbin "gjob.elf"
gjob_prog_end:

/* Ikesu 合流テストの素材 (test.sh シナリオ U)。リポジトリの
 * 外にある Ikesu のビルド成果物を build.sh が写してきた
 * ときだけ (-DIKESU_EMBED)、virt 向けに取り込む */
#ifdef IKESU_EMBED
    .globl ikesu_prog_start
    .globl ikesu_prog_end
ikesu_prog_start:
    .incbin "ikesu.elf"
ikesu_prog_end:

    .globl player_wasm_start
    .globl player_wasm_end
player_wasm_start:
    .incbin "player.wasm"
player_wasm_end:

    .globl test_mp3_start
    .globl test_mp3_end
test_mp3_start:
    .incbin "test.mp3"
test_mp3_end:
#endif

/* nami (音声ファイル再生プレーヤー) の素材。IwaC 製の
 * ビルド成果物を build.sh が写してきたときだけ
 * (-DNAMI_EMBED) 取り込む (doc/nami-design.md) */
#ifdef NAMI_EMBED
    .globl nami_prog_start
    .globl nami_prog_end
nami_prog_start:
    .incbin "nami.elf"
nami_prog_end:

    .globl wavgen_prog_start
    .globl wavgen_prog_end
wavgen_prog_start:
    .incbin "wavgen.elf"
wavgen_prog_end:
#endif

/* recv (シリアル受信) の素材。IwaC 製のビルド成果物を
 * build.sh が写してきたときだけ (-DRECV_EMBED) 取り込む
 * (doc/recv-design.md) */
#ifdef RECV_EMBED
    .globl recv_prog_start
    .globl recv_prog_end
recv_prog_start:
    .incbin "recv.elf"
recv_prog_end:
#endif

/* synth (シンセ音源) の素材。IwaC 製のビルド成果物を
 * build.sh が写してきたときだけ (-DSYNTH_EMBED) 取り込む
 * (doc/synth-design.md) */
#ifdef SYNTH_EMBED
    .globl synth_prog_start
    .globl synth_prog_end
synth_prog_start:
    .incbin "synth.elf"
synth_prog_end:
#endif

/* smfplay (SMF プレーヤー) の素材。synth と同じ流儀で、
 * build.sh が写してきたときだけ (-DSMFPLAY_EMBED) 取り込む
 * (doc/synth-design.md) */
#ifdef SMFPLAY_EMBED
    .globl smfplay_prog_start
    .globl smfplay_prog_end
smfplay_prog_start:
    .incbin "smfplay.elf"
smfplay_prog_end:
#endif
