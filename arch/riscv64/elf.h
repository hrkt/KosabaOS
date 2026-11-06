#ifndef KOSABAOS_ELF_H
#define KOSABAOS_ELF_H

#include <stdint.h>

struct fs_file;
struct user_pages;

/* FS のファイル (ELF イメージ) を、段のページ台帳 (vm.h) へ
 * 読み込み、入口アドレスを返す。セグメントが触るページだけを
 * プールから取る (exec 多段化の実需割り当て)。失敗したら 0
 * (途中で取ったページは呼び出し側が vm_pages_release で返す)。
 * 実装は elf.c。読み出しは fs_read_at 経由 (ファイルのデータは
 * 連続とは限らないため) */
uint64_t elf_load_to(struct fs_file *f, struct user_pages *up);

#endif
