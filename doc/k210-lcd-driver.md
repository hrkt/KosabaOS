# K210 の LCD ドライバ — 開発メモ

これは仕様書ではなく、開発メモ。Maix Dock (K210) 付属 LCD を
自前ドライバで表示できるようにしたときの、実機で確かめた事実と
落とし穴の記録。将来 KosabaOS 本体へ画面出力を入れるときの出発点。
仕組みと用語の解説は学習用ドキュメント `lcd-basics.md` に分けた。

- 状態: spike で実機確認 (2026-11-03) の後、OS 本体へ移植済み。
  `boards/k210/lcd.c` が起動の進み具合 (ステージ) と tick 秒数を
  LCD に表示する。実機で表示確認済み、virt は test.sh 全 8 件 PASS。
- コードの置き場: 本体は `os/arch/riscv64/boards/k210/lcd.[ch]`。
  実験の原形は `~/workspace/kosaba-os/spike-k210/step4-lcd/`
  (リポジトリ外の実験置き場。詳細はそこの README)。
- spike で確認した表示: 「HELLO WORLD!」+ 起動経過時間の時計
  (HH:MM:SS、CLINT mtime 駆動)。
- 本体で表示するもの: タイトル「KOSABAOS / BOARD K210」、ステージ行
  (STEP2 BOARD OK → STEP3 TRAP TIMER OK → FS READY →
  CONSOLE READY) を各初期化の完了ごとに 1 行ずつ追加し、最下行に
  tick 秒数を毎秒描き直す。tick の描画は mtrap の割り込みハンドラ
  から呼ぶが、`lcd_tick_setup` が済むまでは描かない (起動中の
  描画と割り込みからの描画がぶつからないための番人)。
  フォントは A-Z・0-9・記号の 43 字に拡張した (生成と検品の
  手順は落とし穴 4 の通り)。

## ドライバの形

アプリが使う API と、変換の置き場所を分けた。

- 公開 API (`lcd.h`): `lcd_init()` / `lcd_fill_rect(x, y, w, h, color)` /
  `lcd_draw_text(x, y, s, scale, color)`。色は `LCD_RGB(r, g, b)`
  マクロで作る (RGB565 への変換を各所で手計算しない)。
- ドライバ内部 (`lcd.c`): SPI0 の octal 転送、ST7789 のコマンドと
  パラメータの詰め方、フォントのビット並び。変換は全部ここに閉じる。
- アプリ (`clock.c`): 描画は API のみ。7 セグ数字のような
  ウィジェットはアプリ側に置き、セグメント配置の計算だけを持って
  描画は `lcd_fill_rect` に任せる。

## ハードウェアの事実 (実物合わせ)

パネルは ST7789、240x320。横向き 320x240 で使う (MADCTL = 0xA0、
DIR_YX_RLDU。MaixPy の既定と同じ向き)。

| 信号 | K210 の IO | 割り当て |
|---|---|---|
| CS | IO36 | SPI0_SS3 (FPIOA func 15) |
| CLK | IO39 | SPI0_SCLK (func 17) |
| RST | IO37 | GPIOHS30 (func 54) |
| DC | IO38 | GPIOHS31 (func 55) |

- FPIOA の書き値は `func | 0x1F00` (ds=0xF が bits[11:8]、oe_en が
  bit12)。UART のピンと同じ旗パターン。
- データ線は FPIOA では割り当てない。sysctl の
  `misc.spi0_dvp_data` (0x50440054 の bit10) を有効にすると、
  SPI0 のデータが DVP データ線へ出る (MaixPy が起動時に設定)。
- SPI0 の有効化: sysctl clk_en_cent (0x50440028) bit5 と
  clk_en_peri (0x5044002C) bit6。周波数は PLL0 / ((threshold+1)×2)、
  threshold は clk_th1 (0x5044003C) bits[7:0] を 0 に。
  ROM が残す状態 (PLL0=780MHz) では入力 390MHz、baudr 26 で 15MHz
  (MaixPy の LCD 既定と同じ速さ)。
- 転送は SPI0 の octal モード (DesignWare SSI の FF_OCTAL、
  spi_ctrlr0 はベース+0xF4、trans mode = AITM_AS_FRAME_FORMAT):
  コマンドは instruction フェーズ 8bit、データは address フェーズ。
- ST7789 初期化列: ハードリセット (RST Low → 50ms → High → 50ms) →
  SWRESET (0x01) 50ms → SLPOUT (0x11) 120ms → COLMOD (0x3A) = 0x55
  (RGB565) → MADCTL (0x36) = 0xA0 → NORON (0x13) → DISPON (0x29)。
  窓設定は CASET (0x2A) / RASET (0x2B) に各 4 バイト
  (開始の上位・下位、終了の上位・下位)、続いて RAMWR (0x2C)。
  出発点は MaixPy-v1 の py_lcd.c / lcd_mcu.c / st7789.c と
  Kendryte SDK の spi.c。

## 落とし穴 (実機で切り分けた順に)

切り分け実験は spike の lcdtest2〜6。各段で変数を 1 つだけ変え、
画面の変化と UART ログで判定した。

1. **転送の列挙値**: trans mode (AITM) を 0、TMOD を 0 にしていた
   初版は、初期化も描画も画面に何も届かなかった (未初期化の白)。
   正しくは AITM=2、TMOD=1 (送信)。ここを直すと書き込み自体は
   届くようになった。
2. **パラメータは 8bit address フェーズでは届かない** (最大の山)。
   CASET/RASET のパラメータを 1 バイトずつ (あるいは 4 バイト
   まとめて) 8bit フェーズで送ると、窓が効かず描画が画面外へ
   消える。コマンド (instruction フェーズ) と画素データ
   (16bit address フェーズ) は正常だったので、壊れているのは
   パラメータの経路だけ、と切り分けられた。
   対策: パラメータは **16bit address フェーズで、2 バイトを
   1 つの uint16 (上位バイトが先) に詰めて送る**。これで窓が
   正しく効いた (全画面の窓→全面赤、小さい窓→緑の四角)。
   奇数バイトのパラメータ (COLMOD、MADCTL) は末尾に 0x00 を
   足して偶数にする。0x00 はパネル側で NOP コマンドとして
   捨てられるので無害。実装は `st7789_params` に集約した。
   なぜ 8bit フェーズが届かないのか (DW SSI の仕様か K210 の
   実装か) は未解明。MaixPy は 8bit フェーズで送って動いている
   ので、こちらの初期化手順との差がどこかにあるはずだが、
   実用上は 16bit フェーズで送ればよい、で先に進めた。
3. **画素は窓ごとに 1 本の連続ストリームで送る**。
   32 画素ずつ転送を区切る (その都度 SPI 再初期化 + CS トグル)
   と絵が化ける。MaixPy は 1 つの窓に 1 本の連続転送。
   `lcd_fill_rect` は窓設定からストリームまでを 1 関数に閉じた。
4. **フォント表の約束の混在**。5x7 フォントの列バイトは
   「bit0 が最下行、bit6 が最上行」に統一。初版は文字ごとに
   約束が混ざっていて A/S/N/1/2/! が化けた (S と N と 2 は
   左右反転、A と 1 は上下の約束違い)。対策として、字形は
   行文字列から機械生成し、ASCII アートへ再描画して全字形を
   検品してから表にした。今後も表を直すときは同じ手順で。

## OS 本体への移植 (2026-11-03、実施済み)

- このドライバは Maix Dock の配線に依存する部分 (FPIOA の func
  番号、DVP データ線、SPI0 の癖) と、ST7789 一般の部分
  (初期化列、窓設定、RGB565) の混ざり物。移植では丸ごと
  boards/k210 に置いた (現時点で LCD を持つボードはこれだけ)。
  別の ST7789 ボードが来たら、その時点で ST7789 一般の部分を
  共通ドライバへ切り出す。
- 共通コード (kernel.c) からは BOARD_FEATURE_LCD (k210=1、
  virt=0) のガード越しに lcd.h の API だけを呼ぶ。mtrap.c の
  割り込みハンドラは lcd_tick_draw を毎秒呼ぶ。
- 使ったレジスタや命令は新規なし (既存の MMIO アクセスのみ)。
  instructions.md への追記は不要だった。
- 移植チェックリストの step には含めていない寄り道。本線は
  step 4 (ユーザーモードと syscall) のまま。
