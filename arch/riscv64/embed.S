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

<<<<<<< HEAD
    .globl copy_prog_start
    .globl copy_prog_end
copy_prog_start:
    .incbin "copy.elf"
copy_prog_end:
=======
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
>>>>>>> gfx-windows

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
