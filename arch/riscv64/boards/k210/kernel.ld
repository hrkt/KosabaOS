/* kernel.ld (k210) -- K210 用の配置指示書
 *
 * 考え方は boards/virt/kernel.ld と同じ。違うのは置き場所だけで、
 * kflash -s の契約に合わせ、汎用 SRAM の先頭 0x80000000 に置く
 * (SRAM は 0x80000000 から 6MB)。ファームウェアは介在せず、
 * ROM の ISP スタブがこの住所へ直接ジャンプしてくるので、
 * ファイルの先頭 = _start でなければならない。
 *
 * メモリ配置の全体像 (移植設計文書の配置案):
 *   0x80000000  カーネル (1MB)
 *   0x80100000  ユーザーコード (1MB)   ※ステップ 5 以降
 *   0x80200000  ユーザーデータ (2MB)   ※同上
 *   0x80400000  ユーザースタック (1MB) ※同上
 *   0x80500000  予備 (1MB)
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
