/* embed.S -- ユーザープログラムをカーネルに埋め込む
 * (この行を # で始めると C23 の #embed 指示と誤認されるので注意) */
# ファイルシステムがまだ無いので、ユーザープログラムの
# ELF ファイル (user.elf) はカーネルの .rodata の一部として
# 一緒に運ぶ。実行時は elf.c のローダがこのイメージを読んで
# ユーザープログラムの領域へ配置する。
# user.elf は build.sh が uprog.c から先に作る (生成物なので
# .gitignore で除外される)。

    .section .rodata
    .globl user_prog_start
    .globl user_prog_end
user_prog_start:
    .incbin "user.elf"
user_prog_end:
