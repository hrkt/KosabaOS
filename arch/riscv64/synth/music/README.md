# レパートリー (モーツァルト)

smfplay + synth の実演・負荷テスト用の「本物の曲」素材。
生成スクリプト `make_repertoire.py` が素材を取得・変換して
SMF を出す。**このディレクトリに置く正本はスクリプトと
このメモで、生成した .mid はコミットしない** (OS リポジトリに
他人のバイナリを持ち込まない従来方針に合わせる)。

## 曲目と出典

作曲はいずれも W. A. Mozart でパブリックドメイン。浄書
(タイプセット) には作成者の権利があるため、出所と
ライセンスが明記された Mutopia Project の素材だけを使う。

| 生成物 | 曲 | 出典 (Mutopia) | 浄書者 | ライセンス |
| --- | --- | --- | --- | --- |
| MOZ545.MID | ピアノソナタ第16番 K.545 第1楽章 Allegro | ID 998 (Mutopia-2013/09/01-998) | Alejandro Sierra | Creative Commons Attribution-ShareAlike 3.0 |
| MOZ331.MID | ピアノソナタ第11番 K.331 第3楽章 Rondo alla Turca | ID 108 (Mutopia-2001/09/15-108) | Rune Zedeler / Chris Sawer | Public Domain |

素材 URL は make_repertoire.py の SOURCES に記録してある。
MOZ545.MID は CC BY-SA 3.0 の派生物なので、再配布する
場合は上の帰属表示と同ライセンスを保つこと。

## 生成方法

```
python3 make_repertoire.py [出力ディレクトリ]
```

実行時に Mutopia の FTP から素材 MIDI を取得する
(取得済みのファイルがカレントディレクトリにあれば
それを使う)。出力は MOZ545.MID / MOZ331.MID で、
build.sh があれば SD イメージにも載る (素材として
/sd から `run smfplay /sd/MOZ545.MID` のように使う)。

変換の内容:

- format 1、division は素材のまま 384 PPQ
- 右手トラック -> ch 1、左手トラック -> ch 2
  (synth のマルチティンバーでパート 1/2 に載る)
- 冒頭に program 0 (GM Grand Piano) を両 ch へ追加
- ノートとベロシティは素材のまま
- テンポ: K.545 は素材の指定 (454545 us/四分音符、
  約 132 BPM、曲長約 132 秒)。K.331 は素材 MIDI が
  LilyPond 既定の 60 BPM になっていて楽譜の Allegretto
  と合わないため、編集判断で 500000 us (120 BPM、
  曲長約 128 秒) に置いた
- 繰り返し記号は素材の MIDI の通り (展開しない)

## 対象外

- ペダル (CC64): 素材に含まれず、synth も未対応
- CC 類は落とす。K.545 の CC7 は LilyPond が強弱の
  ヘアピンを描いたもので、パート音量として解釈すると
  暴れるため (音ごとのベロシティは残してある)
- 装飾音符は LilyPond が実音として書き出す範囲で含む

## 検証 (virt、2026-11-25)

生成物の音符数は素材と一致 (K.545: 1288 音、
K.331: 1614 音)。smfplay の上限 (ファイル 32768 バイト、
4096 イベント) の内側。K.545 は smfplay log で
全 2578 イベント・曲長 132,272 ms が予定と一致
(actual 132,274 ms)。両曲とも同時実行で synth が
全曲完走し FNV が audio close と一致した
(K.545: d50102f8f70b9fea、K.331: c4c0e2b104e5708c)。
詳細は doc/synth-design.md のレパートリーの節に記録。
