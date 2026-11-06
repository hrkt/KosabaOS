# 壁時計 (RTC) の設計

KosabaOS がハードウェア RTC から「何時何分か」(壁時計) を
読めるようにし、カーネルの中から設定もできるようにする。
2026-11-06 に方針を決めて着手した。

## 決めたこと (前提)

- **設定の口はカーネル内 API**。ユーザーランド向けの
  syscall は今回足さない (番号も消費しない)。消費者は
  当面カーネル自身 (コンソール表示) だけ。プログラムが
  時刻を使いたくなったら、その時に syscall 化する。
- NTP は将来の話で、今回は入れない。K210 の RTC は
  電源を切ると時刻を保持できない (バックアップ電池の
  仕組みが無い) ので、「合わせる」手段は人手の入力が
  最小の形になる。
- OS の中では **UTC** で扱う (Unix epoch の定義どおり)。
  タイムゾーンは表示側の関心事として将来に残す。

## 階層

```
カーネルコンソール ('t' キー)        ← 消費者 (カーネル内)
        |
共通層  time.c / time.h              ← 日時構造体、検証、
        |                              epoch <-> 日時の変換、
        |                              書式化とパース
        |   board_rtc_get / board_rtc_set (日時構造体で受ける)
        |
ボード層 boards/<board>/rtc.c        ← ハードの生読み書き
```

- 継ぎ目は **日時構造体** にした。K210 の RTC は年月日と
  時分秒のレジスタをそのまま持つのでこの形が素直で、
  Goldfish (epoch ナノ秒) の方はボード層で共通層の
  変換を使って日時に直す。blkdev の `board_blkdev()` と
  同じ流儀で、ボードが RTC を持たなければ -1 を返す。
- epoch との変換 (うるう年を含む) は共通層に 1 回だけ
  書き、両ボードで共用する。曜日も共通層が epoch の
  日数から計算する (1970-01-01 は木曜日)。

### 共通層の API (time.h)

```c
struct datetime { int year, month, day, hour, minute, second; };

int      time_read(struct datetime *dt);        /* 検証済みの読み。RTC が無い/未設定なら -1 */
int      time_set(const struct datetime *dt);   /* 検証してボードへ書く */
int      time_valid(const struct datetime *dt); /* 値域チェック (うるう年の 2/29 含む) */
uint64_t time_to_epoch(const struct datetime *dt);   /* 日時 -> epoch 秒 */
void     time_from_epoch(uint64_t epoch, struct datetime *dt);
int      time_weekday(const struct datetime *dt);    /* 0 = 日曜 */
int      time_parse(const char *s, struct datetime *dt); /* "YYYY-MM-DD hh:mm:ss" */
void     time_format(const struct datetime *dt, char out[20]); /* 同形式に整える */
```

日時の値域は 1970〜2199 年とする (epoch 変換の都合と、
K210 の世紀フィールドで無理なく表せる範囲)。

## ボード層

### virt: Goldfish RTC (0x101000)

QEMU virt 付属の RTC。ホストの時刻を Unix epoch からの
ナノ秒で返す:

- `+0x00` TIME_LOW: 下位 32 ビット。**読むとその瞬間の
  時刻がラッチされ**、続けて読む TIME_HIGH と一貫する
  (Linux の rtc-goldfish と同じ読み方をする)
- `+0x04` TIME_HIGH: 上位 32 ビット

0x101000 は UART (0x10000000) と同じギガページ (root[0])
にあり、既存の恒等マッピングでそのまま届く。
この RTC に時刻を書く口は無い (ホスト所有) ので、
`board_rtc_set` は -1 を返す。

### k210: 内蔵 RTC (0x50460000)

レジスタ配置は Kendryte SDK の rtc ドライバ
(reference/kendryte-standalone-sdk) に従う:

| オフセット | 内容 |
|---|---|
| 0x00 date | week[2:0], day[12:8], month[19:16], year[31:20] (下 2 桁) |
| 0x04 time | second[15:10], minute[21:16], hour[28:24] |
| 0x10 initial_count | 秒あたりのカウント数 (入力クロック周波数) |
| 0x14 current_count | 進行中のカウント |
| 0x18 interrupt_ctrl | 今回は使わない |
| 0x1c register_ctrl | read_enable[0], write_enable[1], timer_mask[20:13], alarm_mask[28:21], initial_count_mask[29], interrupt_register_mask[30] |
| 0x28 extended | century[4:0] (年の上位), leap_year[5] |

年は date の year (0〜99) と extended の century の合成
(2026 年なら year=26, century=20)。

モードは register_ctrl の 2 ビットで切り替える:
読む時は RUNNING (read=1, write=0)、書く時は SETTING
(read=0, write=1)。切り替えには RTC 側の同期待ちが要る
(SDK は 1/26MHz 秒ほど CPU サイクルで待つ)。S モードから
サイクルカウンタは読まない方針なので、小さなスピン
ループで代替する。

#### 初期化の方針 (遅延初期化)

最初に RTC を触る時 (board_rtc_get / set の初回) に
だけ初期化する:

1. sysctl のクロックを有効化する
   (clk_en_cent 0x50440028 の apb1 = bit4、
   clk_en_peri 0x5044002C の rtc = bit29)
2. **見つけた状態を klog に残す** (date / time /
   extended / initial_count / register_ctrl の生値)。
   未設定時に何が読めるかの実測記録を兼ねる
3. 書き込み保護を外す (mask 類を全て 1 に)
4. initial_count が 0 の時だけ 26,000,000 (IN0 の
   26MHz) を書く
5. RUNNING モードにする

SDK の rtc_init はペリフェラルのリセットから始めるが、
こちらでは **リセットしない**。リセットを跨いで設定が
残るかどうかを実測したいのと、温かい再起動で時刻が
消えない方が置き時計には都合がよいため。0x50460000 は
SYSCTL (0x50440000) と同じ 2MB ブロックにあり、
vmhook の既存の割り当てで届く。

## 消費者: カーネルコンソールの 't' キー

未使用の 't' を壁時計に割り当てる:

1. 't' を押すと現在の壁時計を出す:
   `wall: 2026-11-06 14:05:22 UTC (epoch 1793977522)`
   未設定なら `wall: (RTC not set)`
2. 続けて設定の行入力に入る:
   `set time? (YYYY-MM-DD hh:mm:ss, empty = keep): `
   空行なら何もしない。有効な日時なら設定し、読み
   戻して `time set: ...` と `wall: ...` を出す。
   読むだけのボード (virt) では
   `time: cannot set on this board (read-only RTC)`、
   形式が違う場合は `time: bad format` と出す
3. 表示と設定の結果行は kputs 経由 (コンソールと
   klog の両方に残る)。プロンプトと入力のエコーは
   UART にだけ出す

起動時にも 1 行出す: `time: wall clock ... UTC`、
または `time: no wall clock (RTC not set or absent)`。

## 検証

- virt (test.sh にシナリオ S を追加。ブランチ作業時は
  R で、main 統合時に uptime の R と重なるため
  振り直した): 起動行と 't' の
  表示がホストの UTC と大きくずれていないこと
  (epoch で比較)、時間を置いて 2 回読むと進んでいる
  こと、設定が read-only で丁寧に断られること
- 実機 (test-k210.py に K13 を追加。K12 から
  振り直し): 未設定時の読み
  (実測として記録)、設定 → 数秒待って読み戻しが
  数秒以内の誤差で一致すること。再起動 (SRAM 起動の
  やり直し) を跨いだ保持は、テストとは別に手で実測
  して結果を下に残す

結果 (ブランチ時点): virt は全 18 シナリオ ALL PASS、
実機は全 12 シナリオ ALL PASS。実機の K5 が一度
だけ落ちたが、原因は tick 表示が応答文字列の真ん中に
割り込んだハーネス側のフレークで、再実行で通過した。

## 実測結果 (2026-11-06、Maix Dock 実機)

- **未設定時の状態**: 起動直後、クロックを有効化した
  だけの生値は date / time / extended / initial_count /
  register_ctrl が **すべて 0**。read_enable も立って
  いないので、初期化なしには読めない。値域が成り立た
  ないので、共通層の検証で「未設定」と判定される
- **初期化は必要**: sysctl のクロック有効化、書き込み
  保護の解除、initial_count が 0 の時だけ 26MHz を
  書くこと、RUNNING モード化。この手順で読み書きが
  通る。ペリフェラルのリセットはしない方針のまま
- **設定と進行**: コンソールから設定すると即時の
  読み戻しが一致し、その後も実時間の速さで進む
  (K13 の往復で確認)
- **設定はリセットを跨いで残らない**: 設定後に
  kflash の SRAM 起動をやり直しても、board_reset
  (フラッシュの旧カーネルが起動) を挟んでも、次回
  起動時に見つかる生値はすべて 0 だった。ダンプは
  自前ドライバが何か書く前 (クロック有効化のみ) の
  値なので、消しているのはリセット側である。電池
  バックアップが無いので電源断ではなおさら保持
  されない。壁時計として使うには起動ごとの設定が
  前提になる (MaixPy が起動後に合わせ直す運用と
  同じ結論)
- virt 側: Goldfish RTC はホストの UTC を返し、
  epoch はホストと一致した。書く口は無く、設定は
  -1 で断られる (read-only)

## 消費者: LCD の時計表示 (2026-11-07)

カーネルコンソールの 't' に続く 2 人目の消費者として、
LCD の tick 表示 (boards/k210/lcd.c の lcd_tick_draw) が
壁時計を使うようになった。新しいタスクやスケジューラは
作らず、既存の約 1 秒周期の描画に乗せている。

- 毎秒の描画のたびに time_read を試し、有効な日時が
  返る間は "HH:MM:SS" (UTC) を描く。未設定の間は
  従来どおり起動秒数を描くので、表示が退行することは
  ない。起動 → 't' で合わせる → 壁時計に切り替わる、
  という流れになる
- 切り替わりの節目だけ、描いた値を
  "lcd: wall clock YYYY-MM-DD hh:mm:ss UTC" として
  1 回だけ klog に残す (毎秒は残さない)
- 実測 (2026-11-07、Maix Dock、SRAM 起動):
  起動 → 't' で 2032-03-04 05:06:07 を設定 →
  's' で Nish へ戻ると最初の壁時計描画が 05:06:10
  (設定から +3 秒、コンソールの移動を含む) で、
  節目行もその値で残った。その後の読み戻し
  (05:07:12) まで実時間どおりに進んだ
- 注意: tick 描画はユーザープログラムの実行中に進み、
  カーネルコンソールの入力待ちでは泊まる (タイマの
  つながりがそこで止まる既存の性質で、コンソールの
  tick 表示と同じ)。その間も RTC 自体は進んでいて、
  プログラムに戻れば表示は現在時刻に追いつく。
  今回はこの仕組み自体には手を入れていない
- LCD のピクセルそのものはシリアルからは観測
  できない。確認は「描画に使った値の記録 (節目行)
  ＋ RTC の読み戻し」で行い、画面に正しく出て
  いるかの最終確認は目視に残る
