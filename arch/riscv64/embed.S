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
