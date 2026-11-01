/* embed.S -- ユーザープログラムのバイナリをカーネルに埋め込む
 * (この行を # で始めると C23 の #embed 指示と誤認されるので注意) */
# ファイルシステムがまだ無いので、ユーザープログラム (user.bin)
# はカーネルの .rodata の一部として一緒に運ぶ。カーネルは起動後に
# user_prog_start 〜 user_prog_end の範囲を、ユーザープログラム用
# の物理メモリへコピーしてから実行する。
# user.bin は build.sh が uprog.c から先に作る (生成物なので
# .gitignore で除外される)。

    .section .rodata
    .globl user_prog_start
    .globl user_prog_end
user_prog_start:
    .incbin "user.bin"
user_prog_end:
