/* user.ld -- ユーザープログラム用の配置指示書
 *
 * カーネルの kernel.ld と対になる、ユーザープログラム側の約束。
 * カーネルはユーザープログラム用に 2MB の領域を 3 枚用意する:
 *   0x40000000  コード (.text / .rodata)  読み + 実行
 *   0x40200000  データ (.data / .bss)     読み書き
 *   0x40400000  スタック                  読み書き
 * コードと書き換える静的データを別の住所に分ける (W^X) ので、
 * セグメントごとに行き先の領域が変わり、ELF ローダが仮想
 * アドレスを見て振り分ける。
 */

ENTRY(_start)

SECTIONS {
  . = 0x40000000;

  /* 入口 (_start の .text.start) を必ず先頭に置く */
  .text : { *(.text.start) *(.text) *(.text.*) }
  .rodata : { *(.rodata) *(.rodata.*) }

  . = 0x40200000;

  .data : { *(.data) *(.data.*) }
  .bss : { *(.bss) *(.bss.*) *(COMMON) }
}
