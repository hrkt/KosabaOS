#ifndef KOSABAOS_KLOG_H
#define KOSABAOS_KLOG_H

#include <stdint.h>

/* カーネルログ (klog.c)。Linux の printk + dmesg の考え方を
 * 小さく真似たもので、カーネル自身の出来事を固定長の
 * リングバッファにテキストで残す。読み出しは syscall の
 * klog_read (30) 経由で、Nish の dmesg コマンドが使う
 * (仕様は doc/syscalls.md のカーネルログの節)。
 *
 * kputc / kputs / kput_dec / kput_hex は「コンソール (UART)
 * にも同時に出す」記録口。カーネルのメッセージは今まで
 * どおり画面に出たうえで、ログにも残る。ユーザープログラム
 * の出力 (SYS_PUTC) はこちらを通らないので、ログには
 * 入らない。 */

void kputc(char c);            /* 1 文字: コンソール + ログ */
void kputs(const char *s);     /* 文字列: コンソール + ログ */
void kput_dec(uint64_t x);     /* 数値 (10 進): コンソール + ログ */
void kput_hex(uint64_t x);     /* 数値 (16 進): コンソール + ログ */

uint64_t klog_total(void);     /* 起動以来に記録した総バイト数 */
uint64_t klog_oldest(void);    /* まだ残っている最古の位置 (通し) */
/* 絶対位置 offset から最大 max バイトを dst へ写し、写した
 * バイト数を返す。offset が最古より前なら最古に丸める
 * (古い分は上書きされて消えている)。offset が最新以上なら 0 */
uint64_t klog_read(uint64_t offset, char *dst, uint64_t max);

#endif
