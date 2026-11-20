# recv 設計メモ — シリアル経由のファイル受信

状態: 実装済み (branch `recv`、2026-11-20)。
きっかけは「SD カードの抜き差しに疲れた」。Hatoba に
file_push ツールを足し、OS 側には受け側のユーザー
プログラム RECV.ELF を用意する。プロトコルは両側の
共通仕様で、Kermit の芯 (stop-and-wait + CRC) だけ。

## 使い方

```
run /sd/RECV.ELF <NAME>     (SD に置いた場合)
run recv.elf <NAME>         (OS 同梱の場合)
```

コンソール UART からバイナリを受け取り、`/sd/<NAME>`
へ書く。NAME は裸の名前 (スラッシュ不可、40 文字まで)。
同名ファイルがあれば create の仕様どおり空にしてから
書く (上書き)。実装は IwaC 製 (nami と同じ流儀、
`arch/riscv64/recv/`)。OS 同梱は nami と同じ
「素材があれば取り込む」形 (RECV_EMBED)。

## プロトコル (Hatoba 側と共通)

フレーム (データ):

```
+------+------+--------+---------+-----------+
| SOH  | seq  | len    | payload | CRC-16    |
| 0x01 | 1B   | 2B LE  | 0..1024 | 2B        |
+------+------+--------+---------+-----------+
```

- seq は mod 256 で 0 から数える
- CRC は **CRC-16/CCITT-FALSE** (多項式 0x1021、
  初期値 0xFFFF、反転なし)。対象は seq + len (2B LE)
  + payload。**CRC フィールドはビッグエンディアン**
  (上位バイト先) で送る
- 終了フレームは先頭バイトが 0x03 (ETX) で、形は
  データフレームと同じ (seq + len=0 + CRC)

応答:

- ACK = 0x06 + seq
- NAK = 0x15 + 期待 seq

受け側の規則:

- CRC OK かつ seq == 期待値: payload をファイルへ
  書いて ACK。ETX なら ACK を返してから close し、
  完了行を印字する
- seq == 期待値 - 1 (重複 = ACK 紛失時の再送):
  書かずに ACK だけ返す
- それ以外 (CRC 不一致・seq ずれ・ETX で len != 0):
  NAK (期待 seq)
- len > 1024 のヘッダは同期ずれとみなして捨て、
  先頭バイト (0x01 / 0x03) 探しに戻る。送り側は
  タイムアウトで再送するので、そこで同期が戻る

ハンドシェイクと完了報告:

- ファイルを開いた後に `RECV1 READY\n`
- 終了フレームを受けたら close し、
  `RECV1 DONE size=<n> fnv=<16 桁 16 進>\n`
  (FNV-1a-64、audio の klog と同じ物差し) を印字
  して Nish へ戻る
- 書き込みに失敗したら `RECV1 ERROR write` を
  印字してステータス 1 で終わる

前提: コンソール 115200 baud。受け側は SD 書き込み中
受信できないので stop-and-wait が必須 (送り側は
ACK/NAK を待つ。タイムアウト 2 秒・再送は送り側の
流儀)。syscall は既存のものだけを使う (getc=2、
create=9、write=10、close=6)。新規 syscall はなし。

## 割り切り

- 受け側にタイムアウトはない (getc が割り込み待ちで
  ブロックする syscall のため)。送り側が諦めたら
  RECV は待ったままになる。基板リセットで戻る
- ファイルスコープには char 配列だけ置く (nori の
  データ配置が 8 バイト揃えでない件の回避、nami と同じ)
