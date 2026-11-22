/* kernel.ld (k210) -- K210 用の配置指示書
 *
 * 考え方は boards/virt/kernel.ld と同じ。違うのは置き場所だけで、
 * kflash -s の契約に合わせ、汎用 SRAM の先頭 0x80000000 に置く
 * (SRAM は 0x80000000 から 6MB)。ファームウェアは介在せず、
 * ROM の ISP スタブがこの住所へ直接ジャンプしてくるので、
 * ファイルの先頭 = _start でなければならない。
 *
 * メモリ配置の全体像:
 *   0x80000000  カーネル (1MB の想定だったが、埋め込み素材や
 *               BSS が育ち、2026-11-22 時点で約 1.14MB)
 *   0x80100000  ページプール (0x80600000 まで):
 *               ユーザープログラムのページはここから実需で配る
 *               (exec 多段化。board.h の BOARD_PAGE_POOL_* を参照)。
 *               実効の先頭は __bss_end が 0x80100000 を越えたら
 *               page_init がイメージの後ろまでずらす (page.c)。
 *               「カーネルは 1MB に収まる」前提はもう無い
 */

ENTRY(_start)

SECTIONS {
  . = 0x80000000;

  /* _start を含む .text.start を真っ先に置き、続いて全部品の .text */
  .text : { *(.text.start) *(.text) }

  .rodata : { *(.rodata) }

  .data : { *(.data) *(.sdata) }

  .bss : {
    __bss_start = .;
    *(.bss) *(.sbss) *(COMMON)
    __bss_end = .;
  }
}
