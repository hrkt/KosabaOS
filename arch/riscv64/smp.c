/* smp.c -- hart1 の起こしとハートビート (doc/smp-design.md 段1) */

#include "smp.h"
#include "board.h"
#include "klog.h"
#include "audio.h"

struct hart_state hart1_state;

volatile uint64_t smp_heartbeat;
volatile uint64_t smp_hart1_alive;
volatile uint64_t smp_hart1_fault;

/* キャッシュ追い出し用のダミー領域 (128KB)。K210 の
 * D キャッシュ (32KB と言われる) より大きく流し読み
 * すると、自ハートのキャッシュラインが全部入れ替わり、
 * dirty な行はメモリへ書き戻され、stale な行は捨て
 * られる。K210 のキャッシュはハート間でコヒーレント
 * でないので、ハート間の受け渡しでは「書いた側が追い
 * 出して書き戻す」「読む側が追い出して読み直す」が
 * 段1 の実測で得た作法 (doc/smp-design.md)。
 * virt では無害な空転になる */
static char evbuf[131072];

void smp_evict_cache(void)
{
    volatile char *p = evbuf;
    uint64_t sum = 0;
    int i;
    for (i = 0; i < 131072; i += 64) {
        sum += (uint64_t)p[i];
    }
    (void)sum;
}

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
    board_hart1_mark(0xC3);  /* S の hart1_main に着いた */
    smp_hart1_alive = 1;
    for (;;) {
        /* 再生係の仕事は DMA 化で引退した (2026-11-21、
         * doc/audio-dma-design.md)。hart1 は生存印だけ */
        /* 平常時: 生きている印に数え続ける。K210 では
         * 数えた行を定期的に追い出して書き戻さないと、
         * 相手の hart0 からはメモリ上の古い 0 しか
         * 見えない */
        {
            int i;
            for (i = 0; i < 1024; i++) {
                smp_heartbeat++;
            }
            smp_evict_cache();
        }
    }
}
