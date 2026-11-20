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

/* 1 フレーム (左右 1 組) を FIFO へ入れる。空きが無ければ
 * 待たずに 0 を返す (入れたら 1)。供給は共通コードの
 * リングバッファ + フィーダが担い、ドライバは「今入るか」
 * だけに答える (doc/audio-driver-design.md 追補3) */
int board_audio_try_push(uint16_t l, uint16_t r);

/* 閉じる。ドライバ側の記録 (K210 では i2s の要約行) は
 * ここでカーネルログへ出す */
void board_audio_close(void);

/* いま audio が開いているか (共通コード trap.c が持つ
 * open 状態を読む口)。I2S の FIFO は 1ms 分も無いので、
 * 再生中に ms 単位で CPU を占有する仕事 (時計帯の LCD
 * 再描画、tick のコンソール印字) が入ると DAC が飢えて
 * 音が周期的に欠ける。そちら側が後回しにするために使う */
int audio_is_open(void);

#endif
