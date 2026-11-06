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
 *
 * ファイルからの読み出しは fs_read_at (位置指定) 経由。
 * FS のブロックチェーン化 (doc/fs-design.md 段1) で、
 * ファイルのデータは連続とは限らなくなったため。
 *
 * 置き先は「段のページ台帳」(vm.h) で、物理メモリは連続とは
 * 限らない。セグメントが触るページだけをプール (page.c) から
 * 取って台帳に記録し、コピーはページ境界で区切りながら行う
 * (exec 多段化、doc/exec-multilevel.md)。page_alloc がゼロ埋め
 * して渡すので、ファイルに無い分 (.bss) は自動でゼロになる。
 */

#include <stdint.h>
#include "elf.h"
#include "klog.h"
#include "vm.h"
#include "page.h"
#include "fs.h"

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

/* ユーザープログラムの各領域の大きさはボード依存 (board.h の
 * BOARD_CODE_SIZE / BOARD_DATA_SIZE)。コードとデータで大きさが
 * 違うボード (virt はデータ 4MB) があるため、範囲チェックと
 * 台帳の列は領域ごとに分ける */

uint64_t elf_load_to(struct fs_file *f, struct user_pages *up)
{
    struct elf64_header h;
    int i;

    /* ヘッダを読む。短ければ ELF ではない */
    if (fs_read_at(f, 0, (unsigned char *)&h, sizeof(h)) != sizeof(h)) {
        kputs("[elf] bad magic\n");
        return 0;
    }

    /* ヘッダの確認: マジック (0x7F 'E' 'L' 'F')、64 ビット、
     * リトルエンディアン、実行形式、RISC-V */
    if (!(h.e_ident[0] == 0x7F && h.e_ident[1] == 'E' &&
          h.e_ident[2] == 'L' && h.e_ident[3] == 'F')) {
        kputs("[elf] bad magic\n");
        return 0;
    }
    if (h.e_ident[4] != 2 || h.e_ident[5] != 1) {
        kputs("[elf] not a 64-bit little-endian ELF\n");
        return 0;
    }
    if (h.e_type != ET_EXEC || h.e_machine != EM_RISCV) {
        kputs("[elf] not a RISC-V executable\n");
        return 0;
    }

    /* プログラムヘッダ表をたどり、PT_LOAD を置いていく */
    for (i = 0; i < h.e_phnum; i++) {
        struct elf64_phdr p;
        uint64_t *pages;   /* この領域の台帳の列 */
        uint64_t base;     /* 領域の先頭 VA */
        uint64_t extent;   /* セグメントが触る長さ (memsz と filesz の大きい方) */
        uint64_t done;

        if (fs_read_at(f, h.e_phoff + (uint64_t)i * h.e_phentsize,
                       (unsigned char *)&p, sizeof(p)) != sizeof(p)) {
            kputs("[elf] truncated program header\n");
            return 0;
        }
        if (p.p_type != PT_LOAD) {
            continue;
        }
        /* 行き先はコード領域かデータ領域の中だけに限る。
         * どちらに入るかで台帳の列が変わる */
        extent = p.p_memsz > p.p_filesz ? p.p_memsz : p.p_filesz;
        if (p.p_vaddr >= USER_CODE_VA &&
            p.p_vaddr + extent <= USER_CODE_VA + BOARD_CODE_SIZE) {
            pages = up->code;
            base = USER_CODE_VA;
        } else if (p.p_vaddr >= USER_DATA_VA &&
                   p.p_vaddr + extent <= USER_DATA_VA + BOARD_DATA_SIZE) {
            pages = up->data;
            base = USER_DATA_VA;
        } else {
            kputs("[elf] segment out of user range\n");
            return 0;
        }
        /* セグメントが触るページをプールから取る。既に取って
         * あるページはそのまま使う (セグメント同士でページを
         * 共有する場合)。取るページはゼロ埋め済みなので、
         * ファイルに無い分 (.bss) は改めて触らなくてよい */
        if (extent > 0) {
            uint64_t first = (p.p_vaddr - base) / 4096;
            uint64_t last = (p.p_vaddr + extent - 1 - base) / 4096;
            uint64_t idx;
            for (idx = first; idx <= last; idx++) {
                if (pages[idx] == 0) {
                    uint64_t pa = page_alloc();
                    if (pa == 0) {
                        /* プール枯渇。exec の戻り値 (-1) では
                         * 理由が伝わらないので、残量つきで
                         * ログに残す (dmesg で追える) */
                        kputs("[elf] out of pages (pool free ");
                        kput_dec(page_free_count());
                        kputs(")\n");
                        return 0;
                    }
                    pages[idx] = pa;
                }
            }
        }
        /* ファイルにある分を、ページ境界で区切りながら運ぶ
         * (物理ページは連続とは限らないため) */
        done = 0;
        while (done < p.p_filesz) {
            unsigned char buf[512];
            uint64_t va = p.p_vaddr + done;
            uint64_t pa = vm_pages_va_to_pa(up, va);
            uint64_t chunk = p.p_filesz - done;
            uint64_t room = 4096 - (va & 4095);
            uint64_t got, k;
            if (pa == 0) {
                kputs("[elf] segment page missing\n");
                return 0;
            }
            if (chunk > room) {
                chunk = room;
            }
            if (chunk > sizeof(buf)) {
                chunk = sizeof(buf);
            }
            got = fs_read_at(f, p.p_offset + done, buf, chunk);
            if (got == 0) {
                kputs("[elf] truncated segment\n");
                return 0;
            }
            for (k = 0; k < got; k++) {
                *(unsigned char *)(pa + k) = buf[k];
            }
            done += got;
        }
    }

    /* 書いた命令を命令キャッシュ側へ反映させる */
    asm volatile("fence.i");
    return h.e_entry;
}
