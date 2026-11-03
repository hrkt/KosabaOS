/* elf.c -- ELF ローダ (ステップ 6 の最初の一歩)
 *
 * ELF は「プログラムのファイル形式」。中身は:
 *   先頭のヘッダ: 入口アドレス (e_entry) と、後述の表の場所
 *   プログラムヘッダ表: 「ファイルのこの範囲を、メモリの
 *     この住所へ、この長さで置く」という指示 (セグメント) の一覧
 * ローダの仕事は、表の指示どおりにバイト列をメモリへ
 * コピーし (足りない分はゼロ埋め = .bss)、入口を返すこと。
 * これで「平らなバイナリを先頭から実行」という前提を卒業し、
 * ファイルが自分の配置を申告する形になる。
 *
 * 今回読むのは、自分たちの user.elf に必要な最小限:
 * ELF64 / リトルエンディアン / 実行形式 (ET_EXEC) / RISC-V、
 * PT_LOAD セグメントだけを対象にする。
 */

#include <stdint.h>
#include "elf.h"
#include "uart.h"
#include "vm.h"

struct elf64_header {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

#define PT_LOAD 1
#define ET_EXEC 2
#define EM_RISCV 0xF3

/* ユーザープログラムの各領域の大きさ (megapage 1 枚分) */
#define REGION_SIZE BOARD_REGION_SIZE  /* 1 領域の上限 (ボード依存) */

uint64_t elf_load_to(const unsigned char *image, uint64_t code_pa,
                     uint64_t data_pa)
{
    const struct elf64_header *h = (const struct elf64_header *)image;
    int i;

    /* ヘッダの確認: マジック (0x7F 'E' 'L' 'F')、64 ビット、
     * リトルエンディアン、実行形式、RISC-V */
    if (!(h->e_ident[0] == 0x7F && h->e_ident[1] == 'E' &&
          h->e_ident[2] == 'L' && h->e_ident[3] == 'F')) {
        uart_puts("[elf] bad magic\n");
        return 0;
    }
    if (h->e_ident[4] != 2 || h->e_ident[5] != 1) {
        uart_puts("[elf] not a 64-bit little-endian ELF\n");
        return 0;
    }
    if (h->e_type != ET_EXEC || h->e_machine != EM_RISCV) {
        uart_puts("[elf] not a RISC-V executable\n");
        return 0;
    }

    /* プログラムヘッダ表をたどり、PT_LOAD を置いていく */
    for (i = 0; i < h->e_phnum; i++) {
        const struct elf64_phdr *p =
            (const struct elf64_phdr *)(image + h->e_phoff +
                                         (uint64_t)i * h->e_phentsize);
        unsigned char *dst;
        uint64_t j;

        if (p->p_type != PT_LOAD) {
            continue;
        }
        /* 行き先はコード領域かデータ領域の中だけに限る。
         * どちらに入るかで置く物理メモリが変わる */
        if (p->p_vaddr >= USER_CODE_VA &&
            p->p_vaddr + p->p_memsz <= USER_CODE_VA + REGION_SIZE) {
            dst = (unsigned char *)(code_pa + (p->p_vaddr - USER_CODE_VA));
        } else if (p->p_vaddr >= USER_DATA_VA &&
                   p->p_vaddr + p->p_memsz <= USER_DATA_VA + REGION_SIZE) {
            dst = (unsigned char *)(data_pa + (p->p_vaddr - USER_DATA_VA));
        } else {
            uart_puts("[elf] segment out of user range\n");
            return 0;
        }
        for (j = 0; j < p->p_filesz; j++) {
            dst[j] = image[p->p_offset + j];
        }
        for (; j < p->p_memsz; j++) {
            dst[j] = 0;  /* ファイルに無い分はゼロ埋め (.bss) */
        }
    }

    /* 書いた命令を命令キャッシュ側へ反映させる */
    asm volatile("fence.i");
    return h->e_entry;
}

uint64_t elf_load(const unsigned char *image)
{
    return elf_load_to(image, USER_CODE_PA, USER_DATA_PA);
}
