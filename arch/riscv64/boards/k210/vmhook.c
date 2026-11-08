/* vmhook.c -- vm_init へのボード固有の追加割り当て (k210)
 *
 * カーネルが使う MMIO は 0x502B0000 (FPIOA)、0x50440000
 * (SYSCTL)、0x52000000 (SPI0、LCD)、0x53000000 (SPI1、SD)、
 * 0x50250000 (I2S0、オーディオ) で、仮想アドレスでは
 * ユーザー領域と同じ root[1] の配下に
 * ある。vm.c の共通の地図には
 * 入っていないので、vm_init の途中 (satp を切り替える直前) に
 * ここで割り当ててもらう。UARTHS と GPIOHS は root[0] の
 * ギガページに含まれているので追加は要らない。
 * I2S0 は FPIOA と同じ 2MB ブロックの中にあるので、実は
 * FPIOA の割り当てでも届くが、使う以上は明示しておく
 * (同じブロックへの再割り当ては同じ葉を書くだけで無害)。 */

#include <stdint.h>
#include "vm.h"

void board_vm_hook(void)
{
    vm_map_device(0x502B0000);  /* FPIOA */
    vm_map_device(0x50440000);  /* SYSCTL */
    vm_map_device(0x52000000);  /* SPI0 */
    vm_map_device(0x53000000);  /* SPI1 (SD カード) */
    vm_map_device(0x50250000);  /* I2S0 (オーディオ) */
}
