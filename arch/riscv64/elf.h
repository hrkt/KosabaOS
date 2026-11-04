#ifndef KOSABAOS_ELF_H
#define KOSABAOS_ELF_H

#include <stdint.h>

struct fs_file;

/* FS のファイル (ELF イメージ) をユーザープログラムの領域へ
 * 読み込み、入口アドレスを返す。失敗したら 0。実装は elf.c。
 * 読み出しは fs_read_at 経由 (ファイルのデータは連続とは
 * 限らないため) */
uint64_t elf_load(struct fs_file *f);
uint64_t elf_load_to(struct fs_file *f, uint64_t code_pa,
                     uint64_t data_pa);

#endif
