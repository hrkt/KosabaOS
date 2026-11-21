/* boards/virt/smp.c -- SBI HSM で hart1 を起こす
 * (doc/smp-design.md 段1)
 *
 * OpenSBI は boot hart だけを S モードで渡し、他の
 * hart は止めてある。HSM 拡張 (EID 0x48534D = "HSM")
 * の hart_start (FID 0) で起こすと、相手は指定した
 * 物理アドレスから S モードで走り出す (a0 = hartid、
 * a1 = opaque)。opaque には hart0 の satp を渡し、
 * hart1_entry がそのまま自分の satp に入れる (ページ
 * 表は 1 枚を共有する、という前提どおり)。
 */

#include "board.h"
#include "smp.h"
#include "vm.h"

extern unsigned char hart1_entry[];

/* SBI 呼び出し (ecall)。戻り値は a0 = エラーコード */
static long sbi_hsm_hart_start(uint64_t hartid, uint64_t addr,
                               uint64_t opaque)
{
    register long r0 asm("a0") = (long)hartid;
    register long r1 asm("a1") = (long)addr;
    register long r2 asm("a2") = (long)opaque;
    register long r6 asm("a6") = 0;         /* FID 0 */
    register long r7 asm("a7") = 0x48534D;  /* EID "HSM" */
    asm volatile("ecall"
                 : "+r"(r0)
                 : "r"(r1), "r"(r2), "r"(r6), "r"(r7)
                 : "memory");
    return r0;
}

int board_start_hart1(void)
{
    /* カーネルの像は VA == PA で載っているので、入口の
     * リンク時アドレスがそのまま物理アドレスになる */
    long err = sbi_hsm_hart_start(1, (uint64_t)hart1_entry,
                                  vm_satp_value());
    return (int)err;
}
