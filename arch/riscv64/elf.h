#ifndef KOSABAOS_ELF_H
#define KOSABAOS_ELF_H

#include <stdint.h>

struct fs_file;
struct user_pages;

/* 位置指定読みの口。fs_read_at と fat_read_at はどちらも
 * この形 (file はそれぞれのファイル構造体へのポインタ)
 * なので、ローダは読み口を差し替えるだけで RAM FS と
 * SD (FAT) の両方から読める (doc/sd-exec-design.md) */
typedef uint64_t (*elf_read_at_fn)(void *file, uint64_t pos,
                                   unsigned char *dst, uint64_t n);

/* ELF イメージを、段のページ台帳 (vm.h) へ読み込み、入口
 * アドレスを返す。セグメントが触るページだけをプールから
 * 取る (exec 多段化の実需割り当て)。失敗したら 0 (途中で
 * 取ったページは呼び出し側が vm_pages_release で返す)。
 * 実装は elf.c */
uint64_t elf_load_src(void *file, elf_read_at_fn read_at,
                      struct user_pages *up);

/* FS のファイル (ELF イメージ) から読む従来の入口。
 * elf_load_src へ fs_read_at を渡す橋渡し */
uint64_t elf_load_to(struct fs_file *f, struct user_pages *up);

#endif
