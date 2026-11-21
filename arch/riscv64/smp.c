/* smp.c -- hart1 の起こしとハートビート (doc/smp-design.md 段1) */

#include "smp.h"
#include "board.h"
#include "klog.h"

struct hart_state hart1_state;

volatile uint64_t smp_heartbeat;
volatile uint64_t smp_hart1_alive;
volatile uint64_t smp_hart1_fault;

void smp_start(void)
{
    int rc = board_start_hart1();
    if (rc == 0) {
        kputs("smp: hart1 start requested\n");
    } else {
        kputs("smp: hart1 not started (");
        if (rc < 0) {
            kputc('-');
            kput_dec((uint64_t)(-(int64_t)rc));
        } else {
            kput_dec((uint64_t)rc);
        }
        kputs(")\n");
    }
}

void hart1_main(uint64_t hartid)
{
    hart1_state.hartid = hartid;
    smp_hart1_alive = 1;
    /* 生きている印に、数え続けるだけのループ。K210 では
     * この単純な受け渡しが「キャッシュされる SRAM で
     * 書いた値が相手に見えるか」の実測を兼ねる */
    for (;;) {
        smp_heartbeat++;
    }
}
