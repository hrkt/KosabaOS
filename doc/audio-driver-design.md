# オーディオドライバ設計 — K210 の I2S 実体化 (40〜42)

状態: 設計 (2026-11-08、branch `audio-i2s`)。
syscall 40〜42 の仕様は doc/syscalls.md と Ikesu 側の
doc/audio-syscall.md が正本で、本書はその K210 実体の
設計を決める。virt はダミーのまま変えない。

## ハードウェア構成 (確定事項)

出典は Ikesu doc/stage1-prep.md の下調べと、Kendryte
SDK (reference/kendryte-standalone-sdk) の i2s.c /
i2s.h / sysctl の実物で裏付けた値。

- I2S0 (DesignWare I2S) は 0x50250000。FPIOA
  (0x502B0000) と同じ 2MB ブロックの中なので既存の
  割り当てでも届くが、vmhook.c に明示の
  vm_map_device(0x50250000) を足す。
- ピン (Maix Dock の TM8211 DAC): IO33 = WS
  (FPIOA 89 = I2S0_WS)、IO34 = DATA (95 =
  I2S0_OUT_D1)、IO35 = SCLK (88 = I2S0_SCLK)。
  旗は lcd.c の流儀で func | 0x1F00 (ds 最大 +
  oe_en)。3 本とも K210 が駆動する出力。
  OUT_D1 に対応する送信チャンネル 1 を使う。
- 形式: 解決度 16bit、SCLK 32 サイクル、配置は
  RIGHT_JUSTIFYING (MaixPy の wav 再生作例の実績
  どおり)。無音なら STANDARD へ、が切り分け順で、
  切り替えはドライバ内の定数 1 つ。
- TM8211 はステレオ前提。channels=1 の複製は
  ドライバの内側で行う (仕様側の決定どおり)。

## クロック

- 有効化: SYSCTL clk_en_cent (0x50440028) の
  apb0_clk_en = bit3、clk_en_peri (0x5044002C) の
  i2s0_clk_en = bit10。
- I2S0 のリセットは peri_reset (0x50440034) の
  bit10 をパルスする (SDK の i2s_init と同じ。
  RTC と違い、跨いで残したい状態は無い)。
- 源クロックは PLL2。KosabaOS は PLL2 を他では
  一切使っていないので、オーディオが握ってよい。
- PLL2 の式は IN0 (26MHz) / (clkr+1) × (clkf+1)
  / (clkod+1) で、分周フィールドの範囲では
  45.1584MHz ちょうどは作れない。最良の組み合わせ
  clkr=0 / clkf=25 / clkod=14 で 45.0667MHz
  (0.2% 低い)。bwadj は 12 (= clkf/2 - 1 相当)。
  設定手順は SDK の sysctl_pll_set_freq の順
  (out_en を落とす → pwrd を落とす → 値を書く →
  pwrd → リセットパルス → pll_lock (0x50440018、
  pll_lock2 の下位ビット。SDK の判定どおり) を
  上限付きで待つ → out_en)。
  PLL2 の設定は最初の open で 1 回だけ。
- サンプルレートは clk_th3 (0x50440044) の
  i2s0_clk_threshold (下位 16bit) で決まる:
  threshold = round(pll2 / (rate × 128)) − 1
  (32 サイクル × ステレオの分周)。実レートは
  pll2 / ((threshold+1) × 128)。open は実レートが
  要求の 1% 以内のときだけ受け、それ以外は -1。
  44100 → threshold 7、実 44010Hz で可。22050 も可。
  48000 は実 50305Hz になり 1% を超えるので -1
  (48k 系が要る日は PLL2 = 49.152MHz 系の別
  パラメータが要る。それは将来の話)。

## ブロッキングの意味論 (設計の中心)

audio_write の契約は「デバイスの空きを待ってよい
(それが再生のペーシングになる)」。段1 の形:

- 送信 FIFO (深さ 16 段) へステレオフレーム単位で
  書く。満杯に近い間 (チャンネル 1 の ISR の
  txfe = 0) はポーリングで待ち、待ちの合間に
  sched_poll() を呼ぶ。getc の待ちと同じ作法で、
  寝ているカーネルタスク (時計) は待ちの間も
  起きて走れる。書く側の判定は TFCR = 12 とし、
  txfe = 1 (残り 12 段以下) のときだけ 1 フレーム
  (2 段) を書くので、あふれは起きない設計。実際に
  あふれたら (txfo) 数えて TOR 読みで消す。
- **走者 (ジョブ) の切替は段1 ではしない。**
  getc は副作用が無いので「ecall の位置で保存して
  再実行」が使えるが、audio_write は FIFO へ
  入れた分が累積する副作用を持つので、その作法は
  そのまま移植できない。帰結として、1 回の
  audio_write が戻るまで他の走者は走らない
  (カーネルタスクは sched_poll で走る)。
  実害の見積もり: 呼び出し側 (Ikesu) はデコードの
  塊 (数十 ms 分) ごとに書くので、1 回の占有は
  その再生時間まで。巨大な 1 回書きは再生時間
  そのまま占有するが、それは長い計算をする
  ユーザープログラムが前面を占有するのと同じで、
  KosabaOS に前面プログラムへの割り込み手段は
  元々無い。割り切りとして明記する。
- 将来の道筋: DMA + リングバッファにすれば、
  書き込みはリングへ移すだけで戻り、実再生は
  DMA が担うので占有は消える。その時も syscall
  の契約 (40〜42、部分受け付け可) は変えずに
  実装だけ差し替える。本書の API 境界
  (board_audio_write が塊を受けて最後まで送る)
  は、その差し替え点になる。

## 共通コードとの境界と観測可能性

- 共通ヘッダ audio.h にボード側の口を 3 つ宣言し、
  board.h の BOARD_FEATURE_AUDIO で選ぶ
  (k210 = 1、virt = 0):
    board_audio_open(rate, channels) → 0 / -1
    board_audio_write(buf, nbytes, channels) → 0 / -1
    board_audio_close()
- trap.c の流れは両ボード共通のまま: 開くときに
  ドライバを呼び (-1 なら open 自体が -1)、
  write は従来どおりユーザーメモリを 256 バイト
  ずつ写して FNV を更新し、その塊をドライバへも
  渡す。close の klog 行 (audio: close rate=...
  fnv=...) は従来の形を両ボードで残す。
- K210 では close のときドライバがもう 1 行出す:
    audio: i2s frames=<送ったステレオフレーム数>
      waits=<満杯で待った回数> over=<あふれ回数>
  最初の open では PLL2 の実周波数とロック状態、
  決まった threshold と実レートも 1 行残す
  (audio: i2s pll2=... lock=... th=... rate=...)。
  立ち上げの証拠はシリアルで全部追える形にする。
- シリアルで確認できる範囲: open/write/close の
  戻り値、close 行の frames が「レート × 秒数」と
  一致すること、over = 0、カーネルと時計が
  生き続けること。耳でしか確認できない範囲:
  音が出ること自体、音程、音量、RIGHT_JUSTIFYING
  が合っているか。K21 は前者までを担い、後者は
  ユーザーの耳の確認待ちとして残す。

## テスト計画

- tone.elf: 440Hz のサイン波 (振幅 8000) を
  44100Hz / モノラルで 3 秒分 (132300 サンプル)
  書き、"tone: done" を出して終わるデモ兼テスト
  プログラム。モノラルなので左右複製の経路も通る。
- test-k210.py に K21 を追加: tone.elf が正常終了
  し、dmesg に audio: i2s 行 (frames=132300) と
  従来の audio: close 行が出ることを見る。
- virt の test.sh は従来どおり (シナリオ T が
  ダミー経路の回帰を見る)。フルランで確認する。
- 実機ランは SRAM ブートのみ。フラッシュは触らない。
