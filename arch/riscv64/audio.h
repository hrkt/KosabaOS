#ifndef KOSABAOS_AUDIO_H
#define KOSABAOS_AUDIO_H

#include <stdint.h>

/* audio.h -- オーディオ syscall (40〜42) のボード側の口
 *
 * 仕様は doc/syscalls.md (Ikesu 側 doc/audio-syscall.md が
 * 原案)。共通コード (trap.c) は「サンプルの流れをデバイスへ
 * 渡す」ことだけを知り、実デバイスの都合はボード層が受ける。
 * BOARD_FEATURE_AUDIO が 1 のボード (K210) は boards/k210/
 * i2s.c がこの 3 つを実装し、0 のボード (virt) では呼ばれ
 * ない (trap.c のダミー実装だけが動く)。設計は
 * doc/audio-driver-design.md */

/* 開く。対応できないレートなら -1 (open 自体を失敗させる) */
int board_audio_open(uint64_t rate, uint64_t channels);

/* 受けた塊 (nbytes バイト、形式は open で宣言したもの) を
 * 最後までデバイスへ送る。再生のペーシングで待つことが
 * ある。失敗は -1 */
int board_audio_write(const unsigned char *buf, uint64_t nbytes,
                      uint64_t channels);

/* 閉じる。ドライバ側の記録 (K210 では i2s の要約行) は
 * ここでカーネルログへ出す */
void board_audio_close(void);

#endif
