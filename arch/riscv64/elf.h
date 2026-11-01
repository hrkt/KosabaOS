#ifndef KOSABAOS_ELF_H
#define KOSABAOS_ELF_H

#include <stdint.h>

/* ELF イメージをユーザープログラムの領域へ読み込み、
 * 入口アドレスを返す。失敗したら 0。実装は elf.c */
uint64_t elf_load(const unsigned char *image);

#endif
