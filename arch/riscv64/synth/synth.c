/* synth.c -- KosabaOS のシンセ音源アプリ (doc/synth-design.md 段1)
 *
 *   run synth          FM デモ曲 (引数なしの既定)
 *   run synth fm       FM デモ曲
 *   run synth psg      PSG デモ曲 (同じ曲)
 *   run synth play fm  シリアル鍵盤で FM を弾く
 *   run synth play psg シリアル鍵盤で PSG を弾く
 *   run synth fm <n>   プリセット n でデモ曲
 *                      (FM 0..21 / PSG 0..15。play でも
 *                      第 3 引数で指定可。対話中は
 *                      [ / ] キーで切り替え)
 *   run synth midi <p> [fm|psg] MIDI バイト列ストリームを
 *                      読んで鳴らす (エンジン既定は fm。
 *                      fm はマルチティンバー版: [pool 1-32]
 *                      [2op|4op] [dbg] も指定可)
 *                      (smfplay の file モードの受け手。解釈は
 *                      下の run_midi のコメント参照)
 *   全モード共通の追加引数 (エフェクトラック):
 *                      [nofx] [rev|dly|cho] [fxchain] [fxdbg]
 *                      (下のエフェクトラックの節、doc 参照)
 *
 * IwaC の C サブセットで書く (nami / wavgen と同じ作法):
 * #include / libc なし、switch なし、文字リテラルなし
 * (文字コードを数で書く)、浮動小数点なし (全部固定小数点)。
 * ワード単位の状態はファイルスコープの int32 配列に置き
 * 直接アクセスする (IwaC v63 で nori が配列を 8 バイト
 * 境界に揃える。それ以前は char 配列 + バイト合成が
 * 必要だった。経緯は doc の「直接アクセス化」の節)。
 * バイト列 (曲データ・プリセット記録・名前・出力バッファ)
 * は char 配列。表は初期化子にせず、関数内で代入で埋める。
 * main に大きなローカル配列を置くと IwaC のフレーム生成が
 * 壊れる (実測あり) ので、main と run_demo / run_play の
 * ローカルはスカラだけに保つ。
 *
 * 音源 (段1):
 *   FM  : 2 オペレータ (変調側の位相で搬送側を変調)。
 *         正弦波は 256 点テーブル + 線形補間。変調の深さは
 *         専用エンベロープ (menv) で減衰するので、ベル /
 *         エレピ寄りの立ち上がりになる
 *   FM 波形 (段2): オペレータごとに TX81Z 風 8 波形
 *         (サイン / 二乗 / 全波整流 / 半波二乗 / 半波
 *         2 倍音 / 同二乗 / 折り返し / 半波全波 2 倍音)
 *         を選べる。波形は make_waves.py が生成する
 *         256 点 Q15 テーブルで、参照先が変わるだけ
 *         なので描画コストは増えない。4op は段1 の
 *         スタック / 2+2 の両形態が対象
 *   ピッチ EG (段2.5): DX11 風の R1..R4 / L1..L4 +
 *         レンジ。ボイスごとに 1 本で全オペレータの
 *         音程をまとめて動かす。64 サンプル単位で進め、
 *         実効増分 = 基本増分 x 2^(ずらし / 12)。
 *         レンジ 0 = オフ (既存プリセットは全部オフ)
 *   PSG : 矩形波 (デューティ 50%)。デモでは 4 ステップ
 *         ごとにノイズ (16bit LFSR) のハットも重ねる
 *   共通: 4 ボイスのラウンドロビン割り当て、簡易 ADSR
 *         (attack / decay / sustain / release)、整数ミキサー
 *
 * 位相は 1 周を 65536 とする固定小数点で持つ。音程の表は
 * MIDI ノート 36..96 に対する位相増分 (freq * 65536 /
 * 44100) をホストで計算したもの。実効レートは K210 で
 * 44010Hz なので音程は約 -0.2% 低い (OS 側の事情)。
 *
 * 対話モードの割り切り: 端末 (シリアル) にはキーアップが
 * 無いので、キーを押すと gate 分だけ鳴って自動で release
 * に入る。現行の audio リングは 8192 フレーム (約 186ms)
 * なので、弾いてから鳴るまでにはその分の遅れがある
 * (低遅延化は OS 側 realtime-prep の段2 の仕事)。
 *
 * 終了時は総サンプル数と PCM の FNV-1a 64bit を印字する
 * (virt のダミーオーディオが dmesg に出す audio: close
 * 行の samples / fnv と照合できる。wavgen の流儀)。
 * 戻り値: 0 = 正常、1 = オーディオを開けない等、2 = 引数不正
 */

#define SYS_GETC 2
#define SYS_OPEN 4
#define SYS_READ 5
#define SYS_FCLOSE 6
#define SYS_SLEEP 34
#define SYS_UPTIME 31
#define SYS_PERF 33
#define SYS_AUDIO_OPEN 40
#define SYS_AUDIO_WRITE 41
#define SYS_AUDIO_CLOSE 42

#define RATE 44100
#define CHUNK 1024
#define STEP_LEN 11025
#define NSTEPS 32

void out_str(char *s)
{
    while (*s != 0) {
        putc(*s);
        s = s + 1;
    }
}

void out_num(int64 v)
{
    char tmp[24];
    int32 n;
    n = 0;
    if (v == 0) {
        putc(48);
        return;
    }
    if (v < 0) {
        putc(45);
        v = 0 - v;
    }
    while (v > 0) {
        tmp[n] = (char)(48 + v % 10);
        n = n + 1;
        v = v / 10;
    }
    while (n > 0) {
        n = n - 1;
        putc(tmp[n]);
    }
}

void out_hex64(int64 v)
{
    int32 sh;
    sh = 60;
    while (sh >= 0) {
        int32 d;
        d = (int32)((v >> sh) & 15);
        if (d < 10) {
            putc(48 + d);
        } else {
            putc(87 + d);
        }
        sh = sh - 4;
    }
}

int32 streq(char *a, char *b)
{
    int32 i;
    i = 0;
    while (a[i] != 0 && b[i] != 0) {
        if (a[i] != b[i]) {
            return 0;
        }
        i = i + 1;
    }
    if (a[i] == 0 && b[i] == 0) {
        return 1;
    }
    return 0;
}


/* 大きな表とボイス状態はファイルスコープに置く。
 * main に大きなローカル配列を置くと IwaC のフレーム生成が
 * 壊れる (実測: フレーム 1504 バイトで addi sp の符号が反転し
 * スタック上に書いて即フォールト) ことへの対策でもある。
 * ワード単位の状態は int32 配列の直接アクセス (IwaC v63 で
 * nori が配列を 8 バイト境界に揃えるようになったため。
 * それ以前は char 配列 + バイト合成 rd32/wr32 が必要だった)。
 * バイト列そのもの (曲データ・プリセット記録・名前・
 * 出力バッファ) は char 配列のまま */
int32 g_sine[256];  /* 正弦波 256 点 */
int32 g_wav[2048];  /* TX81Z 風波形 8 種 x 256 点 (段2)。
                     * 行 0 は g_sine の写し (fill_waves) */
int32 g_pow2[256];  /* 2^(i/256) の Q15 表 (ピッチ EG 用) */
int32 g_ninc[61];   /* 音程表 61 点 */
char g_mel[32];
char g_bas[32];
int32 g_vs[128];    /* ボイス状態 (ワード単位で直接アクセス) */

/* プリセット (doc/synth-design.md のプリセットの節)。
 * 1 件 20 バイトのバイトパックで、FM 22 件 / PSG 16 件:
 *   +0 波形 (FM=0, PSG=1 矩形 / 2 ノイズ)  +1 レベルシフト
 *   +2 atk  +4 dec  +6 sus  +8 rel (各 2 バイト LE)
 *   FM : +10 変調比 x2  +12 変調の深さ  +14 変調 env 減衰
 *        +16 変調 env 減衰 (release 時)
 *   PSG: +10 デューティ閾値  +12 ノイズ混合量  +13 オクターブ
 * 名前は 16 バイト枠 (FM 22 件 / PSG 16 件)。g_vp は
 * ボイスごとの実効パラメータ (trig がプリセットから写す、
 * 1 ボイス 16 ワード):
 *   +0 atk +1 dec +2 sus +3 rel +4 変調の深さ +5 変調 env
 *   減衰 +6 同 (release) +7 デューティ +8 ノイズ混合
 *   +9 レベルシフト +10 op モード +11..+14 オペレータ
 *   波形のテーブル先頭 (波形番号 x 256、段2)
 * g_cur は現在の (エンジン, プリセット番号) の 2 ワード。
 * g_gmf / g_gmp は GM プログラム番号 (>>3 のファミリー)
 * からプリセット番号への対応表 */
char g_pfm[440];
char g_pps[320];
char g_fnm[352];
char g_pnm[256];
/* FM プリセットごとのオペレータ波形 (段2)。1 件 4 バイト、
 * 値は波形番号 0..7 (0 = サイン)。スロットの対応は
 * ボイスパラメータ g_vp / g_mvp の +11..+14 と同じ:
 *   +0 位相スロット +15 の op (4op スタック第 1 変調 /
 *      2+2 の第 2 ペア変調)
 *   +1 位相スロット +14 の op (スタック第 2 変調 /
 *      2+2 の第 2 ペア搬送)
 *   +2 主変調 op  +3 主搬送 op
 * 2op では +2 / +3 だけが鳴る。既定 0 (ゼロ初期化) =
 * 全 op サインで、既存プリセットの音は変わらない */
char g_wfm[88];
/* FM プリセットごとのピッチ EG (段2.5、DX11 風)。
 * 1 件 16 バイト: +0..+3 = R1..R4 (0..99)、
 * +4..+7 = L1..L4 (0..99、50 = 音程ずらしゼロ)、
 * +8 = レンジ (レベル最大時のずらし、半音単位)。
 * レンジ 0 = PEG オフ (既定。既存プリセットは全部オフ) */
char g_pegp[352];
/* ピッチ EG のボイスごとの状態。1 ボイス 16 ワード:
 *   +0 現在レベル (レベル単位 x 256、(L-50)*2 が単位)
 *   +1 セグメント (0=停止 1..3=L1..L3 へ移動中
 *      4=L4 へ移動中 (release))
 *   +2 / +3 実効位相増分 (搬送 / 変調。ブロックごとに
 *      基本増分 x ピッチ倍率で作り直す)
 *   +4..+7 R1..R4 を 1 ブロックあたりの歩幅に直したもの
 *   +8..+11 L1..L4 をレベル単位 x 256 に直したもの
 *   +12 レンジ  +13 有効フラグ (0 = オフ)
 *   +14 サイン以外の波形を使うボイスのフラグ
 *      (描画側が従来の sine_at 経路と振り分ける) */
int32 g_pegs[64];    /* デモ / 対話用 (4 ボイス) */
int32 g_mpegs[512];  /* MT 用 (プール最大 32 ボイス) */
int32 g_pegclk[4];   /* +0 デモ側 / +1 MT 側のブロック時計 */
int32 g_vp[128];
int32 g_cur[8];
char g_gmf[16];
char g_gmp[16];


/* 正弦波テーブル (Q15、256 点) を代入で埋める */
void fill_sine()
{
    g_sine[0] = 0;
    g_sine[1] = 804;
    g_sine[2] = 1608;
    g_sine[3] = 2410;
    g_sine[4] = 3212;
    g_sine[5] = 4011;
    g_sine[6] = 4808;
    g_sine[7] = 5602;
    g_sine[8] = 6393;
    g_sine[9] = 7179;
    g_sine[10] = 7962;
    g_sine[11] = 8739;
    g_sine[12] = 9512;
    g_sine[13] = 10278;
    g_sine[14] = 11039;
    g_sine[15] = 11793;
    g_sine[16] = 12539;
    g_sine[17] = 13279;
    g_sine[18] = 14010;
    g_sine[19] = 14732;
    g_sine[20] = 15446;
    g_sine[21] = 16151;
    g_sine[22] = 16846;
    g_sine[23] = 17530;
    g_sine[24] = 18204;
    g_sine[25] = 18868;
    g_sine[26] = 19519;
    g_sine[27] = 20159;
    g_sine[28] = 20787;
    g_sine[29] = 21403;
    g_sine[30] = 22005;
    g_sine[31] = 22594;
    g_sine[32] = 23170;
    g_sine[33] = 23731;
    g_sine[34] = 24279;
    g_sine[35] = 24811;
    g_sine[36] = 25329;
    g_sine[37] = 25832;
    g_sine[38] = 26319;
    g_sine[39] = 26790;
    g_sine[40] = 27245;
    g_sine[41] = 27683;
    g_sine[42] = 28105;
    g_sine[43] = 28510;
    g_sine[44] = 28898;
    g_sine[45] = 29268;
    g_sine[46] = 29621;
    g_sine[47] = 29956;
    g_sine[48] = 30273;
    g_sine[49] = 30571;
    g_sine[50] = 30852;
    g_sine[51] = 31113;
    g_sine[52] = 31356;
    g_sine[53] = 31580;
    g_sine[54] = 31785;
    g_sine[55] = 31971;
    g_sine[56] = 32137;
    g_sine[57] = 32285;
    g_sine[58] = 32412;
    g_sine[59] = 32521;
    g_sine[60] = 32609;
    g_sine[61] = 32678;
    g_sine[62] = 32728;
    g_sine[63] = 32757;
    g_sine[64] = 32767;
    /* 残りは対称性で展開する: 65..127 は 128-i の鏡像、
     * 128..255 は前半の符号反転 (表の値は従来と同一) */
    {
        int32 i;
        i = 65;
        while (i < 128) {
            g_sine[i] = g_sine[128 - i];
            i = i + 1;
        }
        i = 128;
        while (i < 256) {
            g_sine[i] = 0 - g_sine[i - 128];
            i = i + 1;
        }
    }
 }

/* MIDI ノート 36..96 の位相増分を代入で埋める */
void fill_ninc()
{
    g_ninc[0] = 97;
    g_ninc[1] = 103;
    g_ninc[2] = 109;
    g_ninc[3] = 116;
    g_ninc[4] = 122;
    g_ninc[5] = 130;
    g_ninc[6] = 137;
    g_ninc[7] = 146;
    g_ninc[8] = 154;
    g_ninc[9] = 163;
    g_ninc[10] = 173;
    g_ninc[11] = 183;
    g_ninc[12] = 194;
    g_ninc[13] = 206;
    g_ninc[14] = 218;
    g_ninc[15] = 231;
    g_ninc[16] = 245;
    g_ninc[17] = 259;
    g_ninc[18] = 275;
    g_ninc[19] = 291;
    g_ninc[20] = 309;
    g_ninc[21] = 327;
    g_ninc[22] = 346;
    g_ninc[23] = 367;
    g_ninc[24] = 389;
    g_ninc[25] = 412;
    g_ninc[26] = 436;
    g_ninc[27] = 462;
    g_ninc[28] = 490;
    g_ninc[29] = 519;
    g_ninc[30] = 550;
    g_ninc[31] = 583;
    g_ninc[32] = 617;
    g_ninc[33] = 654;
    g_ninc[34] = 693;
    g_ninc[35] = 734;
    g_ninc[36] = 778;
    g_ninc[37] = 824;
    g_ninc[38] = 873;
    g_ninc[39] = 925;
    g_ninc[40] = 980;
    g_ninc[41] = 1038;
    g_ninc[42] = 1100;
    g_ninc[43] = 1165;
    g_ninc[44] = 1234;
    g_ninc[45] = 1308;
    g_ninc[46] = 1386;
    g_ninc[47] = 1468;
    g_ninc[48] = 1555;
    g_ninc[49] = 1648;
    g_ninc[50] = 1746;
    g_ninc[51] = 1849;
    g_ninc[52] = 1959;
    g_ninc[53] = 2076;
    g_ninc[54] = 2199;
    g_ninc[55] = 2330;
    g_ninc[56] = 2469;
    g_ninc[57] = 2615;
    g_ninc[58] = 2771;
    g_ninc[59] = 2936;
    g_ninc[60] = 3110;
 }

/* デモ曲 (32 ステップ、1 ステップ = 0.25 秒で合計 8 秒)。
 * メロディとベース。0 は休符 (ベースは 2 ステップおき) */
void fill_song()
{
    g_mel[0] = 72;
    g_mel[1] = 76;
    g_mel[2] = 79;
    g_mel[3] = 76;
    g_mel[4] = 81;
    g_mel[5] = 79;
    g_mel[6] = 76;
    g_mel[7] = 72;
    g_mel[8] = 74;
    g_mel[9] = 76;
    g_mel[10] = 77;
    g_mel[11] = 79;
    g_mel[12] = 81;
    g_mel[13] = 79;
    g_mel[14] = 76;
    g_mel[15] = 0;
    g_mel[16] = 72;
    g_mel[17] = 76;
    g_mel[18] = 79;
    g_mel[19] = 76;
    g_mel[20] = 81;
    g_mel[21] = 84;
    g_mel[22] = 81;
    g_mel[23] = 79;
    g_mel[24] = 77;
    g_mel[25] = 76;
    g_mel[26] = 74;
    g_mel[27] = 71;
    g_mel[28] = 72;
    g_mel[29] = 0;
    g_mel[30] = 0;
    g_mel[31] = 0;
    g_bas[0] = 48;
    g_bas[1] = 0;
    g_bas[2] = 55;
    g_bas[3] = 0;
    g_bas[4] = 48;
    g_bas[5] = 0;
    g_bas[6] = 55;
    g_bas[7] = 0;
    g_bas[8] = 53;
    g_bas[9] = 0;
    g_bas[10] = 60;
    g_bas[11] = 0;
    g_bas[12] = 53;
    g_bas[13] = 0;
    g_bas[14] = 60;
    g_bas[15] = 0;
    g_bas[16] = 55;
    g_bas[17] = 0;
    g_bas[18] = 62;
    g_bas[19] = 0;
    g_bas[20] = 55;
    g_bas[21] = 0;
    g_bas[22] = 62;
    g_bas[23] = 0;
    g_bas[24] = 48;
    g_bas[25] = 0;
    g_bas[26] = 55;
    g_bas[27] = 0;
    g_bas[28] = 48;
    g_bas[29] = 0;
    g_bas[30] = 0;
    g_bas[31] = 0;
}

/* 波形テーブルを g_sine から導出する (段2)。定義の
 * 数式は make_waves.py が正本で、同スクリプトが FFT で
 * マニュアルの倍音構成と照合済み。当初は同スクリプトが
 * 生成する代入表を取り込む形だったが、iwac のノード数
 * 上限を超えたため、実行時に g_sine から整数演算で
 * 導出する形に変えた。W5 / W7 / W8 はサイン格子に
 * 一致するので生成表とビット同一、W2 / W4 / W6 の
 * 二乗は丸めで最大 2 LSB だけ生成表とずれる。
 * 後半が 0 の波形は g_wav のゼロ初期化に任せる */
void fill_waves(void)
{
    int32 i;
    int32 s;
    int32 a;
    int32 w;
    for (i = 0; i < 256; i++) {
        s = g_sine[i];
        a = s;
        if (a < 0) {
            a = 0 - a;
        }
        g_wav[i] = s;                       /* W1 サイン */
        g_wav[256 + i] = (s * a) >> 15;     /* W2 二乗 */
        g_wav[512 + i] = a;                 /* W3 全波整流 */
        if (i < 128) {
            g_wav[768 + i] = (s * a) >> 15; /* W4 半波二乗 */
            w = g_sine[i * 2];
            g_wav[1024 + i] = w;            /* W5 半波 2 倍音 */
            if (w < 0) {
                g_wav[1280 + i] = 0 - ((w * w) >> 15);
            } else {
                g_wav[1280 + i] = (w * w) >> 15;
            }                               /* W6 W5 の二乗 */
            g_wav[1792 + i] = w;
            if (w < 0) {
                g_wav[1792 + i] = 0 - w;
            }                               /* W8 半波全波 2 倍音 */
        }
        if (i >= 128) {
            w = g_sine[(i + 64) & 255];
            if (w < 0) {
                w = 0 - w;
            }
            g_wav[1536 + i] = w;            /* W7 後半 |cos| */
        } else {
            g_wav[1536 + i] = s;            /* W7 前半 サイン */
        }
    }
}

/* make_waves.py が生成 (手で直さない) */
void fill_pow2(void)
{
    g_pow2[0] = 32768; g_pow2[1] = 32857; g_pow2[2] = 32946; g_pow2[3] = 33035;
    g_pow2[4] = 33125; g_pow2[5] = 33215; g_pow2[6] = 33305; g_pow2[7] = 33395;
    g_pow2[8] = 33486; g_pow2[9] = 33576; g_pow2[10] = 33667; g_pow2[11] = 33759;
    g_pow2[12] = 33850; g_pow2[13] = 33942; g_pow2[14] = 34034; g_pow2[15] = 34126;
    g_pow2[16] = 34219; g_pow2[17] = 34312; g_pow2[18] = 34405; g_pow2[19] = 34498;
    g_pow2[20] = 34591; g_pow2[21] = 34685; g_pow2[22] = 34779; g_pow2[23] = 34874;
    g_pow2[24] = 34968; g_pow2[25] = 35063; g_pow2[26] = 35158; g_pow2[27] = 35253;
    g_pow2[28] = 35349; g_pow2[29] = 35445; g_pow2[30] = 35541; g_pow2[31] = 35637;
    g_pow2[32] = 35734; g_pow2[33] = 35831; g_pow2[34] = 35928; g_pow2[35] = 36025;
    g_pow2[36] = 36123; g_pow2[37] = 36221; g_pow2[38] = 36319; g_pow2[39] = 36417;
    g_pow2[40] = 36516; g_pow2[41] = 36615; g_pow2[42] = 36715; g_pow2[43] = 36814;
    g_pow2[44] = 36914; g_pow2[45] = 37014; g_pow2[46] = 37114; g_pow2[47] = 37215;
    g_pow2[48] = 37316; g_pow2[49] = 37417; g_pow2[50] = 37518; g_pow2[51] = 37620;
    g_pow2[52] = 37722; g_pow2[53] = 37824; g_pow2[54] = 37927; g_pow2[55] = 38030;
    g_pow2[56] = 38133; g_pow2[57] = 38236; g_pow2[58] = 38340; g_pow2[59] = 38444;
    g_pow2[60] = 38548; g_pow2[61] = 38653; g_pow2[62] = 38757; g_pow2[63] = 38863;
    g_pow2[64] = 38968; g_pow2[65] = 39074; g_pow2[66] = 39180; g_pow2[67] = 39286;
    g_pow2[68] = 39392; g_pow2[69] = 39499; g_pow2[70] = 39606; g_pow2[71] = 39714;
    g_pow2[72] = 39821; g_pow2[73] = 39929; g_pow2[74] = 40037; g_pow2[75] = 40146;
    g_pow2[76] = 40255; g_pow2[77] = 40364; g_pow2[78] = 40473; g_pow2[79] = 40583;
    g_pow2[80] = 40693; g_pow2[81] = 40804; g_pow2[82] = 40914; g_pow2[83] = 41025;
    g_pow2[84] = 41136; g_pow2[85] = 41248; g_pow2[86] = 41360; g_pow2[87] = 41472;
    g_pow2[88] = 41584; g_pow2[89] = 41697; g_pow2[90] = 41810; g_pow2[91] = 41923;
    g_pow2[92] = 42037; g_pow2[93] = 42151; g_pow2[94] = 42265; g_pow2[95] = 42380;
    g_pow2[96] = 42495; g_pow2[97] = 42610; g_pow2[98] = 42726; g_pow2[99] = 42841;
    g_pow2[100] = 42958; g_pow2[101] = 43074; g_pow2[102] = 43191; g_pow2[103] = 43308;
    g_pow2[104] = 43425; g_pow2[105] = 43543; g_pow2[106] = 43661; g_pow2[107] = 43780;
    g_pow2[108] = 43898; g_pow2[109] = 44017; g_pow2[110] = 44137; g_pow2[111] = 44256;
    g_pow2[112] = 44376; g_pow2[113] = 44497; g_pow2[114] = 44617; g_pow2[115] = 44738;
    g_pow2[116] = 44859; g_pow2[117] = 44981; g_pow2[118] = 45103; g_pow2[119] = 45225;
    g_pow2[120] = 45348; g_pow2[121] = 45471; g_pow2[122] = 45594; g_pow2[123] = 45718;
    g_pow2[124] = 45842; g_pow2[125] = 45966; g_pow2[126] = 46091; g_pow2[127] = 46216;
    g_pow2[128] = 46341; g_pow2[129] = 46467; g_pow2[130] = 46593; g_pow2[131] = 46719;
    g_pow2[132] = 46846; g_pow2[133] = 46973; g_pow2[134] = 47100; g_pow2[135] = 47228;
    g_pow2[136] = 47356; g_pow2[137] = 47484; g_pow2[138] = 47613; g_pow2[139] = 47742;
    g_pow2[140] = 47871; g_pow2[141] = 48001; g_pow2[142] = 48131; g_pow2[143] = 48262;
    g_pow2[144] = 48393; g_pow2[145] = 48524; g_pow2[146] = 48655; g_pow2[147] = 48787;
    g_pow2[148] = 48920; g_pow2[149] = 49052; g_pow2[150] = 49185; g_pow2[151] = 49319;
    g_pow2[152] = 49452; g_pow2[153] = 49586; g_pow2[154] = 49721; g_pow2[155] = 49856;
    g_pow2[156] = 49991; g_pow2[157] = 50126; g_pow2[158] = 50262; g_pow2[159] = 50399;
    g_pow2[160] = 50535; g_pow2[161] = 50672; g_pow2[162] = 50810; g_pow2[163] = 50947;
    g_pow2[164] = 51085; g_pow2[165] = 51224; g_pow2[166] = 51363; g_pow2[167] = 51502;
    g_pow2[168] = 51642; g_pow2[169] = 51782; g_pow2[170] = 51922; g_pow2[171] = 52063;
    g_pow2[172] = 52204; g_pow2[173] = 52346; g_pow2[174] = 52488; g_pow2[175] = 52630;
    g_pow2[176] = 52773; g_pow2[177] = 52916; g_pow2[178] = 53059; g_pow2[179] = 53203;
    g_pow2[180] = 53347; g_pow2[181] = 53492; g_pow2[182] = 53637; g_pow2[183] = 53782;
    g_pow2[184] = 53928; g_pow2[185] = 54074; g_pow2[186] = 54221; g_pow2[187] = 54368;
    g_pow2[188] = 54515; g_pow2[189] = 54663; g_pow2[190] = 54811; g_pow2[191] = 54960;
    g_pow2[192] = 55109; g_pow2[193] = 55258; g_pow2[194] = 55408; g_pow2[195] = 55558;
    g_pow2[196] = 55709; g_pow2[197] = 55860; g_pow2[198] = 56012; g_pow2[199] = 56163;
    g_pow2[200] = 56316; g_pow2[201] = 56468; g_pow2[202] = 56622; g_pow2[203] = 56775;
    g_pow2[204] = 56929; g_pow2[205] = 57083; g_pow2[206] = 57238; g_pow2[207] = 57393;
    g_pow2[208] = 57549; g_pow2[209] = 57705; g_pow2[210] = 57861; g_pow2[211] = 58018;
    g_pow2[212] = 58176; g_pow2[213] = 58333; g_pow2[214] = 58491; g_pow2[215] = 58650;
    g_pow2[216] = 58809; g_pow2[217] = 58968; g_pow2[218] = 59128; g_pow2[219] = 59289;
    g_pow2[220] = 59449; g_pow2[221] = 59611; g_pow2[222] = 59772; g_pow2[223] = 59934;
    g_pow2[224] = 60097; g_pow2[225] = 60260; g_pow2[226] = 60423; g_pow2[227] = 60587;
    g_pow2[228] = 60751; g_pow2[229] = 60916; g_pow2[230] = 61081; g_pow2[231] = 61247;
    g_pow2[232] = 61413; g_pow2[233] = 61579; g_pow2[234] = 61746; g_pow2[235] = 61914;
    g_pow2[236] = 62081; g_pow2[237] = 62250; g_pow2[238] = 62419; g_pow2[239] = 62588;
    g_pow2[240] = 62757; g_pow2[241] = 62928; g_pow2[242] = 63098; g_pow2[243] = 63269;
    g_pow2[244] = 63441; g_pow2[245] = 63613; g_pow2[246] = 63785; g_pow2[247] = 63958;
    g_pow2[248] = 64132; g_pow2[249] = 64306; g_pow2[250] = 64480; g_pow2[251] = 64655;
    g_pow2[252] = 64830; g_pow2[253] = 65006; g_pow2[254] = 65182; g_pow2[255] = 65359;
}

/* 正弦波の線形補間。phase は 1 周 = 65536 */
int32 sine_at(int32 phase)
{
    int32 idx;
    int32 frac;
    int32 a;
    int32 b;
    idx = phase >> 8;
    frac = phase & 255;
    a = g_sine[idx];
    b = g_sine[(idx + 1) & 255];
    return a + (((b - a) * frac) >> 8);
}

/* TX81Z 風波形の線形補間 (段2)。base は波形のテーブル
 * 先頭 (波形番号 x 256)。base = 0 は g_sine の写しなので
 * sine_at とビット同一の値を返す */
int32 wave_at(int32 base, int32 phase)
{
    int32 idx;
    int32 frac;
    int32 a;
    int32 b;
    idx = phase >> 8;
    frac = phase & 255;
    a = g_wav[base + idx];
    b = g_wav[base + ((idx + 1) & 255)];
    return a + (((b - a) * frac) >> 8);
}


int32 rd16u(char *p, int32 off)
{
    return (p[off] & 255) | ((p[off + 1] & 255) << 8);
}

void put16(char *p, int32 off, int32 v)
{
    p[off] = (char)(v & 255);
    p[off + 1] = (char)((v >> 8) & 255);
}

void pset_c(char *tab, int32 idx, int32 wave, int32 lvl,
            int32 atk, int32 dec, int32 sus, int32 rel)
{
    int32 o;
    o = idx * 20;
    tab[o] = (char)wave;
    tab[o + 1] = (char)lvl;
    put16(tab, o + 2, atk);
    put16(tab, o + 4, dec);
    put16(tab, o + 6, sus);
    put16(tab, o + 8, rel);
}

/* FM の追加分 (変調比・深さ・変調 env)。pset_c と対で使う
 * (IwaC は引数 8 つまでなので分けてある) */
void pset_fx(int32 idx, int32 ratio, int32 depth,
             int32 mrate, int32 mrel)
{
    int32 o;
    o = idx * 20;
    g_pfm[o + 10] = (char)ratio;
    put16(g_pfm, o + 12, depth);
    put16(g_pfm, o + 14, mrate);
    put16(g_pfm, o + 16, mrel);
}

/* PSG の追加分 (デューティ・ノイズ混合・オクターブ)。
 * pset_c と対で使う (引数 8 つまでのため) */
void pset_px(int32 idx, int32 duty, int32 nmix, int32 oct)
{
    int32 o;
    o = idx * 20;
    put16(g_pps, o + 10, duty);
    g_pps[o + 12] = (char)nmix;
    g_pps[o + 13] = (char)oct;
}

/* FM プリセットのオペレータ波形 (段2)。引数は波形番号
 * 0..7 をスロット順 (位相 +15 の op、位相 +14 の op、
 * 主変調、主搬送) に並べたもの */
void pset_w(int32 idx, int32 w0, int32 w1, int32 w2, int32 w3)
{
    g_wfm[idx * 4 + 0] = (char)w0;
    g_wfm[idx * 4 + 1] = (char)w1;
    g_wfm[idx * 4 + 2] = (char)w2;
    g_wfm[idx * 4 + 3] = (char)w3;
}

/* ピッチ EG のレート側 (段2.5)。pset_peg2 と対で使う */
void pset_peg1(int32 idx, int32 r1, int32 r2, int32 r3,
               int32 r4, int32 l1, int32 l2)
{
    int32 o;
    o = idx * 16;
    g_pegp[o + 0] = (char)r1;
    g_pegp[o + 1] = (char)r2;
    g_pegp[o + 2] = (char)r3;
    g_pegp[o + 3] = (char)r4;
    g_pegp[o + 4] = (char)l1;
    g_pegp[o + 5] = (char)l2;
}

/* ピッチ EG のレベル残りとレンジ (段2.5) */
void pset_peg2(int32 idx, int32 l3, int32 l4, int32 range)
{
    int32 o;
    o = idx * 16;
    g_pegp[o + 6] = (char)l3;
    g_pegp[o + 7] = (char)l4;
    g_pegp[o + 8] = (char)range;
}

void set_name(char *tab, int32 idx, char *s)
{
    int32 i;
    i = 0;
    while (i < 15 && s[i] != 0) {
        tab[idx * 16 + i] = s[i];
        i = i + 1;
    }
    while (i < 16) {
        tab[idx * 16 + i] = 0;
        i = i + 1;
    }
}

/* プリセット表を埋める。0 番は段1 の従来音色そのもの
 * (パラメータが全て従来の固定値と一致し、デモの FNV が
 * 変わらないことを確認済み) */
void fill_presets()
{
    pset_c(g_pfm, 0, 0, 2, 600, 20, 40000, 80);
    pset_fx(0, 7, 20000, 30, 200);
    pset_c(g_pfm, 1, 0, 2, 1200, 2, 14000, 50);
    pset_fx(1, 2, 16000, 12, 80);
    pset_c(g_pfm, 2, 0, 2, 2000, 6, 0, 90);
    pset_fx(2, 8, 14000, 60, 300);
    pset_c(g_pfm, 3, 0, 2, 800, 2, 20000, 40);
    pset_fx(3, 6, 8000, 8, 60);
    pset_c(g_pfm, 4, 0, 2, 1800, 10, 0, 120);
    pset_fx(4, 8, 10000, 45, 200);
    pset_c(g_pfm, 5, 0, 2, 120, 1, 60000, 30);
    pset_fx(5, 4, 5000, 2, 20);
    pset_c(g_pfm, 6, 0, 2, 1600, 5, 6000, 70);
    pset_fx(6, 3, 11000, 35, 150);
    pset_c(g_pfm, 7, 0, 1, 1400, 3, 22000, 45);
    pset_fx(7, 5, 15000, 15, 60);
    pset_c(g_pfm, 8, 0, 1, 1300, 4, 12000, 55);
    pset_fx(8, 2, 7000, 25, 100);
    pset_c(g_pfm, 9, 0, 1, 1500, 4, 10000, 55);
    pset_fx(9, 3, 12000, 30, 120);
    pset_c(g_pfm, 10, 0, 2, 60, 1, 52000, 25);
    pset_fx(10, 4, 4500, 3, 25);
    pset_c(g_pfm, 11, 0, 1, 220, 2, 46000, 35);
    pset_fx(11, 4, 6500, 6, 40);
    pset_c(g_pfm, 12, 0, 2, 150, 1, 50000, 30);
    pset_fx(12, 2, 2500, 4, 30);
    pset_c(g_pfm, 13, 0, 2, 1700, 2, 6000, 35);
    pset_fx(13, 11, 13000, 18, 90);
    pset_c(g_pfm, 14, 0, 1, 700, 1, 48000, 40);
    pset_fx(14, 2, 8000, 5, 50);
    pset_c(g_pfm, 15, 0, 2, 45, 1, 54000, 18);
    pset_fx(15, 6, 6000, 2, 20);
    set_name(g_fnm, 0, "Grand Piano");
    set_name(g_fnm, 1, "Electric Piano");
    set_name(g_fnm, 2, "Music Box");
    set_name(g_fnm, 3, "Vibraphone");
    set_name(g_fnm, 4, "Marimba");
    set_name(g_fnm, 5, "Church Organ");
    set_name(g_fnm, 6, "Acoustic Guitar");
    set_name(g_fnm, 7, "Electric Guitar");
    set_name(g_fnm, 8, "Acoustic Bass");
    set_name(g_fnm, 9, "Electric Bass");
    set_name(g_fnm, 10, "Strings");
    set_name(g_fnm, 11, "Trumpet");
    set_name(g_fnm, 12, "Flute");
    set_name(g_fnm, 13, "Tubular Bells");
    set_name(g_fnm, 14, "Synth Lead");
    set_name(g_fnm, 15, "Synth Pad");
    pset_c(g_pps, 0, 1, 2, 600, 20, 40000, 80);
    pset_px(0, 32768, 0, 0);
    pset_c(g_pps, 1, 1, 2, 900, 4, 14000, 60);
    pset_px(1, 16384, 0, 0);
    pset_c(g_pps, 2, 1, 2, 1600, 8, 0, 100);
    pset_px(2, 8192, 0, 1);
    pset_c(g_pps, 3, 1, 2, 500, 2, 20000, 45);
    pset_px(3, 24576, 0, 0);
    pset_c(g_pps, 4, 1, 2, 1700, 12, 0, 130);
    pset_px(4, 32768, 30, 0);
    pset_c(g_pps, 5, 1, 2, 130, 1, 60000, 30);
    pset_px(5, 32768, 0, 0);
    pset_c(g_pps, 6, 1, 2, 1500, 5, 6000, 70);
    pset_px(6, 20500, 15, 0);
    pset_c(g_pps, 7, 1, 1, 1200, 4, 14000, 55);
    pset_px(7, 32768, 0, -1);
    pset_c(g_pps, 8, 1, 2, 55, 1, 52000, 25);
    pset_px(8, 16384, 0, 0);
    pset_c(g_pps, 9, 1, 1, 200, 2, 46000, 35);
    pset_px(9, 12288, 0, 0);
    pset_c(g_pps, 10, 1, 2, 140, 1, 50000, 30);
    pset_px(10, 40960, 0, 0);
    pset_c(g_pps, 11, 1, 2, 1800, 3, 4000, 40);
    pset_px(11, 8192, 0, 1);
    pset_c(g_pps, 12, 1, 1, 800, 1, 48000, 40);
    pset_px(12, 32768, 0, 0);
    pset_c(g_pps, 13, 1, 1, 800, 1, 48000, 40);
    pset_px(13, 6000, 20, 0);
    pset_c(g_pps, 14, 2, 2, 2000, 25, 0, 200);
    pset_px(14, 0, 0, 0);
    pset_c(g_pps, 15, 1, 2, 40, 1, 54000, 18);
    pset_px(15, 24576, 0, 0);
    set_name(g_pnm, 0, "Chiptune Piano");
    set_name(g_pnm, 1, "E.Piano");
    set_name(g_pnm, 2, "Music Box");
    set_name(g_pnm, 3, "Vibraphone");
    set_name(g_pnm, 4, "Marimba");
    set_name(g_pnm, 5, "Organ");
    set_name(g_pnm, 6, "Guitar");
    set_name(g_pnm, 7, "Bass");
    set_name(g_pnm, 8, "Strings");
    set_name(g_pnm, 9, "Trumpet");
    set_name(g_pnm, 10, "Flute");
    set_name(g_pnm, 11, "Bell");
    set_name(g_pnm, 12, "Square Lead");
    set_name(g_pnm, 13, "Saw Lead");
    set_name(g_pnm, 14, "Noise Perc.");
    set_name(g_pnm, 15, "Synth Pad");
    g_gmf[0] = 0; g_gmf[1] = 2; g_gmf[2] = 5; g_gmf[3] = 6;
    g_gmf[4] = 8; g_gmf[5] = 10; g_gmf[6] = 10; g_gmf[7] = 11;
    g_gmf[8] = 11; g_gmf[9] = 12; g_gmf[10] = 14; g_gmf[11] = 15;
    g_gmf[12] = 14; g_gmf[13] = 6; g_gmf[14] = 13; g_gmf[15] = 13;
    g_gmp[0] = 0; g_gmp[1] = 2; g_gmp[2] = 5; g_gmp[3] = 6;
    g_gmp[4] = 7; g_gmp[5] = 8; g_gmp[6] = 8; g_gmp[7] = 9;
    g_gmp[8] = 9; g_gmp[9] = 10; g_gmp[10] = 12; g_gmp[11] = 15;
    g_gmp[12] = 12; g_gmp[13] = 6; g_gmp[14] = 14; g_gmp[15] = 14;
    /* 段2: TX81Z 風プリセット (末尾 16..19、サイン以外の
     * 波形を使う)。波形番号は make_waves.py の定義:
     * 0=サイン 1=二乗 2=全波整流 3=半波二乗 4=半波2倍音
     * 5=半波2倍音二乗 6=折り返し 7=半波全波2倍音 */
    pset_c(g_pfm, 16, 0, 1, 900, 4, 26000, 60);
    pset_fx(16, 2, 14000, 20, 120);
    pset_w(16, 4, 1, 1, 2);
    set_name(g_fnm, 16, "TX Brass");
    pset_c(g_pfm, 17, 0, 2, 1200, 2, 12000, 50);
    pset_fx(17, 2, 15000, 12, 80);
    pset_w(17, 5, 1, 4, 0);
    set_name(g_fnm, 17, "TX E.Piano");
    pset_c(g_pfm, 18, 0, 1, 1500, 5, 9000, 55);
    pset_fx(18, 2, 12000, 25, 100);
    pset_w(18, 0, 0, 6, 2);
    set_name(g_fnm, 18, "TX Bass");
    pset_c(g_pfm, 19, 0, 1, 1400, 3, 20000, 45);
    pset_fx(19, 3, 13000, 15, 60);
    pset_w(19, 3, 7, 1, 6);
    set_name(g_fnm, 19, "TX Lead");
    /* 段2.5: ピッチ EG を使うプリセット (末尾 20..21)。
     * 波形は全部サイン (PEG の効果だけを聞き分ける) */
    pset_c(g_pfm, 20, 0, 1, 1000, 4, 24000, 60);
    pset_fx(20, 2, 13000, 18, 100);
    pset_peg1(20, 90, 35, 10, 50, 78, 50);
    pset_peg2(20, 50, 44, 12);
    set_name(g_fnm, 20, "PEG Brass");
    pset_c(g_pfm, 21, 0, 1, 1600, 3, 18000, 40);
    pset_fx(21, 4, 9000, 10, 60);
    pset_peg1(21, 99, 22, 10, 30, 88, 50);
    pset_peg2(21, 50, 50, 12);
    set_name(g_fnm, 21, "PEG Drop");
    /* FM の op モード (プリセット +18 バイト): 0=2op、
     * 1=4op 直列スタック、2=4op 2+2 ペア。0 番は従来音色の
     * 保存のため 2op のまま (デモ FNV 不変) */
    g_pfm[1 * 20 + 18] = 1;
    g_pfm[6 * 20 + 18] = 1;
    g_pfm[7 * 20 + 18] = 2;
    g_pfm[8 * 20 + 18] = 1;
    g_pfm[9 * 20 + 18] = 2;
    g_pfm[11 * 20 + 18] = 2;
    g_pfm[14 * 20 + 18] = 2;
    g_pfm[16 * 20 + 18] = 1;
    g_pfm[17 * 20 + 18] = 2;
    g_pfm[19 * 20 + 18] = 1;
    g_pfm[20 * 20 + 18] = 1;
}

/* ピッチ EG (段2.5、DX11 風)。ボイスごとに 1 本で、
 * 全オペレータの音程をまとめて動かす (OPZ と同じ)。
 * レベルは音程のずらし量を対数側で持つ: レベル単位
 * (L-50) x 2 = -100..+98 を x 256 の固定小数点で保持し、
 * 半音のずらし = レベル単位 x レンジ / 100。倍率は
 * 2^(ずらし / 12) を g_pow2 表 (1 オクターブ分) +
 * オクターブシフトで作る。進行は 64 サンプル = 1
 * ブロック単位 (mix 側の g_pegclk が数える)。
 * note on で L1 -> L2 -> L3 と進んで L3 で保持し、
 * release (env stage 3) に入ると L4 へ向かう。
 * 開始レベルは L4 (DX の作法)。レートの歩幅は
 * (R+1) x 8 レベル単位 x 256 / ブロックで、R=99 の
 * 全振幅が約 46ms、R=0 は約 4.6 秒 (DX の 0 = 最遅
 * の割り切り)。レンジ 0 のプリセットでは実効増分が
 * 基本増分の写しになるだけなので音は変わらない */

/* 現在レベルからピッチ倍率 (Q15、32768 = x1.0) を作る */
int32 peg_factor(int32 *sts, int32 sb)
{
    int32 semi;
    int32 oct;
    int32 whole;
    int32 frac;
    int32 f;
    semi = (sts[sb + 0] * sts[sb + 12]) / 100;
    oct = semi / 12;
    if (oct > 512) {
        oct = 512;
    }
    if (oct < 0 - 512) {
        oct = 0 - 512;
    }
    if (oct >= 0) {
        whole = oct >> 8;
        frac = oct & 255;
        f = g_pow2[frac] << whole;
    } else {
        int32 a;
        a = 0 - oct;
        whole = a >> 8;
        frac = a & 255;
        f = g_pow2[frac] << whole;
        f = (32768 * 32768) / f;
    }
    return f;
}

/* 実効増分 (搬送 / 変調) を基本増分 x 倍率で作り直す */
void peg_apply(int32 *sts, int32 sb, int32 *vs, int32 vb)
{
    int32 f;
    if (sts[sb + 13] == 0) {
        sts[sb + 2] = vs[vb + 10];
        sts[sb + 3] = vs[vb + 11];
        return;
    }
    f = peg_factor(sts, sb);
    sts[sb + 2] = (int32)(((int64)vs[vb + 10] * f) >> 15);
    sts[sb + 3] = (int32)(((int64)vs[vb + 11] * f) >> 15);
}

/* トリガ時の初期化。vs は基本増分を置き終えた後で呼ぶ */
void peg_init(int32 *sts, int32 sb, int32 pidx,
              int32 *vs, int32 vb)
{
    int32 o;
    o = pidx * 16;
    sts[sb + 12] = g_pegp[o + 8] & 255;
    if (sts[sb + 12] == 0) {
        sts[sb + 13] = 0;
        sts[sb + 1] = 0;
        sts[sb + 0] = 0;
        sts[sb + 2] = vs[vb + 10];
        sts[sb + 3] = vs[vb + 11];
        return;
    }
    sts[sb + 13] = 1;
    sts[sb + 4] = ((g_pegp[o + 0] & 255) + 1) * 8;
    sts[sb + 5] = ((g_pegp[o + 1] & 255) + 1) * 8;
    sts[sb + 6] = ((g_pegp[o + 2] & 255) + 1) * 8;
    sts[sb + 7] = ((g_pegp[o + 3] & 255) + 1) * 8;
    sts[sb + 8] = ((g_pegp[o + 4] & 255) - 50) * 512;
    sts[sb + 9] = ((g_pegp[o + 5] & 255) - 50) * 512;
    sts[sb + 10] = ((g_pegp[o + 6] & 255) - 50) * 512;
    sts[sb + 11] = ((g_pegp[o + 7] & 255) - 50) * 512;
    sts[sb + 0] = sts[sb + 11];
    sts[sb + 1] = 1;
    peg_apply(sts, sb, vs, vb);
}

/* 1 ブロック分進める。vs の +6 (env stage) が 3 に
 * 入っていたら release 側 (L4 へ) に切り替える */
void peg_step(int32 *sts, int32 sb, int32 *vs, int32 vb)
{
    int32 seg;
    int32 tgt;
    int32 rate;
    int32 lvl;
    if (sts[sb + 13] == 0) {
        sts[sb + 2] = vs[vb + 10];
        sts[sb + 3] = vs[vb + 11];
        return;
    }
    seg = sts[sb + 1];
    if (vs[vb + 6] == 3 && seg != 0 && seg != 4) {
        seg = 4;
    }
    tgt = sts[sb + 0];
    rate = 0;
    if (seg == 1) {
        tgt = sts[sb + 8];
        rate = sts[sb + 4];
    } else if (seg == 2) {
        tgt = sts[sb + 9];
        rate = sts[sb + 5];
    } else if (seg == 3) {
        tgt = sts[sb + 10];
        rate = sts[sb + 6];
    } else if (seg == 4) {
        tgt = sts[sb + 11];
        rate = sts[sb + 7];
    }
    lvl = sts[sb + 0];
    if (lvl < tgt) {
        lvl = lvl + rate;
        if (lvl > tgt) {
            lvl = tgt;
        }
    } else if (lvl > tgt) {
        lvl = lvl - rate;
        if (lvl < tgt) {
            lvl = tgt;
        }
    }
    if (lvl == tgt) {
        if (seg == 1) {
            seg = 2;
        } else if (seg == 2) {
            seg = 3;
        } else if (seg == 4) {
            seg = 0;
        }
    }
    sts[sb + 0] = lvl;
    sts[sb + 1] = seg;
    peg_apply(sts, sb, vs, vb);
}

/* デモ / 対話側のブロック駆動 (4 ボイス分) */
void peg_block_demo()
{
    int32 vi;
    vi = 0;
    while (vi < 4) {
        if (g_vs[vi * 16] != 0) {
            peg_step(g_pegs, vi * 16, g_vs, vi * 16);
        }
        vi = vi + 1;
    }
}

/* "synth: preset <番号> <名前>" を印字する */
void print_preset(int32 eng, int32 idx)
{
    int32 i;
    out_str("synth: preset ");
    out_num(idx);
    putc(32);
    i = 0;
    while (i < 16) {
        int32 c;
        if (eng == 0) {
            c = g_fnm[idx * 16 + i] & 255;
        } else {
            c = g_pnm[idx * 16 + i] & 255;
        }
        if (c == 0) {
            break;
        }
        putc(c);
        i = i + 1;
    }
    putc(10);
}

/* 10 進文字列を数にする。不正な文字があれば -1 */
int32 parse_num(char *s)
{
    int32 v;
    int32 i;
    v = 0;
    i = 0;
    if (s[0] == 0) {
        return 0 - 1;
    }
    while (s[i] != 0) {
        if (s[i] < 48 || s[i] > 57) {
            return 0 - 1;
        }
        v = v * 10 + (s[i] - 48);
        i = i + 1;
    }
    return v;
}

/* ボイス状態 (g_vs の中): 1 ボイス 16 ワード、4 ボイス分。
 *   +0 active  +1 type (0=FM 1=矩形 2=ノイズ)  +2 note
 *   +3 搬送位相  +4 変調位相  +5 env (0..65535)
 *   +6 env stage (0=A 1=D 2=S 3=R)  +7 gate 残りサンプル
 *   +8 変調 env  +9 LFSR  +10 搬送増分  +11 変調増分
 * trig の引数 rr が次に使うボイス番号 (ラウンドロビン)。
 * type が負なら波形は現在のプリセットから取る */
void trig(int32 rr, int32 note, int32 gate, int32 type)
{
    int32 b;
    int32 eng;
    int32 o;
    char *tab;
    int32 inc;
    int32 oct;
    b = rr * 16;
    eng = g_cur[0];
    if (eng == 0) {
        tab = g_pfm;
    } else {
        tab = g_pps;
    }
    o = g_cur[1] * 20;
    if (type < 0) {
        type = tab[o] & 255;  /* 波形はプリセットから */
    }
    g_vs[b + 0] = 1;
    g_vs[b + 1] = type;
    g_vs[b + 2] = note;
    g_vs[b + 3] = 0;
    g_vs[b + 4] = 0;
    g_vs[b + 5] = 0;
    g_vs[b + 6] = 0;
    g_vs[b + 7] = gate;
    g_vs[b + 8] = 65535;
    g_vs[b + 9] = 257;
    g_vs[b + 14] = 0;
    g_vs[b + 15] = 0;
    g_vp[b + 0] = rd16u(tab, o + 2);
    g_vp[b + 1] = rd16u(tab, o + 4);
    g_vp[b + 2] = rd16u(tab, o + 6);
    g_vp[b + 3] = rd16u(tab, o + 8);
    g_vp[b + 9] = tab[o + 1] & 255;
    if (eng == 0) {
        g_vp[b + 4] = rd16u(tab, o + 12);
        g_vp[b + 5] = rd16u(tab, o + 14);
        g_vp[b + 6] = rd16u(tab, o + 16);
        g_vp[b + 7] = 32768;
        g_vp[b + 8] = 0;
        g_vp[b + 10] = tab[o + 18] & 255;
        g_vp[b + 11] = (g_wfm[g_cur[1] * 4 + 0] & 255) * 256;
        g_vp[b + 12] = (g_wfm[g_cur[1] * 4 + 1] & 255) * 256;
        g_vp[b + 13] = (g_wfm[g_cur[1] * 4 + 2] & 255) * 256;
        g_vp[b + 14] = (g_wfm[g_cur[1] * 4 + 3] & 255) * 256;
    } else {
        g_vp[b + 4] = 0;
        g_vp[b + 5] = 30;
        g_vp[b + 6] = 200;
        g_vp[b + 7] = rd16u(tab, o + 10);
        g_vp[b + 8] = tab[o + 12] & 255;
        g_vp[b + 10] = 0;
        g_vp[b + 11] = 0;
        g_vp[b + 12] = 0;
        g_vp[b + 13] = 0;
        g_vp[b + 14] = 0;
    }
    if (note >= 36 && note <= 96) {
        inc = g_ninc[note - 36];
    } else {
        inc = 0;
    }
    if (eng == 1) {
        oct = tab[o + 13] & 255;
        if (oct >= 128) {
            oct = oct - 256;
        }
        if (oct > 0) {
            inc = inc << oct;
        } else if (oct < 0) {
            inc = inc >> (0 - oct);
        }
    }
    g_vs[b + 10] = inc;
    if (eng == 0) {
        g_vs[b + 11] = (inc * (tab[o + 10] & 255)) / 2;
    } else {
        g_vs[b + 11] = inc;
    }
    if (eng == 0) {
        peg_init(g_pegs, b, g_cur[1], g_vs, b);
        if (g_vp[b + 11] + g_vp[b + 12] + g_vp[b + 13] +
            g_vp[b + 14] != 0) {
            g_pegs[b + 14] = 1;
        } else {
            g_pegs[b + 14] = 0;
        }
    } else {
        g_pegs[b + 13] = 0;
        g_pegs[b + 14] = 0;
        g_pegs[b + 2] = g_vs[b + 10];
        g_pegs[b + 3] = g_vs[b + 11];
    }
}

/* 1 サンプル分のミックス (全ボイスの env も 1 段進める) */
int32 mix_sample()
{
    int32 sum;
    int32 vi;
    sum = 0;
    g_pegclk[0] = g_pegclk[0] + 1;
    if (g_pegclk[0] >= 64) {
        g_pegclk[0] = 0;
        peg_block_demo();
    }
    vi = 0;
    while (vi < 4) {
        int32 b;
        b = vi * 16;
        if (g_vs[b + 0] != 0) {
            int32 typ;
            int32 pc;
            int32 pm;
            int32 env;
            int32 stg;
            int32 gate;
            int32 menv;
            int32 lfsr;
            int32 inc_c;
            int32 inc_m;
            int32 atk;
            int32 dec;
            int32 sus;
            int32 rel;
            int32 dep;
            int32 mrate;
            int32 mrel;
            int32 duty;
            int32 nmix;
            int32 lvl;
            int32 outv;
            typ = g_vs[b + 1];
            pc = g_vs[b + 3];
            pm = g_vs[b + 4];
            env = g_vs[b + 5];
            stg = g_vs[b + 6];
            gate = g_vs[b + 7];
            menv = g_vs[b + 8];
            lfsr = g_vs[b + 9];
            inc_c = g_pegs[b + 2];
            inc_m = g_pegs[b + 3];
            atk = g_vp[b + 0];
            dec = g_vp[b + 1];
            sus = g_vp[b + 2];
            rel = g_vp[b + 3];
            dep = g_vp[b + 4];
            mrate = g_vp[b + 5];
            mrel = g_vp[b + 6];
            duty = g_vp[b + 7];
            nmix = g_vp[b + 8];
            lvl = g_vp[b + 9];
            outv = 0;
            if (stg == 0) {
                env = env + atk;
                if (env >= 65535) {
                    env = 65535;
                    stg = 1;
                }
            } else if (stg == 1) {
                env = env - dec;
                if (env <= sus) {
                    env = sus;
                    stg = 2;
                }
            } else if (stg == 3) {
                env = env - rel;
                if (env <= 0) {
                    env = 0;
                    g_vs[b + 0] = 0;
                }
            }
            if (stg != 3) {
                gate = gate - 1;
                if (gate <= 0) {
                    stg = 3;
                }
            }
            if (menv > 12000) {
                if (stg == 3) {
                    menv = menv - mrel;
                } else {
                    menv = menv - mrate;
                }
            }
            if (g_vs[b + 0] != 0) {
                if (typ == 0) {
                    int32 mod;
                    int32 depth;
                    int32 off;
                    int32 car;
                    int32 opm;
                    opm = g_vp[b + 10];
                    depth = (dep * menv) >> 16;
                    if (g_pegs[b + 14] == 0) {
                        /* 全 op サイン: 従来の sine_at 経路 */
                        if (opm == 1) {
                            int32 p1;
                            int32 p2;
                            int32 o1;
                            int32 o2;
                            p1 = (g_vs[b + 15] + inc_m) & 65535;
                            g_vs[b + 15] = p1;
                            o1 = (sine_at(p1) *
                                  (depth >> 2)) >> 15;
                            p2 = (g_vs[b + 14] + inc_m + o1) & 65535;
                            g_vs[b + 14] = p2;
                            o2 = (sine_at(p2) *
                                  (depth >> 1)) >> 15;
                            pm = (pm + inc_m + o2) & 65535;
                        } else {
                            pm = (pm + inc_m) & 65535;
                        }
                        mod = sine_at(pm);
                        off = (mod * depth) >> 15;
                        pc = (pc + inc_c) & 65535;
                        car = sine_at((pc + off) & 65535);
                        outv = (car * env) >> 16;
                        if (opm == 2) {
                            int32 pcb;
                            int32 pmb;
                            int32 offb;
                            int32 carb;
                            pmb = (g_vs[b + 15] + inc_m) & 65535;
                            g_vs[b + 15] = pmb;
                            offb = (sine_at(pmb) *
                                    (depth >> 1)) >> 15;
                            pcb = (g_vs[b + 14] + inc_c +
                                   (inc_c >> 8)) & 65535;
                            g_vs[b + 14] = pcb;
                            carb = sine_at((pcb + offb) & 65535);
                            outv = (outv >> 1) +
                                   ((carb * env) >> 17);
                        }
                    } else {
                        if (opm == 1) {
                            int32 p1;
                            int32 p2;
                            int32 o1;
                            int32 o2;
                            p1 = (g_vs[b + 15] + inc_m) & 65535;
                            g_vs[b + 15] = p1;
                            o1 = (wave_at(g_vp[b + 11], p1) *
                                  (depth >> 2)) >> 15;
                            p2 = (g_vs[b + 14] + inc_m + o1) & 65535;
                            g_vs[b + 14] = p2;
                            o2 = (wave_at(g_vp[b + 12], p2) *
                                  (depth >> 1)) >> 15;
                            pm = (pm + inc_m + o2) & 65535;
                        } else {
                            pm = (pm + inc_m) & 65535;
                        }
                        mod = wave_at(g_vp[b + 13], pm);
                        off = (mod * depth) >> 15;
                        pc = (pc + inc_c) & 65535;
                        car = wave_at(g_vp[b + 14],
                                      (pc + off) & 65535);
                        outv = (car * env) >> 16;
                        if (opm == 2) {
                            int32 pcb;
                            int32 pmb;
                            int32 offb;
                            int32 carb;
                            pmb = (g_vs[b + 15] + inc_m) & 65535;
                            g_vs[b + 15] = pmb;
                            offb = (wave_at(g_vp[b + 11], pmb) *
                                    (depth >> 1)) >> 15;
                            pcb = (g_vs[b + 14] + inc_c +
                                   (inc_c >> 8)) & 65535;
                            g_vs[b + 14] = pcb;
                            carb = wave_at(g_vp[b + 12],
                                           (pcb + offb) & 65535);
                            outv = (outv >> 1) +
                                   ((carb * env) >> 17);
                        }
                    }
                    sum = sum + (outv >> lvl);
                } else if (typ == 1) {
                    int32 sq;
                    pc = (pc + inc_c) & 65535;
                    if (pc < duty) {
                        sq = 32767;
                    } else {
                        sq = 0 - 32767;
                    }
                    outv = (sq * env) >> 16;
                    if (nmix > 0) {
                        int32 bit2;
                        int32 nv2;
                        bit2 = (lfsr ^ (lfsr >> 2) ^ (lfsr >> 3) ^
                                (lfsr >> 5)) & 1;
                        lfsr = ((lfsr >> 1) | (bit2 << 15)) & 65535;
                        if ((lfsr & 1) != 0) {
                            nv2 = 32767;
                        } else {
                            nv2 = 0 - 32767;
                        }
                        outv = outv + (((nv2 * env) >> 16) * nmix >> 8);
                    }
                    sum = sum + (outv >> lvl);
                } else {
                    int32 bit;
                    int32 nv;
                    bit = (lfsr ^ (lfsr >> 2) ^ (lfsr >> 3) ^
                           (lfsr >> 5)) & 1;
                    lfsr = ((lfsr >> 1) | (bit << 15)) & 65535;
                    if ((lfsr & 1) != 0) {
                        nv = 32767;
                    } else {
                        nv = 0 - 32767;
                    }
                    outv = (nv * env) >> 16;
                    sum = sum + (outv >> (lvl + 1));
                }
            }
            g_vs[b + 3] = pc;
            g_vs[b + 4] = pm;
            g_vs[b + 5] = env;
            g_vs[b + 6] = stg;
            g_vs[b + 7] = gate;
            g_vs[b + 8] = menv;
            g_vs[b + 9] = lfsr;
        }
        vi = vi + 1;
    }
    if (sum > 30000) {
        sum = 30000;
    }
    if (sum < 0 - 30000) {
        sum = 0 - 30000;
    }
    return sum;
}

/* 鍵盤の文字コード -> 半音 (0..24)。無いキーは -1。
 * 下段 z s x d c v g b h n j m , / 上段 q 2 w 3 e r 5 t 6 y 7 u i */
int32 key_semi(int32 c)
{
    if (c == 122) { return 0; }
    if (c == 115) { return 1; }
    if (c == 120) { return 2; }
    if (c == 100) { return 3; }
    if (c == 99) { return 4; }
    if (c == 118) { return 5; }
    if (c == 103) { return 6; }
    if (c == 98) { return 7; }
    if (c == 104) { return 8; }
    if (c == 110) { return 9; }
    if (c == 106) { return 10; }
    if (c == 109) { return 11; }
    if (c == 44) { return 12; }
    if (c == 113) { return 12; }
    if (c == 50) { return 13; }
    if (c == 119) { return 14; }
    if (c == 51) { return 15; }
    if (c == 101) { return 16; }
    if (c == 114) { return 17; }
    if (c == 53) { return 18; }
    if (c == 116) { return 19; }
    if (c == 54) { return 20; }
    if (c == 121) { return 21; }
    if (c == 55) { return 22; }
    if (c == 117) { return 23; }
    if (c == 105) { return 24; }
    return 0 - 1;
}

char g_buf[2048];   /* CHUNK * 2 バイト (サンプルは LE で詰める) */

/* MIDI ストリーム解釈の状態 (run_midi 用、16 ワード):
 *   +0 ランニングステータス  +1 必要データ数  +2 受信済み数
 *   +3 data1  +4 SysEx の中か  +5 SysEx の照合段
 *   +6 システムコモンの残りデータ数  +7 プログラム番号
 *   +8 終了マーカーを見た旗  +9 現在の音色 (0=FM 1=PSG)
 *   +10 ボイス奪い取りのラウンドロビン */
int32 g_mst[16];
char g_mbuf[512];   /* ストリームの読みバッファ */

/* プリセットを現在の音色にする。MIDI モードのノートオンが
 * 波形を読む g_mst +9 も一緒に更新する */
void load_preset(int32 eng, int32 idx)
{
    g_cur[0] = eng;
    g_cur[1] = idx;
    if (eng == 0) {
        g_mst[9] = g_pfm[idx * 20] & 255;
    } else {
        g_mst[9] = g_pps[idx * 20] & 255;
    }
}

/* audio_write の部分受け付けに残りを再送する。失敗は -1 */
int32 audio_write_all(int32 n)
{
    int32 off;
    off = 0;
    while (off < n) {
        int64 w;
        w = syscall3(SYS_AUDIO_WRITE, g_buf + off * 2, n - off, 0);
        if (w <= 0) {
            return -1;
        }
        off = off + (int32)w;
    }
    return 0;
}

/* FNV-1a 64bit の初期値を 16 ビットずつ組む (wavgen の作法):
 * basis 0xCBF29CE484222325。prime は呼び出し側で組む */
int64 fnv_init()
{
    int64 v;
    v = 52210;
    v = (v << 16) | 40164;
    v = (v << 16) | 33826;
    v = (v << 16) | 8997;
    return v;
}

/* ============================================================
 * エフェクトラック (マスターのセンド/リターン)
 *
 * エフェクトは共通の枠組み (モジュール登録表) で管理する。
 * 各モジュールは「名前・センド CC・既定 ON/OFF・既定センド・
 * wet トリム・1 フレーム処理・自分の状態置き場」を持ち、
 * ラック側 (fx_frame) は登録表を回してセンドバスの受け渡しと
 * 処理呼び出しをする。新エフェクトの追加は「処理関数を 1 つ
 * 書いて fx_init の登録表に 1 行足し、fx_proc の振り分けに
 * 1 行足す」で済む (doc/synth-design.md のエフェクトラック
 * の節に手順)。
 *
 * 登録表の並び順がそのままチェーンモードの直列順になる:
 *   0 Chorus (CC93) -> 1 Delay (CC94) -> 2 Reverb (CC91)
 * パラレルモード (既定) では各モジュールが自分のセンドバス
 * だけを受けて wet をマスターへ直接返す。チェーンモード
 * では信号が表の順に直列を通る: 各段の入力は「前段の出力 +
 * その段へのセンドバス」で、戻りは最終段の出力だけになる
 * (センドは各段への注入量として働く。割り切りは doc 参照)。
 *
 * 処理関数は入力フレーム (g_fxin) とパラメータと自分の状態
 * だけで動き、synth 本体の状態 (ボイス等) には触れない。
 * 将来ミキサー側へ同じモジュールを持っていくための形。
 *
 * 状態置き場はファイルスコープの int32 配列 (直接アクセス)。
 * 遅延ラインは 1 サンプル = 1 ワードで持ち、値は int16
 * 相当の範囲にクランプしてから書く。
 * ============================================================ */
#define FX_N 3
#define FX_CHO 0
#define FX_DLY 1
#define FX_RVB 2

char g_fxnm[48];    /* モジュール名 16 バイト x 3 */
int32 g_fxmeta[24];  /* 登録表 3 モジュール x 8 ワード:
                       +0 センド CC +1 既定 ON +2 既定センド
                       +3 wet トリム (256 = 1.0) */
int32 g_fxmod[12];   /* モジュール実行時状態 3 x 4 ワード:
                       +0 ON/OFF */
int32 g_fxctl[16];   /* ラック全体 16 ワード:
                       +0 どれか ON か +1 チェーンモード
                       +2 個別指定を見た +3 fxdbg
                       +4..+6 モノラルセンド (モジュール順)
                       +7 コーラス LFO 位相 (32bit 蓄積) */
int32 g_fxin[8];    /* 処理への入力フレーム (0=L 1=R) */
int32 g_fxout[8];   /* 処理からの出力フレーム (同上) */
int32 g_fxbus[24];   /* モジュールごとのセンドバス 3 x 2 ワード */
int32 g_fxwet[8];   /* ラックの wet リターン (0=L 1=R) */
int32 g_psum[16];    /* MT のパート別ドライ和 8 x 2 ワード */
int32 g_cho[4096];  /* コーラス遅延 2ch x 2048 サンプル */
int32 g_dly[32768]; /* ディレイ遅延 2ch x 16384 サンプル */
int32 g_rvb[25450]; /* リバーブ遅延 (配置は fx_init) */
int32 g_fxoff[32];  /* リバーブ各ラインの先頭位置 32 ワード
                       (ch*12 + line、line = comb 8 + allpass 4) */
int32 g_fxlen[32];  /* リバーブ各ラインの長さ (同上) */
int32 g_fxpos[32];   /* 書き位置 int16 x 32: リバーブ 0..23、
                       コーラス 24/25 (L/R)、ディレイ 26/27 */
int32 g_fxdamp[32];  /* リバーブ comb のダンピング状態
                       int16 x 16 (ch*8 + comb) */


int32 fx_clip(int32 v)
{
    if (v > 30000) {
        return 30000;
    }
    if (v < 0 - 30000) {
        return 0 - 30000;
    }
    return v;
}

/* Freeverb 系の遅延長 (44100Hz)。右 ch は +23 サンプルずらす */
int32 fx_comb_len(int32 i)
{
    if (i == 0) { return 1116; }
    if (i == 1) { return 1188; }
    if (i == 2) { return 1277; }
    if (i == 3) { return 1356; }
    if (i == 4) { return 1422; }
    if (i == 5) { return 1491; }
    if (i == 6) { return 1557; }
    return 1617;
}

int32 fx_ap_len(int32 i)
{
    if (i == 0) { return 556; }
    if (i == 1) { return 441; }
    if (i == 2) { return 341; }
    return 225;
}

void fx_refresh()
{
    int32 m;
    int32 any;
    any = 0;
    m = 0;
    while (m < FX_N) {
        if (g_fxmod[m * 4] != 0) {
            any = 1;
        }
        m = m + 1;
    }
    g_fxctl[0] = any;
}

/* 登録表の初期化と状態クリア。モジュールを足すときは
 * ここの表に 1 行 (名前・CC・既定 ON・既定センド・トリム)
 * を足す */
void fx_init()
{
    int32 m;
    int32 i;
    int32 off;
    int32 ch;
    set_name(g_fxnm, 0, "Chorus");
    set_name(g_fxnm, 1, "Delay");
    set_name(g_fxnm, 2, "Reverb");
    g_fxmeta[0 * 8 + 0] = 93;   /* Chorus: CC93 */
    g_fxmeta[0 * 8 + 1] = 0;
    g_fxmeta[0 * 8 + 2] = 32;
    g_fxmeta[0 * 8 + 3] = 256;
    g_fxmeta[1 * 8 + 0] = 94;   /* Delay: CC94 */
    g_fxmeta[1 * 8 + 1] = 0;
    g_fxmeta[1 * 8 + 2] = 32;
    g_fxmeta[1 * 8 + 3] = 256;
    g_fxmeta[2 * 8 + 0] = 91;   /* Reverb: CC91 */
    g_fxmeta[2 * 8 + 1] = 1;
    g_fxmeta[2 * 8 + 2] = 40;
    g_fxmeta[2 * 8 + 3] = 192;
    m = 0;
    while (m < FX_N) {
        g_fxmod[m * 4 + 0] = g_fxmeta[m * 8 + 1];
        g_fxctl[4 + m] = g_fxmeta[m * 8 + 2];
        m = m + 1;
    }
    g_fxctl[1] = 0;
    g_fxctl[2] = 0;
    g_fxctl[3] = 0;
    g_fxctl[7] = 0;
    off = 0;
    ch = 0;
    while (ch < 2) {
        i = 0;
        while (i < 8) {
            int32 ln;
            ln = fx_comb_len(i);
            if (ch == 1) {
                ln = ln + 23;
            }
            g_fxoff[ch * 12 + i] = off;
            g_fxlen[ch * 12 + i] = ln;
            off = off + ln;
            i = i + 1;
        }
        i = 0;
        while (i < 4) {
            int32 ln2;
            ln2 = fx_ap_len(i);
            if (ch == 1) {
                ln2 = ln2 + 23;
            }
            g_fxoff[ch * 12 + 8 + i] = off;
            g_fxlen[ch * 12 + 8 + i] = ln2;
            off = off + ln2;
            i = i + 1;
        }
        ch = ch + 1;
    }
    i = 0;
    while (i < 4096) {
        g_cho[i] = 0;
        i = i + 1;
    }
    i = 0;
    while (i < 32768) {
        g_dly[i] = 0;
        i = i + 1;
    }
    i = 0;
    while (i < 25450) {
        g_rvb[i] = 0;
        i = i + 1;
    }
    i = 0;
    while (i < 32) {
        g_fxpos[i] = 0;
        g_fxdamp[i] = 0;
        i = i + 1;
    }
    fx_refresh();
}

void fx_toggle(int32 mod)
{
    if (g_fxmod[mod * 4] != 0) {
        g_fxmod[mod * 4] = 0;
    } else {
        g_fxmod[mod * 4] = 1;
    }
    fx_refresh();
}

void fx_print_state()
{
    int32 m;
    int32 i;
    out_str("synth: fx");
    m = 0;
    while (m < FX_N) {
        putc(32);
        i = 0;
        while (i < 16 && g_fxnm[m * 16 + i] != 0) {
            putc(g_fxnm[m * 16 + i]);
            i = i + 1;
        }
        putc(61);
        if (g_fxmod[m * 4] != 0) {
            out_str("on");
        } else {
            out_str("off");
        }
        m = m + 1;
    }
    out_str(" route=");
    if (g_fxctl[1] != 0) {
        out_str("chain");
    } else {
        out_str("parallel");
    }
    putc(10);
}

/* 起動引数のエフェクト指定。消費したら 1 を返す。
 * nofx = 全 OFF、rev / dly / cho = 挙げたものだけ ON
 * (最初に 1 つ挙がった時点で全 OFF から始める)、
 * fxchain = チェーンモード、fxdbg = チャンクエネルギー印字 */
int32 fx_arg(char *s)
{
    int32 m;
    int32 k;
    if (streq(s, "nofx")) {
        m = 0;
        while (m < FX_N) {
            g_fxmod[m * 4] = 0;
            m = m + 1;
        }
        g_fxctl[2] = 1;
        fx_refresh();
        return 1;
    }
    if (streq(s, "fxchain")) {
        g_fxctl[1] = 1;
        return 1;
    }
    if (streq(s, "fxdbg")) {
        g_fxctl[3] = 1;
        return 1;
    }
    m = 0 - 1;
    if (streq(s, "cho")) {
        m = FX_CHO;
    }
    if (streq(s, "dly")) {
        m = FX_DLY;
    }
    if (streq(s, "rev")) {
        m = FX_RVB;
    }
    if (m >= 0) {
        if (g_fxctl[2] == 0) {
            k = 0;
            while (k < FX_N) {
                g_fxmod[k * 4] = 0;
                k = k + 1;
            }
            g_fxctl[2] = 1;
        }
        g_fxmod[m * 4] = 1;
        fx_refresh();
        return 1;
    }
    return 0;
}

/* コーラス: 基準 20ms (882 サンプル) の遅延を LFO 0.6Hz で
 * ±264 サンプル (約 ±6ms) 変調する変調ディレイ。読みは
 * 隣接 2 サンプルの平均 (安い補間)。wet は遅延信号そのもの
 * で、ドライとの干渉がコーラス感になる */
void fx_proc_cho(int32 nch)
{
    int32 ch;
    int32 ph;
    int32 dly;
    ph = g_fxctl[7];
    g_fxctl[7] = ph + 58426;
    dly = 882 + ((sine_at((ph >> 16) & 65535) * 264) >> 15);
    ch = 0;
    while (ch < nch) {
        int32 wp;
        int32 rp;
        int32 s0;
        int32 s1;
        wp = g_fxpos[24 + ch];
        g_cho[ch * 2048 + wp] = g_fxin[ch];
        rp = wp - dly;
        while (rp < 0) {
            rp = rp + 2048;
        }
        s0 = g_cho[ch * 2048 + rp];
        rp = rp + 1;
        if (rp >= 2048) {
            rp = rp - 2048;
        }
        s1 = g_cho[ch * 2048 + rp];
        g_fxout[ch] = (s0 + s1) >> 1;
        wp = wp + 1;
        if (wp >= 2048) {
            wp = 0;
        }
        g_fxpos[24 + ch] = wp;
        ch = ch + 1;
    }
}

/* ディレイ: 13230 サンプル (300ms) のフィードバック
 * ディレイ。帰還率は 11468/32768 ≒ 0.35 */
void fx_proc_dly(int32 nch)
{
    int32 ch;
    ch = 0;
    while (ch < nch) {
        int32 wp;
        int32 rp;
        int32 d;
        wp = g_fxpos[26 + ch];
        rp = wp - 13230;
        while (rp < 0) {
            rp = rp + 16384;
        }
        d = g_dly[ch * 16384 + rp];
        g_dly[ch * 16384 + wp] = g_fxin[ch] + ((d * 11468) >> 15);
        g_fxout[ch] = d;
        wp = wp + 1;
        if (wp >= 16384) {
            wp = 0;
        }
        g_fxpos[26 + ch] = wp;
        ch = ch + 1;
    }
}

/* リバーブ: Freeverb 系。ダンピング付き (帰還路の 1 次
 * ローパス) コムフィルタ 8 本の並列 → オールパス 4 段直列。
 * 入力ゲイン 1/4、コム帰還は 26214/32768 ≒ 0.80 で、
 * ダンピングと合わせて発散しない */
void fx_proc_rvb(int32 nch)
{
    int32 ch;
    ch = 0;
    while (ch < nch) {
        int32 inp;
        int32 acc;
        int32 i;
        inp = g_fxin[ch] >> 2;
        acc = 0;
        i = 0;
        while (i < 8) {
            int32 li;
            int32 pos;
            int32 y;
            int32 filt;
            li = ch * 12 + i;
            pos = g_fxpos[li];
            y = g_rvb[g_fxoff[li] + pos];
            filt = g_fxdamp[ch * 8 + i];
            filt = filt + ((y - filt) >> 2);
            g_fxdamp[ch * 8 + i] = filt;
            g_rvb[g_fxoff[li] + pos] = inp + ((filt * 26214) >> 15);
            acc = acc + y;
            pos = pos + 1;
            if (pos >= g_fxlen[li]) {
                pos = 0;
            }
            g_fxpos[li] = pos;
            i = i + 1;
        }
        acc = acc >> 3;
        i = 0;
        while (i < 4) {
            int32 li2;
            int32 pos2;
            int32 buf;
            int32 outv;
            li2 = ch * 12 + 8 + i;
            pos2 = g_fxpos[li2];
            buf = g_rvb[g_fxoff[li2] + pos2];
            outv = buf - acc;
            g_rvb[g_fxoff[li2] + pos2] = acc + (outv >> 1);
            acc = outv;
            pos2 = pos2 + 1;
            if (pos2 >= g_fxlen[li2]) {
                pos2 = 0;
            }
            g_fxpos[li2] = pos2;
            i = i + 1;
        }
        g_fxout[ch] = acc;
        ch = ch + 1;
    }
}

/* モジュール処理の振り分け + wet トリム。モジュールを
 * 足すときはここに 1 行足す */
void fx_proc(int32 mod, int32 nch)
{
    int32 trim;
    int32 ch;
    if (mod == FX_CHO) {
        fx_proc_cho(nch);
    } else if (mod == FX_DLY) {
        fx_proc_dly(nch);
    } else {
        fx_proc_rvb(nch);
    }
    trim = g_fxmeta[mod * 8 + 3];
    if (trim != 256) {
        ch = 0;
        while (ch < nch) {
            g_fxout[ch] = (g_fxout[ch] * trim) >> 8;
            ch = ch + 1;
        }
    }
    if (nch == 1) {
        g_fxout[1] = 0;
    }
}

/* ラックの 1 フレーム処理。g_fxbus (モジュール x L/R) を
 * 読んで wet を g_fxwet に置く。ルーティング:
 *   パラレル: 各モジュールが自分のバスだけを受け、wet の
 *             和が戻りになる
 *   チェーン: 表の順に直列。各段の入力は前段の出力 + その
 *             段へのバスで、最終段の出力だけが戻りになる。
 *             OFF の段は素通りする */
void fx_frame(int32 nch)
{
    int32 mod;
    int32 ch;
    g_fxwet[0] = 0;
    g_fxwet[1] = 0;
    if (g_fxctl[0] == 0) {
        return;
    }
    if (g_fxctl[1] != 0) {
        int32 curL;
        int32 curR;
        curL = 0;
        curR = 0;
        mod = 0;
        while (mod < FX_N) {
            if (g_fxmod[mod * 4] != 0) {
                g_fxin[0] = fx_clip(curL + g_fxbus[mod * 2]);
                g_fxin[1] = fx_clip(curR + g_fxbus[mod * 2 + 1]);
                fx_proc(mod, nch);
                curL = g_fxout[0];
                curR = g_fxout[1];
            }
            mod = mod + 1;
        }
        g_fxwet[0] = curL;
        g_fxwet[1] = curR;
        return;
    }
    mod = 0;
    while (mod < FX_N) {
        if (g_fxmod[mod * 4] != 0) {
            g_fxin[0] = g_fxbus[mod * 2];
            g_fxin[1] = g_fxbus[mod * 2 + 1];
            fx_proc(mod, nch);
            ch = 0;
            while (ch < nch) {
                g_fxwet[ch] = g_fxwet[ch] + g_fxout[ch];
                ch = ch + 1;
            }
        }
        mod = mod + 1;
    }
}

/* モノラルモード用: ドライ 1 サンプルにラックを通した
 * 出力を返す。バスはモノラルセンド (g_fxctl +4..) で作る */
int32 fx_mono(int32 dry)
{
    int32 mod;
    mod = 0;
    while (mod < FX_N) {
        g_fxbus[mod * 2] = (dry * g_fxctl[4 + mod]) / 127;
        g_fxbus[mod * 2 + 1] = 0;
        mod = mod + 1;
    }
    fx_frame(1);
    return fx_clip(dry + g_fxwet[0]);
}

int32 run_demo(int32 engine)
{
    int64 fnv;
    int64 fnv_prime;
    int32 total;
    int32 rr;
    int32 step;
    int64 r;
    fnv = fnv_init();
    fnv_prime = ((int64)256 << 32) + 435;
    total = 0;
    rr = 0;
    r = syscall3(SYS_AUDIO_OPEN, RATE, 1, 0);
    if (r != 0) {
        out_str("synth: cannot open audio\n");
        return 1;
    }
    if (engine == 0) {
        out_str("synth: fm demo\n");
    } else {
        out_str("synth: psg demo\n");
    }
    print_preset(engine, g_cur[1]);
    fx_print_state();
    step = 0;
    while (step < NSTEPS) {
        int32 remain;
        if (g_mel[step] != 0) {
            trig(rr, g_mel[step] & 255, STEP_LEN, 0 - 1);
            rr = (rr + 1) & 3;
        }
        if ((step & 1) == 0 && g_bas[step] != 0) {
            trig(rr, g_bas[step] & 255, STEP_LEN * 2, 0 - 1);
            rr = (rr + 1) & 3;
        }
        if (engine == 1 && (step & 3) == 2) {
            trig(rr, 60, 1102, 2);
            rr = (rr + 1) & 3;
        }
        remain = STEP_LEN;
        while (remain > 0) {
            int32 n;
            int32 k;
            n = remain;
            if (n > CHUNK) {
                n = CHUNK;
            }
            k = 0;
            while (k < n) {
                int32 smp;
                smp = mix_sample();
                if (g_fxctl[0] != 0) {
                    smp = fx_mono(smp);
                }
                g_buf[k * 2] = (char)(smp & 255);
                g_buf[k * 2 + 1] = (char)((smp >> 8) & 255);
                fnv = (fnv ^ (g_buf[k * 2] & 255)) * fnv_prime;
                fnv = (fnv ^ (g_buf[k * 2 + 1] & 255)) * fnv_prime;
                k = k + 1;
            }
            if (audio_write_all(n) != 0) {
                syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
                out_str("synth: audio write failed\n");
                return 1;
            }
            total = total + n;
            remain = remain - n;
        }
        step = step + 1;
    }
    r = syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
    if (r != 0) {
        out_str("synth: audio close failed\n");
        return 1;
    }
    out_str("synth: samples=");
    out_num(total);
    out_str(" fnv=");
    out_hex64(fnv);
    putc(10);
    out_str("synth: done\n");
    return 0;
}

int32 run_play(int32 engine)
{
    int64 fnv;
    int64 fnv_prime;
    int32 total;
    int32 rr;
    int32 base;
    int32 done;
    int64 r;
    fnv = fnv_init();
    fnv_prime = ((int64)256 << 32) + 435;
    total = 0;
    rr = 0;
    base = 60;
    done = 0;
    r = syscall3(SYS_AUDIO_OPEN, RATE, 1, 0);
    if (r != 0) {
        out_str("synth: cannot open audio\n");
        return 1;
    }
    out_str("synth: play mode. keys=z..m/q..i, o/p=octave, [/]=preset, R/D/C=fx toggle, X=chain, ESC=end\n");
    print_preset(engine, g_cur[1]);
    fx_print_state();
    while (done == 0) {
        int64 c;
        int32 k;
        c = syscall3(SYS_GETC, 0, 5, 0);
        if (c == 27 || c == 3) {
            done = 1;
        } else if (c == 111) {
            if (base > 36) {
                base = base - 12;
            }
        } else if (c == 112) {
            if (base < 84) {
                base = base + 12;
            }
        } else if (c == 91) {
            int32 np;
            np = 16;
            if (engine == 0) {
                np = 22;
            }
            load_preset(engine, (g_cur[1] + np - 1) % np);
            print_preset(engine, g_cur[1]);
        } else if (c == 93) {
            int32 np;
            np = 16;
            if (engine == 0) {
                np = 22;
            }
            load_preset(engine, (g_cur[1] + 1) % np);
            print_preset(engine, g_cur[1]);
        } else if (c == 82) {
            fx_toggle(FX_RVB);
            fx_print_state();
        } else if (c == 68) {
            fx_toggle(FX_DLY);
            fx_print_state();
        } else if (c == 67) {
            fx_toggle(FX_CHO);
            fx_print_state();
        } else if (c == 88) {
            if (g_fxctl[1] != 0) {
                g_fxctl[1] = 0;
            } else {
                g_fxctl[1] = 1;
            }
            fx_print_state();
        } else if (c > 0) {
            int32 semi;
            semi = key_semi((int32)c);
            if (semi >= 0) {
                trig(rr, base + semi, 16000, 0 - 1);
                rr = (rr + 1) & 3;
            }
        }
        k = 0;
        while (k < 512) {
            int32 smp;
            smp = mix_sample();
            if (g_fxctl[0] != 0) {
                smp = fx_mono(smp);
            }
            g_buf[k * 2] = (char)(smp & 255);
            g_buf[k * 2 + 1] = (char)((smp >> 8) & 255);
            fnv = (fnv ^ (g_buf[k * 2] & 255)) * fnv_prime;
            fnv = (fnv ^ (g_buf[k * 2 + 1] & 255)) * fnv_prime;
            k = k + 1;
        }
        if (audio_write_all(512) != 0) {
            syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
            out_str("synth: audio write failed\n");
            return 1;
        }
        total = total + 512;
    }
    r = syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
    if (r != 0) {
        out_str("synth: audio close failed\n");
        return 1;
    }
    out_str("synth: samples=");
    out_num(total);
    out_str(" fnv=");
    out_hex64(fnv);
    putc(10);
    out_str("synth: done\n");
    return 0;
}

/* --- MIDI ストリーム解釈 (run_midi の部品) ----------------
 * ノートオンは gate を実質無限にして本当のホールドにし、
 * ノートオフで release 段 (stage 3) へ落とす (対話モードの
 * gate 方式とは違う、本物のゲート)。ベロシティの大きさは
 * 使わない (0 = オフの扱いだけ守る) */

void midi_note_off(int32 note)
{
    int32 vi;
    vi = 0;
    while (vi < 4) {
        int32 b;
        b = vi * 16;
        if (g_vs[b + 0] != 0 && g_vs[b + 2] == note) {
            g_vs[b + 6] = 3;
            return;
        }
        vi = vi + 1;
    }
}

void midi_note_on(int32 note, int32 vel)
{
    int32 vi;
    int32 rr;
    if (vel == 0) {
        midi_note_off(note);
        return;
    }
    vi = 0;
    while (vi < 4) {
        int32 b;
        b = vi * 16;
        if (g_vs[b + 0] != 0 && g_vs[b + 2] == note) {
            trig(vi, note, 2000000000, g_mst[9]);
            return;
        }
        vi = vi + 1;
    }
    vi = 0;
    while (vi < 4) {
        if (g_vs[vi * 16] == 0) {
            trig(vi, note, 2000000000, g_mst[9]);
            return;
        }
        vi = vi + 1;
    }
    rr = g_mst[10];
    trig(rr, note, 2000000000, g_mst[9]);
    g_mst[10] = (rr + 1) & 3;
}

void midi_all_off()
{
    int32 vi;
    vi = 0;
    while (vi < 4) {
        if (g_vs[vi * 16] != 0) {
            g_vs[vi * 16 + 6] = 3;
        }
        vi = vi + 1;
    }
}

void midi_event(int32 status, int32 d1, int32 d2)
{
    int32 hi;
    hi = status & 240;
    if (hi == 128) {
        midi_note_off(d1);
    } else if (hi == 144) {
        midi_note_on(d1, d2);
    } else if (hi == 176) {
        if (d1 == 120 || d1 == 123) {
            midi_all_off();
        } else if (d1 == 91 || d1 == 93 || d1 == 94) {
            int32 m2;
            m2 = 0;
            while (m2 < FX_N) {
                if (g_fxmeta[m2 * 8 + 0] == d1) {
                    g_fxctl[4 + m2] = d2;
                }
                m2 = m2 + 1;
            }
        }
    } else if (hi == 192) {
        int32 eng;
        int32 idx;
        eng = g_cur[0];
        g_mst[7] = d1;
        if (eng == 0) {
            idx = g_gmf[(d1 >> 3) & 15] & 255;
        } else {
            idx = g_gmp[(d1 >> 3) & 15] & 255;
        }
        load_preset(eng, idx);
        print_preset(eng, idx);
    }
}

/* ストリームの 1 バイトを解釈する。ランニングステータス、
 * SysEx (終了マーカー F0 7D 00 F7 の検出だけし、他は読み
 * 飛ばす)、システムコモン、リアルタイム (無視) を捌く */
void midi_feed(int32 b)
{
    if (g_mst[4] != 0) {
        if (b == 247) {
            if (g_mst[5] == 2) {
                g_mst[8] = 1;  /* 終了マーカー確定 */
            }
            g_mst[4] = 0;
            g_mst[5] = 0;
        } else if (g_mst[5] == 0) {
            if (b == 125) {
                g_mst[5] = 1;
            } else {
                g_mst[5] = 3;
            }
        } else if (g_mst[5] == 1) {
            if (b == 0) {
                g_mst[5] = 2;
            } else {
                g_mst[5] = 3;
            }
        } else {
            g_mst[5] = 3;
        }
        return;
    }
    if (b >= 248) {
        return;  /* リアルタイムメッセージは無視 */
    }
    if (b == 240) {
        g_mst[4] = 1;
        g_mst[5] = 0;
        g_mst[0] = 0;
        return;
    }
    if (b >= 240) {
        g_mst[0] = 0;
        g_mst[2] = 0;
        if (b == 241 || b == 243) {
            g_mst[6] = 1;
        } else if (b == 242) {
            g_mst[6] = 2;
        } else {
            g_mst[6] = 0;
        }
        return;
    }
    if (b >= 128) {
        int32 hi;
        hi = b & 240;
        g_mst[0] = b;
        if (hi == 192 || hi == 208) {
            g_mst[1] = 1;
        } else {
            g_mst[1] = 2;
        }
        g_mst[2] = 0;
        return;
    }
    if (g_mst[6] > 0) {
        g_mst[6] = g_mst[6] - 1;
        return;
    }
    if (g_mst[0] == 0) {
        return;  /* 行き先のないデータバイトは捨てる */
    }
    if (g_mst[2] == 0) {
        g_mst[3] = b;
        g_mst[2] = 1;
        if (g_mst[1] == 1) {
            midi_event(g_mst[0], b, 0);
            g_mst[2] = 0;
        }
    } else {
        midi_event(g_mst[0], g_mst[3], b);
        g_mst[2] = 0;
    }
}

/* ライブループの刻み (2026-11-26、doc の「ライブモードの
 * リアルタイム化」の節):
 *   LIVE_BLOCK  1 回に描くフレーム数 (128 = 約 2.9ms)。
 *               入力ポーリングはブロックごとに行うので、
 *               ノートオンの反映は最大でもこの分遅れる
 *   LIVE_LEAD   演奏時計に対する先行目標 (256 フレーム =
 *               約 5.8ms)。リングを満杯まで溜めず、枯ら
 *               さない最小限だけ先行する (リングの滞留は
 *               そのまま鍵盤→音の遅延になる)
 *   LIVE_RATE   演奏時計のレート。K210 の実効レート
 *               44010 に合わせる。要求レート 44100 で
 *               時計を作ると消費より 0.2% 速く作り続け、
 *               先行が膨らんで数分でリングが満ちる。
 *               将来 OS の段3 (音声時計の公開) が来たら、
 *               時計の出所はこの定数を差し替える */
#define LIVE_BLOCK 128
#define LIVE_LEAD 256
#define LIVE_RATE 44010

/* MIDI ストリーム再生モード: ファイルをバイト列ストリーム
 * としてポーリング読みし、MIDI を逐次解釈して鳴らす。
 * 自分の読み位置 (fd) を持ち、まだ書かれていない分は
 * read が 0 を返す (書き手が smfplay の同時実行ジョブ
 * でも、書き終えた後から読み始めてもよい)。ストリーム
 * 自体に時刻情報は無いので、タイミングは「届いた時刻」
 * が全て (本物の MIDI と同じ)。
 * 描画は演奏時計駆動: 起点の uptime から「その時刻まで
 * に描くべきフレーム数」を毎回計算し、先行目標まで
 * 128 フレーム単位で描く。スケジューラに CPU を奪われ
 * ても、戻った時点で時計まで取り戻すので、遅れは PCM
 * の欠落ではなくジッタになる (旧ループは 512 フレーム
 * 固定で取り戻す項が無く、実時間の約 1/4 しか作れな
 * かった。doc の解析参照)。
 * 終了マーカー F0 7D 00 F7 を読んだら、release の余韻
 * として 11025 フレーム描いてから閉じる。マーカーが
 * 来ないまま 30 秒新着が無ければ、書き手が死んだと
 * みなして同じく閉じる */
int32 run_midi(char *path, int32 engine)
{
    int64 fnv;
    int64 fnv_prime;
    int32 total;
    int64 r;
    int64 fd;
    int32 blen;
    int32 bpos;
    int32 i;
    int64 t0;
    int64 lastdata;
    int64 rendered;
    int64 tailend;
    int64 lastread;
    fnv = fnv_init();
    fnv_prime = ((int64)256 << 32) + 435;
    total = 0;
    blen = 0;
    bpos = 0;
    rendered = 0;
    tailend = -1;
    lastread = -1000000;
    i = 0;
    while (i < 16) {
        g_mst[i] = 0;
        i = i + 1;
    }
    fd = syscall3(SYS_OPEN, path, 0, 0);
    if (fd < 0) {
        out_str("synth: cannot open ");
        out_str(path);
        putc(10);
        return 1;
    }
    r = syscall3(SYS_AUDIO_OPEN, RATE, 1, 0);
    if (r != 0) {
        out_str("synth: cannot open audio\n");
        syscall3(SYS_FCLOSE, fd, 0, 0);
        return 1;
    }
    out_str("synth: midi mode\n");
    fx_print_state();
    load_preset(engine, 0);
    print_preset(engine, 0);
    t0 = syscall3(SYS_UPTIME, 0, 0, 0);
    lastdata = t0;
    while (1) {
        int64 now;
        int64 target;
        int32 nf;
        int32 k;
        now = syscall3(SYS_UPTIME, 0, 0, 0);
        if (tailend >= 0) {
            target = tailend;
        } else {
            target = (now - t0) * LIVE_RATE / 1000 + LIVE_LEAD;
        }
        /* 入力ポーリングは、前回の読み取りから LIVE_BLOCK
         * フレーム描画が進んでからにする。ファイル先行
         * 書式で全バイトが既にある場合に、読み切りで
         * イベントが冒頭へ集中するのを防ぎ、消費を描画
         * ペースに結ぶため。ライブ時はブロックごとに
         * 描画が進むので、ポーリング間隔はブロックの
         * 刻みと変わらない */
        if (tailend < 0 && rendered - lastread >= LIVE_BLOCK) {
            if (bpos >= blen) {
                int64 n;
                n = syscall3(SYS_READ, fd, g_mbuf, 128);
                if (n < 0) {
                    syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
                    syscall3(SYS_FCLOSE, fd, 0, 0);
                    out_str("synth: read failed\n");
                    return 1;
                }
                if (n > 0) {
                    blen = (int32)n;
                    bpos = 0;
                    lastdata = now;
                    lastread = rendered;
                }
            }
            while (bpos < blen) {
                midi_feed(g_mbuf[bpos] & 255);
                bpos = bpos + 1;
                if (g_mst[8] != 0) {
                    tailend = rendered + 11025;
                    target = tailend;
                    break;
                }
            }
            if (tailend < 0 && now - lastdata > 30000) {
                tailend = rendered + 11025;
                target = tailend;
            }
        }
        if (rendered >= target) {
            if (tailend >= 0) {
                break;
            }
            syscall3(SYS_SLEEP, 1, 0, 0);
            continue;
        }
        nf = LIVE_BLOCK;
        if (rendered + nf > target) {
            nf = (int32)(target - rendered);
        }
        k = 0;
        while (k < nf) {
            int32 smp;
            smp = mix_sample();
            if (g_fxctl[0] != 0) {
                smp = fx_mono(smp);
            }
            g_buf[k * 2] = (char)(smp & 255);
            g_buf[k * 2 + 1] = (char)((smp >> 8) & 255);
            fnv = (fnv ^ (g_buf[k * 2] & 255)) * fnv_prime;
            fnv = (fnv ^ (g_buf[k * 2 + 1] & 255)) * fnv_prime;
            k = k + 1;
        }
        if (audio_write_all(nf) != 0) {
            syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
            syscall3(SYS_FCLOSE, fd, 0, 0);
            out_str("synth: audio write failed\n");
            return 1;
        }
        rendered = rendered + nf;
        total = total + nf;
    }
    syscall3(SYS_FCLOSE, fd, 0, 0);
    r = syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
    if (r != 0) {
        out_str("synth: audio close failed\n");
        return 1;
    }
    out_str("synth: samples=");
    out_num(total);
    out_str(" fnv=");
    out_hex64(fnv);
    putc(10);
    out_str("synth: done\n");
    return 0;
}

/* ============================================================
 * マルチティンバー MIDI モード (FM 専用、ステレオ出力)
 *
 * 8 パート (MIDI ch 1..8) が個別にプリセット / ボリューム
 * (CC7) / パン (CC10) を持ち、全パート共有のボイスプール
 * (最大 NVOICE=32、起動引数で上限を絞れる) から dynamic
 * voice allocation で割り当てる。割り当て規則:
 *   同じ (ch, note) が鳴っていれば再トリガ -> 空きボイス
 *   -> release 中の最も古いボイス -> 全体で最も古いボイス
 *   (steal)。パートごとの同時発音上限は PARTMAX=8 で、
 *   上限に達したパートは自パートの最古ボイスを奪う。
 * ch 9 以降は無視する (ch 10 のドラムは将来課題)。
 *
 * 負荷計測: 描画ブロック (128 フレーム) の前後で perf_read
 * (33、0=サイクル / 1=命令数) を読み、総サイクル・平均
 * cycles/frame・最悪チャンク (512 フレーム換算) と実時間
 * 予算の比・同時発音のピークを終了時に印字する。
 * perf_read はマシン全体の通算カウンタなので、サイクル/秒
 * は uptime との差分で較正する。
 * 描画の主ループは演奏時計駆動 (run_midi の上の LIVE_*
 * のコメントと doc の「ライブモードのリアルタイム化」
 * の節)。
 * ============================================================ */
#define NVOICE 32
#define NPART 8
#define PARTMAX 8

int32 g_mv[512];   /* MT ボイス 32 x 16 ワード (g_vs と同じ
                      並び。+12 所属 ch、+13 発音順 seq、
                      +14/+15 は 4op 用の追加位相) */
int32 g_mvp[512];  /* MT ボイスごとの実効パラメータ
                      (g_vp と同じ並び、+10 に op モード) */
int32 g_part[64];  /* パート 8 x 8 ワード:
                      +0 preset +1 vol (CC7) +2 pan (CC10)
                      +3..+5 エフェクトセンド (モジュール順、
                      登録表の CC に対応) */
int32 g_mctl[16];   /* +0 次の seq +1 pool 上限 +2 dbg
                      +3 同時発音ピーク +4 op 上書き
                      (0=プリセット通り 1=2op 固定
                      2=4op スタック固定) */
int32 g_mix[8];    /* mt_mix の出力 (ワード 0=L、1=R) */

int64 perf_read(int32 kind)
{
    return syscall3(SYS_PERF, kind, 0, 0);
}

int32 mt_def_preset(int32 p)
{
    if (p == 0) {
        return 0;
    }
    if (p == 1) {
        return 9;
    }
    if (p == 2) {
        return 10;
    }
    if (p == 3) {
        return 1;
    }
    if (p == 4) {
        return 6;
    }
    if (p == 5) {
        return 11;
    }
    if (p == 6) {
        return 12;
    }
    return 15;
}

void print_part_preset(int32 ch, int32 idx)
{
    int32 i;
    out_str("synth: part ");
    out_num(ch + 1);
    out_str(" preset ");
    out_num(idx);
    putc(32);
    i = 0;
    while (i < 16 && g_fnm[idx * 16 + i] != 0) {
        putc(g_fnm[idx * 16 + i]);
        i = i + 1;
    }
    putc(10);
}

void mt_init(int32 pool, int32 dbg, int32 opovr)
{
    int32 p;
    int32 v;
    p = 0;
    while (p < NPART) {
        int32 m;
        g_part[p * 8 + 0] = mt_def_preset(p);
        g_part[p * 8 + 1] = 127;
        g_part[p * 8 + 2] = 64;
        m = 0;
        while (m < FX_N) {
            g_part[p * 8 + 3 + m] = g_fxmeta[m * 8 + 2];
            m = m + 1;
        }
        p = p + 1;
    }
    v = 0;
    while (v < NVOICE) {
        g_mv[v * 16 + 0] = 0;
        v = v + 1;
    }
    g_mctl[0] = 0;
    g_mctl[1] = pool;
    g_mctl[2] = dbg;
    g_mctl[3] = 0;
    g_mctl[4] = opovr;
}

/* パート ch のプリセットでボイス vi を発音開始する */
void mt_trig(int32 vi, int32 ch, int32 note)
{
    int32 b;
    int32 o;
    int32 inc;
    int32 opm;
    b = vi * 16;
    o = g_part[ch * 8 + 0] * 20;
    g_mvp[b + 0] = rd16u(g_pfm, o + 2);
    g_mvp[b + 1] = rd16u(g_pfm, o + 4);
    g_mvp[b + 2] = rd16u(g_pfm, o + 6);
    g_mvp[b + 3] = rd16u(g_pfm, o + 8);
    g_mvp[b + 4] = rd16u(g_pfm, o + 12);
    g_mvp[b + 5] = rd16u(g_pfm, o + 14);
    g_mvp[b + 6] = rd16u(g_pfm, o + 16);
    g_mvp[b + 7] = 32768;
    g_mvp[b + 8] = 0;
    g_mvp[b + 9] = g_pfm[o + 1] & 255;
    opm = g_pfm[o + 18] & 255;
    if (g_mctl[4] == 1) {
        opm = 0;
    } else if (g_mctl[4] == 2) {
        opm = 1;
    }
    g_mvp[b + 10] = opm;
    g_mvp[b + 11] = (g_wfm[g_part[ch * 8 + 0] * 4 + 0] & 255) * 256;
    g_mvp[b + 12] = (g_wfm[g_part[ch * 8 + 0] * 4 + 1] & 255) * 256;
    g_mvp[b + 13] = (g_wfm[g_part[ch * 8 + 0] * 4 + 2] & 255) * 256;
    g_mvp[b + 14] = (g_wfm[g_part[ch * 8 + 0] * 4 + 3] & 255) * 256;
    if (g_mvp[b + 11] + g_mvp[b + 12] + g_mvp[b + 13] +
        g_mvp[b + 14] != 0) {
        g_mpegs[b + 14] = 1;
    } else {
        g_mpegs[b + 14] = 0;
    }
    if (note >= 36 && note <= 96) {
        inc = g_ninc[note - 36];
    } else {
        inc = 0;
    }
    g_mv[b + 0] = 1;
    g_mv[b + 1] = 0;
    g_mv[b + 2] = note;
    g_mv[b + 3] = 0;
    g_mv[b + 4] = 0;
    g_mv[b + 5] = 0;
    g_mv[b + 6] = 0;
    g_mv[b + 7] = 2000000000;
    g_mv[b + 8] = 65535;
    g_mv[b + 9] = 257;
    g_mv[b + 10] = inc;
    g_mv[b + 11] = (inc * (g_pfm[o + 10] & 255)) / 2;
    g_mv[b + 12] = ch;
    g_mv[b + 13] = g_mctl[0];
    g_mv[b + 14] = 0;
    g_mv[b + 15] = 0;
    g_mctl[0] = g_mctl[0] + 1;
    peg_init(g_mpegs, b, g_part[ch * 8 + 0], g_mv, b);
}

void mt_note_on(int32 ch, int32 note)
{
    int32 pool;
    int32 v;
    int32 pick;
    int32 cnt;
    int32 steal;
    pool = g_mctl[1];
    /* 同じ (ch, note) が鳴っていればそれを再トリガ */
    v = 0;
    while (v < pool) {
        if (g_mv[v * 16 + 0] != 0 &&
            g_mv[v * 16 + 12] == ch &&
            g_mv[v * 16 + 2] == note) {
            mt_trig(v, ch, note);
            return;
        }
        v = v + 1;
    }
    /* パートの同時発音数を数える */
    cnt = 0;
    v = 0;
    while (v < pool) {
        if (g_mv[v * 16 + 0] != 0 &&
            g_mv[v * 16 + 12] == ch) {
            cnt = cnt + 1;
        }
        v = v + 1;
    }
    pick = -1;
    if (cnt >= PARTMAX) {
        /* パート上限: 自パートの最も古いボイスを奪う */
        int32 best;
        best = 2000000000;
        v = 0;
        while (v < pool) {
            if (g_mv[v * 16 + 0] != 0 &&
                g_mv[v * 16 + 12] == ch &&
                g_mv[v * 16 + 13] < best) {
                best = g_mv[v * 16 + 13];
                pick = v;
            }
            v = v + 1;
        }
    } else {
        v = 0;
        while (v < pool) {
            if (g_mv[v * 16 + 0] == 0) {
                pick = v;
                break;
            }
            v = v + 1;
        }
        if (pick < 0) {
            /* release 中で最も古いボイスを奪う */
            int32 best;
            best = 2000000000;
            v = 0;
            while (v < pool) {
                if (g_mv[v * 16 + 0] != 0 &&
                    g_mv[v * 16 + 6] == 3 &&
                    g_mv[v * 16 + 13] < best) {
                    best = g_mv[v * 16 + 13];
                    pick = v;
                }
                v = v + 1;
            }
        }
        if (pick < 0) {
            /* 全体で最も古いボイスを奪う (steal) */
            int32 best;
            best = 2000000000;
            v = 0;
            while (v < pool) {
                if (g_mv[v * 16 + 0] != 0 &&
                    g_mv[v * 16 + 13] < best) {
                    best = g_mv[v * 16 + 13];
                    pick = v;
                }
                v = v + 1;
            }
        }
    }
    if (pick < 0) {
        return;
    }
    steal = g_mv[pick * 16 + 0];
    mt_trig(pick, ch, note);
    if (g_mctl[2] != 0) {
        out_str("synth: alloc v=");
        out_num(pick);
        out_str(" ch=");
        out_num(ch + 1);
        out_str(" note=");
        out_num(note);
        if (steal != 0) {
            out_str(" steal");
        }
        putc(10);
    }
}

void mt_note_off(int32 ch, int32 note)
{
    int32 v;
    v = 0;
    while (v < NVOICE) {
        if (g_mv[v * 16 + 0] != 0 &&
            g_mv[v * 16 + 12] == ch &&
            g_mv[v * 16 + 2] == note &&
            g_mv[v * 16 + 6] != 3) {
            g_mv[v * 16 + 6] = 3;
        }
        v = v + 1;
    }
}

void mt_all_off(int32 ch)
{
    int32 v;
    v = 0;
    while (v < NVOICE) {
        if (g_mv[v * 16 + 0] != 0 &&
            g_mv[v * 16 + 12] == ch &&
            g_mv[v * 16 + 6] != 3) {
            g_mv[v * 16 + 6] = 3;
        }
        v = v + 1;
    }
}

void mt_peak_update()
{
    int32 v;
    int32 cnt;
    cnt = 0;
    v = 0;
    while (v < NVOICE) {
        if (g_mv[v * 16 + 0] != 0) {
            cnt = cnt + 1;
        }
        v = v + 1;
    }
    if (cnt > g_mctl[3]) {
        g_mctl[3] = cnt;
    }
}

/* MT 側のピッチ EG ブロック駆動 (プール全ボイス分) */
void peg_block_mt()
{
    int32 vi;
    int32 pool;
    pool = g_mctl[1];
    vi = 0;
    while (vi < pool) {
        if (g_mv[vi * 16] != 0) {
            peg_step(g_mpegs, vi * 16, g_mv, vi * 16);
        }
        vi = vi + 1;
    }
}

/* 1 フレーム分を描いて g_mix (ワード 0=L、1=R) に置く。
 * FM の合成そのものは mix_sample の FM 分岐と同じ計算で、
 * op モード (0=2op / 1=4op スタック / 2=4op ペア) も同じ。
 * 出力はボイスごとにパートの vol / pan を掛けて L/R へ */
void mt_mix()
{
    int32 sumL;
    int32 sumR;
    int32 vi;
    sumL = 0;
    sumR = 0;
    g_pegclk[1] = g_pegclk[1] + 1;
    if (g_pegclk[1] >= 64) {
        g_pegclk[1] = 0;
        peg_block_mt();
    }
    if (g_fxctl[0] != 0) {
        int32 pp;
        pp = 0;
        while (pp < 16) {
            g_psum[pp] = 0;
            pp = pp + 1;
        }
    }
    vi = 0;
    while (vi < NVOICE) {
        int32 b;
        b = vi * 16;
        if (g_mv[b + 0] != 0) {
            int32 pc;
            int32 pm;
            int32 env;
            int32 stg;
            int32 gate;
            int32 menv;
            int32 inc_c;
            int32 inc_m;
            int32 atk;
            int32 dec;
            int32 sus;
            int32 rel;
            int32 dep;
            int32 mrate;
            int32 mrel;
            int32 lvl;
            int32 opm;
            int32 outv;
            pc = g_mv[b + 3];
            pm = g_mv[b + 4];
            env = g_mv[b + 5];
            stg = g_mv[b + 6];
            gate = g_mv[b + 7];
            menv = g_mv[b + 8];
            inc_c = g_mpegs[b + 2];
            inc_m = g_mpegs[b + 3];
            atk = g_mvp[b + 0];
            dec = g_mvp[b + 1];
            sus = g_mvp[b + 2];
            rel = g_mvp[b + 3];
            dep = g_mvp[b + 4];
            mrate = g_mvp[b + 5];
            mrel = g_mvp[b + 6];
            lvl = g_mvp[b + 9];
            opm = g_mvp[b + 10];
            outv = 0;
            if (stg == 0) {
                env = env + atk;
                if (env >= 65535) {
                    env = 65535;
                    stg = 1;
                }
            } else if (stg == 1) {
                env = env - dec;
                if (env <= sus) {
                    env = sus;
                    stg = 2;
                }
            } else if (stg == 3) {
                env = env - rel;
                if (env <= 0) {
                    env = 0;
                    g_mv[b + 0] = 0;
                }
            }
            if (stg != 3) {
                gate = gate - 1;
                if (gate <= 0) {
                    stg = 3;
                }
            }
            if (menv > 12000) {
                if (stg == 3) {
                    menv = menv - mrel;
                } else {
                    menv = menv - mrate;
                }
            }
            if (g_mv[b + 0] != 0) {
                int32 mod;
                int32 depth;
                int32 off;
                int32 car;
                int32 t;
                int32 part;
                int32 vol;
                int32 pan;
                depth = (dep * menv) >> 16;
                if (g_mpegs[b + 14] == 0) {
                    /* 全 op サインのボイスは従来の sine_at
                     * 経路 (コストも音も従来通り) */
                    if (opm == 1) {
                        int32 p1;
                        int32 p2;
                        int32 o1;
                        int32 o2;
                        p1 = (g_mv[b + 15] + inc_m) & 65535;
                        g_mv[b + 15] = p1;
                        o1 = (sine_at(p1) * (depth >> 2)) >> 15;
                        p2 = (g_mv[b + 14] + inc_m + o1) & 65535;
                        g_mv[b + 14] = p2;
                        o2 = (sine_at(p2) * (depth >> 1)) >> 15;
                        pm = (pm + inc_m + o2) & 65535;
                    } else {
                        pm = (pm + inc_m) & 65535;
                    }
                    mod = sine_at(pm);
                    off = (mod * depth) >> 15;
                    pc = (pc + inc_c) & 65535;
                    car = sine_at((pc + off) & 65535);
                    outv = (car * env) >> 16;
                    if (opm == 2) {
                        int32 pcb;
                        int32 pmb;
                        int32 offb;
                        int32 carb;
                        pmb = (g_mv[b + 15] + inc_m) & 65535;
                        g_mv[b + 15] = pmb;
                        offb = (sine_at(pmb) *
                                (depth >> 1)) >> 15;
                        pcb = (g_mv[b + 14] + inc_c +
                               (inc_c >> 8)) & 65535;
                        g_mv[b + 14] = pcb;
                        carb = sine_at((pcb + offb) & 65535);
                        outv = (outv >> 1) +
                               ((carb * env) >> 17);
                    }
                } else {
                    if (opm == 1) {
                        int32 p1;
                        int32 p2;
                        int32 o1;
                        int32 o2;
                        p1 = (g_mv[b + 15] + inc_m) & 65535;
                        g_mv[b + 15] = p1;
                        o1 = (wave_at(g_mvp[b + 11], p1) *
                              (depth >> 2)) >> 15;
                        p2 = (g_mv[b + 14] + inc_m + o1) & 65535;
                        g_mv[b + 14] = p2;
                        o2 = (wave_at(g_mvp[b + 12], p2) *
                              (depth >> 1)) >> 15;
                        pm = (pm + inc_m + o2) & 65535;
                    } else {
                        pm = (pm + inc_m) & 65535;
                    }
                    mod = wave_at(g_mvp[b + 13], pm);
                    off = (mod * depth) >> 15;
                    pc = (pc + inc_c) & 65535;
                    car = wave_at(g_mvp[b + 14],
                                  (pc + off) & 65535);
                    outv = (car * env) >> 16;
                    if (opm == 2) {
                        int32 pcb;
                        int32 pmb;
                        int32 offb;
                        int32 carb;
                        pmb = (g_mv[b + 15] + inc_m) & 65535;
                        g_mv[b + 15] = pmb;
                        offb = (wave_at(g_mvp[b + 11], pmb) *
                                (depth >> 1)) >> 15;
                        pcb = (g_mv[b + 14] + inc_c +
                               (inc_c >> 8)) & 65535;
                        g_mv[b + 14] = pcb;
                        carb = wave_at(g_mvp[b + 12],
                                       (pcb + offb) & 65535);
                        outv = (outv >> 1) +
                               ((carb * env) >> 17);
                    }
                }
                t = outv >> lvl;
                part = g_mv[b + 12];
                vol = g_part[part * 8 + 1];
                pan = g_part[part * 8 + 2];
                t = (t * vol) / 127;
                sumL = sumL + ((t * (127 - pan)) >> 7);
                sumR = sumR + ((t * pan) >> 7);
                if (g_fxctl[0] != 0) {
                    g_psum[part * 2] = g_psum[part * 2] +
                         ((t * (127 - pan)) >> 7);
                    g_psum[part * 2 + 1] = g_psum[part * 2 + 1] +
                         ((t * pan) >> 7);
                }
            }
            g_mv[b + 3] = pc;
            g_mv[b + 4] = pm;
            g_mv[b + 5] = env;
            g_mv[b + 6] = stg;
            g_mv[b + 7] = gate;
            g_mv[b + 8] = menv;
        }
        vi = vi + 1;
    }
    if (sumL > 30000) {
        sumL = 30000;
    }
    if (sumL < 0 - 30000) {
        sumL = 0 - 30000;
    }
    if (sumR > 30000) {
        sumR = 30000;
    }
    if (sumR < 0 - 30000) {
        sumR = 0 - 30000;
    }
    g_mix[0] = sumL;
    g_mix[1] = sumR;
}

void mt_event(int32 status, int32 d1, int32 d2)
{
    int32 hi;
    int32 ch;
    hi = status & 240;
    ch = status & 15;
    if (ch >= 8) {
        return;  /* ch 9 以降は無視 (ch 10 ドラムは将来課題) */
    }
    if (hi == 128) {
        mt_note_off(ch, d1);
    } else if (hi == 144) {
        if (d2 == 0) {
            mt_note_off(ch, d1);
        } else {
            mt_note_on(ch, d1);
        }
    } else if (hi == 176) {
        if (d1 == 7) {
            g_part[ch * 8 + 1] = d2;
        } else if (d1 == 10) {
            g_part[ch * 8 + 2] = d2;
        } else if (d1 == 120 || d1 == 123) {
            mt_all_off(ch);
        } else {
            int32 m2;
            m2 = 0;
            while (m2 < FX_N) {
                if (g_fxmeta[m2 * 8 + 0] == d1) {
                    g_part[ch * 8 + 3 + m2] = d2;
                }
                m2 = m2 + 1;
            }
        }
    } else if (hi == 192) {
        int32 idx;
        idx = g_gmf[d1 >> 3] & 255;
        g_part[ch * 8 + 0] = idx;
        print_part_preset(ch, idx);
    }
}

void mt_feed(int32 b)
{
    if (g_mst[4] != 0) {
        if (b == 247) {
            if (g_mst[5] == 2) {
                g_mst[8] = 1;  /* 終了マーカー確定 */
            }
            g_mst[4] = 0;
            g_mst[5] = 0;
        } else if (g_mst[5] == 0) {
            if (b == 125) {
                g_mst[5] = 1;
            } else {
                g_mst[5] = 3;
            }
        } else if (g_mst[5] == 1) {
            if (b == 0) {
                g_mst[5] = 2;
            } else {
                g_mst[5] = 3;
            }
        } else {
            g_mst[5] = 3;
        }
        return;
    }
    if (b >= 248) {
        return;  /* リアルタイムメッセージは無視 */
    }
    if (b == 240) {
        g_mst[4] = 1;
        g_mst[5] = 0;
        g_mst[0] = 0;
        return;
    }
    if (b >= 240) {
        g_mst[0] = 0;
        g_mst[2] = 0;
        if (b == 241 || b == 243) {
            g_mst[6] = 1;
        } else if (b == 242) {
            g_mst[6] = 2;
        } else {
            g_mst[6] = 0;
        }
        return;
    }
    if (b >= 128) {
        int32 hi;
        hi = b & 240;
        g_mst[0] = b;
        if (hi == 192 || hi == 208) {
            g_mst[1] = 1;
        } else {
            g_mst[1] = 2;
        }
        g_mst[2] = 0;
        return;
    }
    if (g_mst[6] > 0) {
        g_mst[6] = g_mst[6] - 1;
        return;
    }
    if (g_mst[0] == 0) {
        return;  /* 行き先のないデータバイトは捨てる */
    }
    if (g_mst[2] == 0) {
        g_mst[3] = b;
        g_mst[2] = 1;
        if (g_mst[1] == 1) {
            mt_event(g_mst[0], b, 0);
            g_mst[2] = 0;
        }
    } else {
        mt_event(g_mst[0], g_mst[3], b);
        g_mst[2] = 0;
    }
}

int32 run_midi_mt(char *path, int32 pool, int32 dbg, int32 opovr)
{
    int64 fnv;
    int64 fnv_prime;
    int32 total;
    int64 r;
    int64 fd;
    int32 blen;
    int32 bpos;
    int32 i;
    int64 tot_cyc;
    int64 max_cyc;
    int64 labs;
    int64 rabs;
    int64 cyc_per_ms;
    int64 ins0;
    int32 peak;
    int32 chunkno;
    int64 base;
    int64 lastdata;
    int64 rendered;
    int64 tailend;
    int64 lastread;
    int64 ceacc;
    int32 fxp;
    fnv = fnv_init();
    fnv_prime = ((int64)256 << 32) + 435;
    total = 0;
    blen = 0;
    bpos = 0;
    tot_cyc = 0;
    max_cyc = 0;
    labs = 0;
    rabs = 0;
    cyc_per_ms = 0;
    peak = 0;
    chunkno = 0;
    rendered = 0;
    tailend = -1;
    lastread = -1000000;
    ceacc = 0;
    fxp = 0;
    i = 0;
    while (i < 16) {
        g_mst[i] = 0;
        i = i + 1;
    }
    mt_init(pool, dbg, opovr);
    fd = syscall3(SYS_OPEN, path, 0, 0);
    if (fd < 0) {
        out_str("synth: cannot open ");
        out_str(path);
        putc(10);
        return 1;
    }
    r = syscall3(SYS_AUDIO_OPEN, RATE, 2, 0);
    if (r != 0) {
        out_str("synth: cannot open audio\n");
        syscall3(SYS_FCLOSE, fd, 0, 0);
        return 1;
    }
    out_str("synth: midi mt mode pool=");
    out_num(pool);
    putc(10);
    i = 0;
    while (i < 8) {
        print_part_preset(i, g_part[i * 8 + 0]);
        i = i + 1;
    }
    fx_print_state();
    /* 演奏時計の起点は較正より前に取る (較正の 100ms も
     * 曲の時間のうち) */
    base = syscall3(SYS_UPTIME, 0, 0, 0);
    lastdata = base;
    /* サイクル/秒の較正: perf_read はマシン全体の通算
     * カウンタなので、100ms の sleep を挟んだ差分と
     * uptime の差分から 1ms あたりのサイクル数が測れる */
    {
        int64 c0;
        int64 t0;
        int64 t1;
        c0 = perf_read(0);
        t0 = syscall3(SYS_UPTIME, 0, 0, 0);
        syscall3(SYS_SLEEP, 100, 0, 0);
        t1 = syscall3(SYS_UPTIME, 0, 0, 0);
        if (t1 > t0) {
            cyc_per_ms = (perf_read(0) - c0) / (t1 - t0);
        }
    }
    ins0 = perf_read(1);
    while (1) {
        int64 now;
        int64 target;
        int32 nf;
        int32 k;
        now = syscall3(SYS_UPTIME, 0, 0, 0);
        if (tailend >= 0) {
            target = tailend;
        } else {
            target = (now - base) * LIVE_RATE / 1000 + LIVE_LEAD;
        }
        /* 入力ポーリングは、前回の読み取りから LIVE_BLOCK
         * フレーム描画が進んでからにする (run_midi と
         * 同じ理由: ファイル先行書式でイベントが先頭に
         * 潰れるのを防ぎ、消費を描画ペースに結ぶ) */
        if (tailend < 0 && rendered - lastread >= LIVE_BLOCK) {
            if (bpos >= blen) {
                int64 n;
                n = syscall3(SYS_READ, fd, g_mbuf, 128);
                if (n < 0) {
                    syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
                    syscall3(SYS_FCLOSE, fd, 0, 0);
                    out_str("synth: read failed\n");
                    return 1;
                }
                if (n > 0) {
                    blen = (int32)n;
                    bpos = 0;
                    lastdata = now;
                    lastread = rendered;
                }
            }
            while (bpos < blen) {
                mt_feed(g_mbuf[bpos] & 255);
                bpos = bpos + 1;
                if (g_mst[8] != 0) {
                    tailend = rendered + 11025;
                    target = tailend;
                    break;
                }
            }
            if (tailend < 0 && now - lastdata > 30000) {
                tailend = rendered + 11025;
                target = tailend;
            }
            mt_peak_update();
        }
        if (rendered >= target) {
            if (tailend >= 0) {
                break;
            }
            syscall3(SYS_SLEEP, 1, 0, 0);
            continue;
        }
        nf = LIVE_BLOCK;
        if (rendered + nf > target) {
            nf = (int32)(target - rendered);
        }
        {
            int64 c0;
            int64 cost;
            int64 ce;
            int64 sc;
            c0 = perf_read(0);
            ce = 0;
            k = 0;
            while (k < nf) {
                int32 L;
                int32 R;
                mt_mix();
                L = g_mix[0];
                R = g_mix[1];
                if (g_fxctl[0] != 0) {
                    int32 mod2;
                    int32 p2;
                    mod2 = 0;
                    while (mod2 < FX_N) {
                        int32 bl;
                        int32 br;
                        bl = 0;
                        br = 0;
                        p2 = 0;
                        while (p2 < NPART) {
                            int32 snd;
                            snd = g_part[p2 * 8 + 3 + mod2];
                            bl = bl + (g_psum[p2 * 2] * snd) / 127;
                            br = br + (g_psum[p2 * 2 + 1] * snd) / 127;
                            p2 = p2 + 1;
                        }
                        g_fxbus[mod2 * 2] = bl;
                        g_fxbus[mod2 * 2 + 1] = br;
                        mod2 = mod2 + 1;
                    }
                    fx_frame(2);
                    L = fx_clip(L + g_fxwet[0]);
                    R = fx_clip(R + g_fxwet[1]);
                }
                g_buf[k * 4] = (char)(L & 255);
                g_buf[k * 4 + 1] = (char)((L >> 8) & 255);
                g_buf[k * 4 + 2] = (char)(R & 255);
                g_buf[k * 4 + 3] = (char)((R >> 8) & 255);
                fnv = (fnv ^ (g_buf[k * 4] & 255)) * fnv_prime;
                fnv = (fnv ^ (g_buf[k * 4 + 1] & 255)) * fnv_prime;
                fnv = (fnv ^ (g_buf[k * 4 + 2] & 255)) * fnv_prime;
                fnv = (fnv ^ (g_buf[k * 4 + 3] & 255)) * fnv_prime;
                if (L < 0) {
                    labs = labs - L;
                    ce = ce - L;
                    if (0 - L > peak) {
                        peak = 0 - L;
                    }
                } else {
                    labs = labs + L;
                    ce = ce + L;
                    if (L > peak) {
                        peak = L;
                    }
                }
                if (R < 0) {
                    rabs = rabs - R;
                    ce = ce - R;
                    if (0 - R > peak) {
                        peak = 0 - R;
                    }
                } else {
                    rabs = rabs + R;
                    ce = ce + R;
                    if (R > peak) {
                        peak = R;
                    }
                }
                k = k + 1;
            }
            ceacc = ceacc + ce;
            fxp = fxp + nf;
            if (fxp >= 512) {
                if (g_fxctl[3] != 0) {
                    out_str("synth: fxdbg ");
                    out_num(chunkno);
                    putc(32);
                    out_num(ceacc);
                    putc(10);
                }
                chunkno = chunkno + 1;
                ceacc = 0;
                fxp = fxp - 512;
            }
            cost = perf_read(0) - c0;
            tot_cyc = tot_cyc + cost;
            /* 最悪チャンクは従来と比較できるよう 512
             * フレーム換算で見る */
            sc = cost * 512 / nf;
            if (sc > max_cyc) {
                max_cyc = sc;
            }
        }
        if (audio_write_all(nf * 2) != 0) {
            syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
            syscall3(SYS_FCLOSE, fd, 0, 0);
            out_str("synth: audio write failed\n");
            return 1;
        }
        rendered = rendered + nf;
        total = total + nf * 2;
    }
    if (g_fxctl[3] != 0 && fxp > 0) {
        out_str("synth: fxdbg ");
        out_num(chunkno);
        putc(32);
        out_num(ceacc);
        putc(10);
    }
    syscall3(SYS_FCLOSE, fd, 0, 0);
    r = syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
    if (r != 0) {
        out_str("synth: audio close failed\n");
        return 1;
    }
    out_str("synth: samples=");
    out_num(total);
    out_str(" fnv=");
    out_hex64(fnv);
    putc(10);
    out_str("synth: lr l=");
    out_num(labs);
    out_str(" r=");
    out_num(rabs);
    putc(10);
    out_str("synth: peak=");
    out_num(peak);
    putc(10);
    out_str("synth: perf cyc=");
    out_num(tot_cyc);
    out_str(" ins=");
    out_num(perf_read(1) - ins0);
    putc(10);
    out_str("synth: perf avg=");
    if (rendered > 0) {
        out_num(tot_cyc / rendered);
    } else {
        out_num(0);
    }
    out_str(" maxchunk=");
    out_num(max_cyc);
    out_str(" budget=");
    if (cyc_per_ms > 0) {
        out_num(max_cyc * 100 / (cyc_per_ms * 11609 / 1000));
    } else {
        out_num(0);
    }
    out_str("% peak=");
    out_num(g_mctl[3]);
    out_str(" pool=");
    out_num(pool);
    putc(10);
    out_str("synth: done\n");
    return 0;
}

int32 main(int32 argc, char **argv)
{
    int32 mode;
    int32 engine;
    int32 preset;
    int32 pool;
    int32 dbg;
    int32 opovr;
    int32 ai;
    mode = 0;
    engine = 0;
    preset = 0;
    pool = 32;
    dbg = 0;
    opovr = 0;
    fx_init();
    if (argc >= 2) {
        if (streq(argv[1], "fm") || streq(argv[1], "psg")) {
            mode = 0;
            if (streq(argv[1], "psg")) {
                engine = 1;
            }
            ai = 2;
            while (ai < argc) {
                if (fx_arg(argv[ai])) {
                } else {
                    preset = parse_num(argv[ai]);
                    if (preset < 0 || (engine == 0 && preset > 21) ||
                        (engine == 1 && preset > 15)) {
                        out_str("usage: synth [fm|psg [preset] [fx]|play [fm|psg] [preset] [fx]|midi <file> [fm|psg] [fx]]\n");
                        return 2;
                    }
                }
                ai = ai + 1;
            }
        } else if (streq(argv[1], "play")) {
            mode = 1;
            engine = 0;
            ai = 2;
            while (ai < argc) {
                if (fx_arg(argv[ai])) {
                } else if (ai == 2 && streq(argv[ai], "psg")) {
                    engine = 1;
                } else if (ai == 2 && streq(argv[ai], "fm")) {
                    engine = 0;
                } else {
                    preset = parse_num(argv[ai]);
                    if (preset < 0 || (engine == 0 && preset > 21) ||
                        (engine == 1 && preset > 15)) {
                        out_str("usage: synth [fm|psg [preset] [fx]|play [fm|psg] [preset] [fx]|midi <file> [fm|psg] [fx]]\n");
                        return 2;
                    }
                }
                ai = ai + 1;
            }
        } else if (streq(argv[1], "midi")) {
            if (argc < 3) {
                out_str("usage: synth midi <file> [fm|psg] [pool] [2op|4op] [dbg] [nofx|rev|dly|cho] [fxchain] [fxdbg]\n");
                return 2;
            }
            mode = 2;
            ai = 3;
            while (ai < argc) {
                if (streq(argv[ai], "psg")) {
                    engine = 1;
                } else if (streq(argv[ai], "fm")) {
                    engine = 0;
                } else if (streq(argv[ai], "dbg")) {
                    dbg = 1;
                } else if (fx_arg(argv[ai])) {
                } else if (streq(argv[ai], "2op")) {
                    opovr = 1;
                } else if (streq(argv[ai], "4op")) {
                    opovr = 2;
                } else {
                    int32 pv;
                    pv = parse_num(argv[ai]);
                    if (pv >= 1 && pv <= 32) {
                        pool = pv;
                    } else {
                        out_str("usage: synth midi <file> [fm|psg] [pool] [2op|4op] [dbg] [nofx|rev|dly|cho] [fxchain] [fxdbg]\n");
                        return 2;
                    }
                }
                ai = ai + 1;
            }
        } else {
            out_str("usage: synth [fm|psg [preset] [fx]|play [fm|psg] [preset] [fx]|midi <file> [fm|psg] [fx]]\n");
            return 2;
        }
    }
    fill_sine();
    fill_waves();
    fill_pow2();
    fill_ninc();
    fill_song();
    fill_presets();
    if (mode == 2) {
        if (engine == 1) {
            return run_midi(argv[2], 1);
        }
        return run_midi_mt(argv[2], pool, dbg, opovr);
    }
    load_preset(engine, preset);
    if (mode == 0) {
        return run_demo(engine);
    }
    return run_play(engine);
}
