/* vmhook.c -- vm_init へのボード固有の追加割り当て (k210)
 *
 * LCD ドライバが使う MMIO は 0x502B0000 (FPIOA)、0x50440000
 * (SYSCTL)、0x52000000 (SPI0) で、仮想アドレスではユーザー
 * 領域と同じ root[1] の配下にある。vm.c の共通の地図には
 * 入っていないので、vm_init の途中 (satp を切り替える直前) に
 * ここで割り当ててもらう。UARTHS と GPIOHS は root[0] の
 * ギガページに含まれているので追加は要らない。 */

#include <stdint.h>
#include "vm.h"

void board_vm_hook(void)
{
    vm_map_device(0x502B0000);  /* FPIOA */
    vm_map_device(0x50440000);  /* SYSCTL */
    vm_map_device(0x52000000);  /* SPI0 */
}
