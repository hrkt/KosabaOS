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

## 検証の計画

- virt (test.sh にシナリオ R を追加): 起動行と 't' の
  表示がホストの UTC と大きくずれていないこと
  (epoch で比較)、時間を置いて 2 回読むと進んでいる
  こと、設定が read-only で丁寧に断られること
- 実機 (test-k210.py に K12 を追加): 未設定時の読み
  (実測として記録)、設定 → 数秒待って読み戻しが
  数秒以内の誤差で一致すること。再起動 (SRAM 起動の
  やり直し) を跨いだ保持は、テストとは別に手で実測
  して結果をここに残す
