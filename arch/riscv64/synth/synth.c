/* synth.c -- KosabaOS のシンセ音源アプリ (doc/synth-design.md 段1)
 *
 *   run synth          FM デモ曲 (引数なしの既定)
 *   run synth fm       FM デモ曲
 *   run synth psg      PSG デモ曲 (同じ曲)
 *   run synth play fm  シリアル鍵盤で FM を弾く
 *   run synth play psg シリアル鍵盤で PSG を弾く
 *   run synth fm <n>   プリセット n (0..15) でデモ曲
 *                      (play でも第 3 引数で指定可。対話中は
 *                      [ / ] キーで切り替え)
 *   run synth midi <p> [fm|psg] MIDI バイト列ストリームを
 *                      読んで鳴らす (エンジン既定は fm)
 *                      (smfplay の file モードの受け手。解釈は
 *                      下の run_midi のコメント参照)
 *
 * IwaC の C サブセットで書く (nami / wavgen と同じ作法):
 * #include / libc なし、switch なし、文字リテラルなし
 * (文字コードを数で書く)、浮動小数点なし (全部固定小数点)。
 * ファイルスコープには char 配列だけ置き (nori のデータ
 * 配置ずれ対策、doc/nami-design.md)、表とボイス状態は
 * その char 配列へ rd32 / wr32 のバイト合成で読み書きする。
 * 表は初期化子にせず、関数内で代入で埋める。main に大きな
 * ローカル配列を置くと IwaC のフレーム生成が壊れる
 * (実測あり。rd32 / wr32 のコメント参照) ので、main と
 * run_demo / run_play のローカルはスカラだけに保つ。
 *
 * 音源 (段1):
 *   FM  : 2 オペレータ (変調側の位相で搬送側を変調)。
 *         正弦波は 256 点テーブル + 線形補間。変調の深さは
 *         専用エンベロープ (menv) で減衰するので、ベル /
 *         エレピ寄りの立ち上がりになる
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


/* 大きな表とボイス状態はファイルスコープの char 配列に置く。
 * main に大きなローカル配列を置くと IwaC のフレーム生成が
 * 壊れる (実測: フレーム 1504 バイトで addi sp の符号が反転し
 * スタック上に書いて即フォールト) ことへの対策でもある。
 * int32 の読み書きはバイト合成の rd32 / wr32 で行うので
 * misaligned にもならない (doc/nami-design.md の作法) */
char g_sine[1024];  /* 正弦波 256 点 x 4 バイト */
char g_ninc[244];   /* 音程表 61 点 x 4 バイト */
char g_mel[32];
char g_bas[32];
char g_vs[512];     /* ボイス状態 64 ワード x 4 バイト */

/* プリセット (doc/synth-design.md のプリセットの節)。
 * 1 件 20 バイトのバイトパックで、FM / PSG 各 16 件:
 *   +0 波形 (FM=0, PSG=1 矩形 / 2 ノイズ)  +1 レベルシフト
 *   +2 atk  +4 dec  +6 sus  +8 rel (各 2 バイト LE)
 *   FM : +10 変調比 x2  +12 変調の深さ  +14 変調 env 減衰
 *        +16 変調 env 減衰 (release 時)
 *   PSG: +10 デューティ閾値  +12 ノイズ混合量  +13 オクターブ
 * 名前は 16 バイト枠 x 16 件。g_vp はボイスごとの実効
 * パラメータ (trig がプリセットから写す、1 ボイス 16 ワード):
 *   +0 atk +1 dec +2 sus +3 rel +4 変調の深さ +5 変調 env
 *   減衰 +6 同 (release) +7 デューティ +8 ノイズ混合
 *   +9 レベルシフト
 * g_cur は現在の (エンジン, プリセット番号) の 2 ワード。
 * g_gmf / g_gmp は GM プログラム番号 (>>3 のファミリー)
 * からプリセット番号への対応表 */
char g_pfm[320];
char g_pps[320];
char g_fnm[256];
char g_pnm[256];
char g_vp[512];
char g_cur[32];
char g_gmf[16];
char g_gmp[16];

int32 rd32(char *p, int32 idx)
{
    int32 o;
    o = idx * 4;
    return (p[o] & 255) | ((p[o + 1] & 255) << 8) |
           ((p[o + 2] & 255) << 16) | ((p[o + 3] & 255) << 24);
}

void wr32(char *p, int32 idx, int32 v)
{
    int32 o;
    o = idx * 4;
    p[o] = (char)(v & 255);
    p[o + 1] = (char)((v >> 8) & 255);
    p[o + 2] = (char)((v >> 16) & 255);
    p[o + 3] = (char)((v >> 24) & 255);
}

/* 正弦波テーブル (Q15、256 点) を代入で埋める */
void fill_sine()
{
    wr32(g_sine, 0, 0);
    wr32(g_sine, 1, 804);
    wr32(g_sine, 2, 1608);
    wr32(g_sine, 3, 2410);
    wr32(g_sine, 4, 3212);
    wr32(g_sine, 5, 4011);
    wr32(g_sine, 6, 4808);
    wr32(g_sine, 7, 5602);
    wr32(g_sine, 8, 6393);
    wr32(g_sine, 9, 7179);
    wr32(g_sine, 10, 7962);
    wr32(g_sine, 11, 8739);
    wr32(g_sine, 12, 9512);
    wr32(g_sine, 13, 10278);
    wr32(g_sine, 14, 11039);
    wr32(g_sine, 15, 11793);
    wr32(g_sine, 16, 12539);
    wr32(g_sine, 17, 13279);
    wr32(g_sine, 18, 14010);
    wr32(g_sine, 19, 14732);
    wr32(g_sine, 20, 15446);
    wr32(g_sine, 21, 16151);
    wr32(g_sine, 22, 16846);
    wr32(g_sine, 23, 17530);
    wr32(g_sine, 24, 18204);
    wr32(g_sine, 25, 18868);
    wr32(g_sine, 26, 19519);
    wr32(g_sine, 27, 20159);
    wr32(g_sine, 28, 20787);
    wr32(g_sine, 29, 21403);
    wr32(g_sine, 30, 22005);
    wr32(g_sine, 31, 22594);
    wr32(g_sine, 32, 23170);
    wr32(g_sine, 33, 23731);
    wr32(g_sine, 34, 24279);
    wr32(g_sine, 35, 24811);
    wr32(g_sine, 36, 25329);
    wr32(g_sine, 37, 25832);
    wr32(g_sine, 38, 26319);
    wr32(g_sine, 39, 26790);
    wr32(g_sine, 40, 27245);
    wr32(g_sine, 41, 27683);
    wr32(g_sine, 42, 28105);
    wr32(g_sine, 43, 28510);
    wr32(g_sine, 44, 28898);
    wr32(g_sine, 45, 29268);
    wr32(g_sine, 46, 29621);
    wr32(g_sine, 47, 29956);
    wr32(g_sine, 48, 30273);
    wr32(g_sine, 49, 30571);
    wr32(g_sine, 50, 30852);
    wr32(g_sine, 51, 31113);
    wr32(g_sine, 52, 31356);
    wr32(g_sine, 53, 31580);
    wr32(g_sine, 54, 31785);
    wr32(g_sine, 55, 31971);
    wr32(g_sine, 56, 32137);
    wr32(g_sine, 57, 32285);
    wr32(g_sine, 58, 32412);
    wr32(g_sine, 59, 32521);
    wr32(g_sine, 60, 32609);
    wr32(g_sine, 61, 32678);
    wr32(g_sine, 62, 32728);
    wr32(g_sine, 63, 32757);
    wr32(g_sine, 64, 32767);
    wr32(g_sine, 65, 32757);
    wr32(g_sine, 66, 32728);
    wr32(g_sine, 67, 32678);
    wr32(g_sine, 68, 32609);
    wr32(g_sine, 69, 32521);
    wr32(g_sine, 70, 32412);
    wr32(g_sine, 71, 32285);
    wr32(g_sine, 72, 32137);
    wr32(g_sine, 73, 31971);
    wr32(g_sine, 74, 31785);
    wr32(g_sine, 75, 31580);
    wr32(g_sine, 76, 31356);
    wr32(g_sine, 77, 31113);
    wr32(g_sine, 78, 30852);
    wr32(g_sine, 79, 30571);
    wr32(g_sine, 80, 30273);
    wr32(g_sine, 81, 29956);
    wr32(g_sine, 82, 29621);
    wr32(g_sine, 83, 29268);
    wr32(g_sine, 84, 28898);
    wr32(g_sine, 85, 28510);
    wr32(g_sine, 86, 28105);
    wr32(g_sine, 87, 27683);
    wr32(g_sine, 88, 27245);
    wr32(g_sine, 89, 26790);
    wr32(g_sine, 90, 26319);
    wr32(g_sine, 91, 25832);
    wr32(g_sine, 92, 25329);
    wr32(g_sine, 93, 24811);
    wr32(g_sine, 94, 24279);
    wr32(g_sine, 95, 23731);
    wr32(g_sine, 96, 23170);
    wr32(g_sine, 97, 22594);
    wr32(g_sine, 98, 22005);
    wr32(g_sine, 99, 21403);
    wr32(g_sine, 100, 20787);
    wr32(g_sine, 101, 20159);
    wr32(g_sine, 102, 19519);
    wr32(g_sine, 103, 18868);
    wr32(g_sine, 104, 18204);
    wr32(g_sine, 105, 17530);
    wr32(g_sine, 106, 16846);
    wr32(g_sine, 107, 16151);
    wr32(g_sine, 108, 15446);
    wr32(g_sine, 109, 14732);
    wr32(g_sine, 110, 14010);
    wr32(g_sine, 111, 13279);
    wr32(g_sine, 112, 12539);
    wr32(g_sine, 113, 11793);
    wr32(g_sine, 114, 11039);
    wr32(g_sine, 115, 10278);
    wr32(g_sine, 116, 9512);
    wr32(g_sine, 117, 8739);
    wr32(g_sine, 118, 7962);
    wr32(g_sine, 119, 7179);
    wr32(g_sine, 120, 6393);
    wr32(g_sine, 121, 5602);
    wr32(g_sine, 122, 4808);
    wr32(g_sine, 123, 4011);
    wr32(g_sine, 124, 3212);
    wr32(g_sine, 125, 2410);
    wr32(g_sine, 126, 1608);
    wr32(g_sine, 127, 804);
    wr32(g_sine, 128, 0);
    wr32(g_sine, 129, -804);
    wr32(g_sine, 130, -1608);
    wr32(g_sine, 131, -2410);
    wr32(g_sine, 132, -3212);
    wr32(g_sine, 133, -4011);
    wr32(g_sine, 134, -4808);
    wr32(g_sine, 135, -5602);
    wr32(g_sine, 136, -6393);
    wr32(g_sine, 137, -7179);
    wr32(g_sine, 138, -7962);
    wr32(g_sine, 139, -8739);
    wr32(g_sine, 140, -9512);
    wr32(g_sine, 141, -10278);
    wr32(g_sine, 142, -11039);
    wr32(g_sine, 143, -11793);
    wr32(g_sine, 144, -12539);
    wr32(g_sine, 145, -13279);
    wr32(g_sine, 146, -14010);
    wr32(g_sine, 147, -14732);
    wr32(g_sine, 148, -15446);
    wr32(g_sine, 149, -16151);
    wr32(g_sine, 150, -16846);
    wr32(g_sine, 151, -17530);
    wr32(g_sine, 152, -18204);
    wr32(g_sine, 153, -18868);
    wr32(g_sine, 154, -19519);
    wr32(g_sine, 155, -20159);
    wr32(g_sine, 156, -20787);
    wr32(g_sine, 157, -21403);
    wr32(g_sine, 158, -22005);
    wr32(g_sine, 159, -22594);
    wr32(g_sine, 160, -23170);
    wr32(g_sine, 161, -23731);
    wr32(g_sine, 162, -24279);
    wr32(g_sine, 163, -24811);
    wr32(g_sine, 164, -25329);
    wr32(g_sine, 165, -25832);
    wr32(g_sine, 166, -26319);
    wr32(g_sine, 167, -26790);
    wr32(g_sine, 168, -27245);
    wr32(g_sine, 169, -27683);
    wr32(g_sine, 170, -28105);
    wr32(g_sine, 171, -28510);
    wr32(g_sine, 172, -28898);
    wr32(g_sine, 173, -29268);
    wr32(g_sine, 174, -29621);
    wr32(g_sine, 175, -29956);
    wr32(g_sine, 176, -30273);
    wr32(g_sine, 177, -30571);
    wr32(g_sine, 178, -30852);
    wr32(g_sine, 179, -31113);
    wr32(g_sine, 180, -31356);
    wr32(g_sine, 181, -31580);
    wr32(g_sine, 182, -31785);
    wr32(g_sine, 183, -31971);
    wr32(g_sine, 184, -32137);
    wr32(g_sine, 185, -32285);
    wr32(g_sine, 186, -32412);
    wr32(g_sine, 187, -32521);
    wr32(g_sine, 188, -32609);
    wr32(g_sine, 189, -32678);
    wr32(g_sine, 190, -32728);
    wr32(g_sine, 191, -32757);
    wr32(g_sine, 192, -32767);
    wr32(g_sine, 193, -32757);
    wr32(g_sine, 194, -32728);
    wr32(g_sine, 195, -32678);
    wr32(g_sine, 196, -32609);
    wr32(g_sine, 197, -32521);
    wr32(g_sine, 198, -32412);
    wr32(g_sine, 199, -32285);
    wr32(g_sine, 200, -32137);
    wr32(g_sine, 201, -31971);
    wr32(g_sine, 202, -31785);
    wr32(g_sine, 203, -31580);
    wr32(g_sine, 204, -31356);
    wr32(g_sine, 205, -31113);
    wr32(g_sine, 206, -30852);
    wr32(g_sine, 207, -30571);
    wr32(g_sine, 208, -30273);
    wr32(g_sine, 209, -29956);
    wr32(g_sine, 210, -29621);
    wr32(g_sine, 211, -29268);
    wr32(g_sine, 212, -28898);
    wr32(g_sine, 213, -28510);
    wr32(g_sine, 214, -28105);
    wr32(g_sine, 215, -27683);
    wr32(g_sine, 216, -27245);
    wr32(g_sine, 217, -26790);
    wr32(g_sine, 218, -26319);
    wr32(g_sine, 219, -25832);
    wr32(g_sine, 220, -25329);
    wr32(g_sine, 221, -24811);
    wr32(g_sine, 222, -24279);
    wr32(g_sine, 223, -23731);
    wr32(g_sine, 224, -23170);
    wr32(g_sine, 225, -22594);
    wr32(g_sine, 226, -22005);
    wr32(g_sine, 227, -21403);
    wr32(g_sine, 228, -20787);
    wr32(g_sine, 229, -20159);
    wr32(g_sine, 230, -19519);
    wr32(g_sine, 231, -18868);
    wr32(g_sine, 232, -18204);
    wr32(g_sine, 233, -17530);
    wr32(g_sine, 234, -16846);
    wr32(g_sine, 235, -16151);
    wr32(g_sine, 236, -15446);
    wr32(g_sine, 237, -14732);
    wr32(g_sine, 238, -14010);
    wr32(g_sine, 239, -13279);
    wr32(g_sine, 240, -12539);
    wr32(g_sine, 241, -11793);
    wr32(g_sine, 242, -11039);
    wr32(g_sine, 243, -10278);
    wr32(g_sine, 244, -9512);
    wr32(g_sine, 245, -8739);
    wr32(g_sine, 246, -7962);
    wr32(g_sine, 247, -7179);
    wr32(g_sine, 248, -6393);
    wr32(g_sine, 249, -5602);
    wr32(g_sine, 250, -4808);
    wr32(g_sine, 251, -4011);
    wr32(g_sine, 252, -3212);
    wr32(g_sine, 253, -2410);
    wr32(g_sine, 254, -1608);
    wr32(g_sine, 255, -804);
 }

/* MIDI ノート 36..96 の位相増分を代入で埋める */
void fill_ninc()
{
    wr32(g_ninc, 0, 97);
    wr32(g_ninc, 1, 103);
    wr32(g_ninc, 2, 109);
    wr32(g_ninc, 3, 116);
    wr32(g_ninc, 4, 122);
    wr32(g_ninc, 5, 130);
    wr32(g_ninc, 6, 137);
    wr32(g_ninc, 7, 146);
    wr32(g_ninc, 8, 154);
    wr32(g_ninc, 9, 163);
    wr32(g_ninc, 10, 173);
    wr32(g_ninc, 11, 183);
    wr32(g_ninc, 12, 194);
    wr32(g_ninc, 13, 206);
    wr32(g_ninc, 14, 218);
    wr32(g_ninc, 15, 231);
    wr32(g_ninc, 16, 245);
    wr32(g_ninc, 17, 259);
    wr32(g_ninc, 18, 275);
    wr32(g_ninc, 19, 291);
    wr32(g_ninc, 20, 309);
    wr32(g_ninc, 21, 327);
    wr32(g_ninc, 22, 346);
    wr32(g_ninc, 23, 367);
    wr32(g_ninc, 24, 389);
    wr32(g_ninc, 25, 412);
    wr32(g_ninc, 26, 436);
    wr32(g_ninc, 27, 462);
    wr32(g_ninc, 28, 490);
    wr32(g_ninc, 29, 519);
    wr32(g_ninc, 30, 550);
    wr32(g_ninc, 31, 583);
    wr32(g_ninc, 32, 617);
    wr32(g_ninc, 33, 654);
    wr32(g_ninc, 34, 693);
    wr32(g_ninc, 35, 734);
    wr32(g_ninc, 36, 778);
    wr32(g_ninc, 37, 824);
    wr32(g_ninc, 38, 873);
    wr32(g_ninc, 39, 925);
    wr32(g_ninc, 40, 980);
    wr32(g_ninc, 41, 1038);
    wr32(g_ninc, 42, 1100);
    wr32(g_ninc, 43, 1165);
    wr32(g_ninc, 44, 1234);
    wr32(g_ninc, 45, 1308);
    wr32(g_ninc, 46, 1386);
    wr32(g_ninc, 47, 1468);
    wr32(g_ninc, 48, 1555);
    wr32(g_ninc, 49, 1648);
    wr32(g_ninc, 50, 1746);
    wr32(g_ninc, 51, 1849);
    wr32(g_ninc, 52, 1959);
    wr32(g_ninc, 53, 2076);
    wr32(g_ninc, 54, 2199);
    wr32(g_ninc, 55, 2330);
    wr32(g_ninc, 56, 2469);
    wr32(g_ninc, 57, 2615);
    wr32(g_ninc, 58, 2771);
    wr32(g_ninc, 59, 2936);
    wr32(g_ninc, 60, 3110);
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

/* 正弦波の線形補間。phase は 1 周 = 65536 */
int32 sine_at(int32 phase)
{
    int32 idx;
    int32 frac;
    int32 a;
    int32 b;
    idx = phase >> 8;
    frac = phase & 255;
    a = rd32(g_sine, idx);
    b = rd32(g_sine, (idx + 1) & 255);
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
    eng = rd32(g_cur, 0);
    if (eng == 0) {
        tab = g_pfm;
    } else {
        tab = g_pps;
    }
    o = rd32(g_cur, 1) * 20;
    if (type < 0) {
        type = tab[o] & 255;  /* 波形はプリセットから */
    }
    wr32(g_vs, b + 0, 1);
    wr32(g_vs, b + 1, type);
    wr32(g_vs, b + 2, note);
    wr32(g_vs, b + 3, 0);
    wr32(g_vs, b + 4, 0);
    wr32(g_vs, b + 5, 0);
    wr32(g_vs, b + 6, 0);
    wr32(g_vs, b + 7, gate);
    wr32(g_vs, b + 8, 65535);
    wr32(g_vs, b + 9, 257);
    wr32(g_vp, b + 0, rd16u(tab, o + 2));
    wr32(g_vp, b + 1, rd16u(tab, o + 4));
    wr32(g_vp, b + 2, rd16u(tab, o + 6));
    wr32(g_vp, b + 3, rd16u(tab, o + 8));
    wr32(g_vp, b + 9, tab[o + 1] & 255);
    if (eng == 0) {
        wr32(g_vp, b + 4, rd16u(tab, o + 12));
        wr32(g_vp, b + 5, rd16u(tab, o + 14));
        wr32(g_vp, b + 6, rd16u(tab, o + 16));
        wr32(g_vp, b + 7, 32768);
        wr32(g_vp, b + 8, 0);
    } else {
        wr32(g_vp, b + 4, 0);
        wr32(g_vp, b + 5, 30);
        wr32(g_vp, b + 6, 200);
        wr32(g_vp, b + 7, rd16u(tab, o + 10));
        wr32(g_vp, b + 8, tab[o + 12] & 255);
    }
    if (note >= 36 && note <= 96) {
        inc = rd32(g_ninc, note - 36);
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
    wr32(g_vs, b + 10, inc);
    if (eng == 0) {
        wr32(g_vs, b + 11, (inc * (tab[o + 10] & 255)) / 2);
    } else {
        wr32(g_vs, b + 11, inc);
    }
}

/* 1 サンプル分のミックス (全ボイスの env も 1 段進める) */
int32 mix_sample()
{
    int32 sum;
    int32 vi;
    sum = 0;
    vi = 0;
    while (vi < 4) {
        int32 b;
        b = vi * 16;
        if (rd32(g_vs, b + 0) != 0) {
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
            typ = rd32(g_vs, b + 1);
            pc = rd32(g_vs, b + 3);
            pm = rd32(g_vs, b + 4);
            env = rd32(g_vs, b + 5);
            stg = rd32(g_vs, b + 6);
            gate = rd32(g_vs, b + 7);
            menv = rd32(g_vs, b + 8);
            lfsr = rd32(g_vs, b + 9);
            inc_c = rd32(g_vs, b + 10);
            inc_m = rd32(g_vs, b + 11);
            atk = rd32(g_vp, b + 0);
            dec = rd32(g_vp, b + 1);
            sus = rd32(g_vp, b + 2);
            rel = rd32(g_vp, b + 3);
            dep = rd32(g_vp, b + 4);
            mrate = rd32(g_vp, b + 5);
            mrel = rd32(g_vp, b + 6);
            duty = rd32(g_vp, b + 7);
            nmix = rd32(g_vp, b + 8);
            lvl = rd32(g_vp, b + 9);
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
                    wr32(g_vs, b + 0, 0);
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
            if (rd32(g_vs, b + 0) != 0) {
                if (typ == 0) {
                    int32 mod;
                    int32 depth;
                    int32 off;
                    int32 car;
                    pm = (pm + inc_m) & 65535;
                    mod = sine_at(pm);
                    depth = (dep * menv) >> 16;
                    off = (mod * depth) >> 15;
                    pc = (pc + inc_c) & 65535;
                    car = sine_at((pc + off) & 65535);
                    outv = (car * env) >> 16;
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
            wr32(g_vs, b + 3, pc);
            wr32(g_vs, b + 4, pm);
            wr32(g_vs, b + 5, env);
            wr32(g_vs, b + 6, stg);
            wr32(g_vs, b + 7, gate);
            wr32(g_vs, b + 8, menv);
            wr32(g_vs, b + 9, lfsr);
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
char g_mst[64];
char g_mbuf[512];   /* ストリームの読みバッファ */

/* プリセットを現在の音色にする。MIDI モードのノートオンが
 * 波形を読む g_mst +9 も一緒に更新する */
void load_preset(int32 eng, int32 idx)
{
    wr32(g_cur, 0, eng);
    wr32(g_cur, 1, idx);
    if (eng == 0) {
        wr32(g_mst, 9, g_pfm[idx * 20] & 255);
    } else {
        wr32(g_mst, 9, g_pps[idx * 20] & 255);
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
    print_preset(engine, rd32(g_cur, 1));
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
    out_str("synth: play mode. keys=z..m/q..i, o/p=octave, [/]=preset, ESC=end\n");
    print_preset(engine, rd32(g_cur, 1));
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
            load_preset(engine, (rd32(g_cur, 1) + 15) & 15);
            print_preset(engine, rd32(g_cur, 1));
        } else if (c == 93) {
            load_preset(engine, (rd32(g_cur, 1) + 1) & 15);
            print_preset(engine, rd32(g_cur, 1));
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
        if (rd32(g_vs, b + 0) != 0 && rd32(g_vs, b + 2) == note) {
            wr32(g_vs, b + 6, 3);
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
        if (rd32(g_vs, b + 0) != 0 && rd32(g_vs, b + 2) == note) {
            trig(vi, note, 2000000000, rd32(g_mst, 9));
            return;
        }
        vi = vi + 1;
    }
    vi = 0;
    while (vi < 4) {
        if (rd32(g_vs, vi * 16) == 0) {
            trig(vi, note, 2000000000, rd32(g_mst, 9));
            return;
        }
        vi = vi + 1;
    }
    rr = rd32(g_mst, 10);
    trig(rr, note, 2000000000, rd32(g_mst, 9));
    wr32(g_mst, 10, (rr + 1) & 3);
}

void midi_all_off()
{
    int32 vi;
    vi = 0;
    while (vi < 4) {
        if (rd32(g_vs, vi * 16) != 0) {
            wr32(g_vs, vi * 16 + 6, 3);
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
        }
    } else if (hi == 192) {
        int32 eng;
        int32 idx;
        eng = rd32(g_cur, 0);
        wr32(g_mst, 7, d1);
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
    if (rd32(g_mst, 4) != 0) {
        if (b == 247) {
            if (rd32(g_mst, 5) == 2) {
                wr32(g_mst, 8, 1);  /* 終了マーカー確定 */
            }
            wr32(g_mst, 4, 0);
            wr32(g_mst, 5, 0);
        } else if (rd32(g_mst, 5) == 0) {
            if (b == 125) {
                wr32(g_mst, 5, 1);
            } else {
                wr32(g_mst, 5, 3);
            }
        } else if (rd32(g_mst, 5) == 1) {
            if (b == 0) {
                wr32(g_mst, 5, 2);
            } else {
                wr32(g_mst, 5, 3);
            }
        } else {
            wr32(g_mst, 5, 3);
        }
        return;
    }
    if (b >= 248) {
        return;  /* リアルタイムメッセージは無視 */
    }
    if (b == 240) {
        wr32(g_mst, 4, 1);
        wr32(g_mst, 5, 0);
        wr32(g_mst, 0, 0);
        return;
    }
    if (b >= 240) {
        wr32(g_mst, 0, 0);
        wr32(g_mst, 2, 0);
        if (b == 241 || b == 243) {
            wr32(g_mst, 6, 1);
        } else if (b == 242) {
            wr32(g_mst, 6, 2);
        } else {
            wr32(g_mst, 6, 0);
        }
        return;
    }
    if (b >= 128) {
        int32 hi;
        hi = b & 240;
        wr32(g_mst, 0, b);
        if (hi == 192 || hi == 208) {
            wr32(g_mst, 1, 1);
        } else {
            wr32(g_mst, 1, 2);
        }
        wr32(g_mst, 2, 0);
        return;
    }
    if (rd32(g_mst, 6) > 0) {
        wr32(g_mst, 6, rd32(g_mst, 6) - 1);
        return;
    }
    if (rd32(g_mst, 0) == 0) {
        return;  /* 行き先のないデータバイトは捨てる */
    }
    if (rd32(g_mst, 2) == 0) {
        wr32(g_mst, 3, b);
        wr32(g_mst, 2, 1);
        if (rd32(g_mst, 1) == 1) {
            midi_event(rd32(g_mst, 0), b, 0);
            wr32(g_mst, 2, 0);
        }
    } else {
        midi_event(rd32(g_mst, 0), rd32(g_mst, 3), b);
        wr32(g_mst, 2, 0);
    }
}

/* MIDI ストリーム再生モード: ファイルをバイト列ストリーム
 * としてポーリング読みし、MIDI を逐次解釈して鳴らす。
 * 自分の読み位置 (fd) を持ち、まだ書かれていない分は
 * read が 0 を返すので、描画を続けながら次の塊で再試行
 * する (書き手が smfplay の同時実行ジョブでも、書き終えた
 * 後から読み始めてもよい)。ストリーム自体に時刻情報は
 * 無いので、タイミングは「届いた時刻」が全て (本物の MIDI
 * と同じ)。終了マーカー F0 7D 00 F7 を読んだら、release の
 * 余韻として 11025 サンプル描いてから閉じる。マーカーが
 * 来ないまま約 30 秒分 (サンプル数換算) 新着が無ければ、
 * 書き手が死んだとみなして同じく閉じる */
int32 run_midi(char *path, int32 engine)
{
    int64 fnv;
    int64 fnv_prime;
    int32 total;
    int64 r;
    int64 fd;
    int32 blen;
    int32 bpos;
    int32 idle;
    int32 tail;
    int32 i;
    fnv = fnv_init();
    fnv_prime = ((int64)256 << 32) + 435;
    total = 0;
    blen = 0;
    bpos = 0;
    idle = 0;
    tail = -1;
    i = 0;
    while (i < 16) {
        wr32(g_mst, i, 0);
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
    load_preset(engine, 0);
    print_preset(engine, 0);
    while (1) {
        int32 k;
        if (tail < 0) {
            if (bpos >= blen) {
                int64 n;
                n = syscall3(SYS_READ, fd, g_mbuf, 128);
                if (n < 0) {
                    syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
                    syscall3(SYS_FCLOSE, fd, 0, 0);
                    out_str("synth: read failed\n");
                    return 1;
                }
                if (n == 0) {
                    idle = idle + 1;
                    syscall3(SYS_SLEEP, 5, 0, 0);
                } else {
                    blen = (int32)n;
                    bpos = 0;
                    idle = 0;
                }
            }
            while (bpos < blen) {
                midi_feed(g_mbuf[bpos] & 255);
                bpos = bpos + 1;
                if (rd32(g_mst, 8) != 0) {
                    tail = 0;
                    break;
                }
            }
            if (idle > 2600) {
                tail = 0;  /* 新着なしが長すぎる: 終わりとみなす */
            }
        }
        k = 0;
        while (k < 512) {
            int32 smp;
            smp = mix_sample();
            g_buf[k * 2] = (char)(smp & 255);
            g_buf[k * 2 + 1] = (char)((smp >> 8) & 255);
            fnv = (fnv ^ (g_buf[k * 2] & 255)) * fnv_prime;
            fnv = (fnv ^ (g_buf[k * 2 + 1] & 255)) * fnv_prime;
            k = k + 1;
        }
        if (audio_write_all(512) != 0) {
            syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
            syscall3(SYS_FCLOSE, fd, 0, 0);
            out_str("synth: audio write failed\n");
            return 1;
        }
        total = total + 512;
        if (tail >= 0) {
            tail = tail + 512;
            if (tail >= 11025) {
                break;
            }
        }
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

int32 main(int32 argc, char **argv)
{
    int32 mode;
    int32 engine;
    int32 preset;
    mode = 0;
    engine = 0;
    preset = 0;
    if (argc >= 2) {
        if (streq(argv[1], "fm") || streq(argv[1], "psg")) {
            mode = 0;
            if (streq(argv[1], "psg")) {
                engine = 1;
            }
            if (argc >= 3) {
                preset = parse_num(argv[2]);
                if (preset < 0 || preset > 15) {
                    out_str("usage: synth [fm|psg [preset]|play [fm|psg] [preset]|midi <file> [fm|psg]]\n");
                    return 2;
                }
            }
        } else if (streq(argv[1], "play")) {
            mode = 1;
            engine = 0;
            if (argc >= 3) {
                if (streq(argv[2], "psg")) {
                    engine = 1;
                } else if (streq(argv[2], "fm")) {
                    engine = 0;
                } else {
                    out_str("usage: synth [fm|psg [preset]|play [fm|psg] [preset]|midi <file> [fm|psg]]\n");
                    return 2;
                }
            }
            if (argc >= 4) {
                preset = parse_num(argv[3]);
                if (preset < 0 || preset > 15) {
                    out_str("usage: synth [fm|psg [preset]|play [fm|psg] [preset]|midi <file> [fm|psg]]\n");
                    return 2;
                }
            }
        } else if (streq(argv[1], "midi")) {
            if (argc < 3) {
                out_str("usage: synth [fm|psg [preset]|play [fm|psg] [preset]|midi <file> [fm|psg]]\n");
                return 2;
            }
            mode = 2;
            if (argc >= 4) {
                if (streq(argv[3], "psg")) {
                    engine = 1;
                } else if (streq(argv[3], "fm")) {
                    engine = 0;
                } else {
                    out_str("usage: synth [fm|psg [preset]|play [fm|psg] [preset]|midi <file> [fm|psg]]\n");
                    return 2;
                }
            }
        } else {
            out_str("usage: synth [fm|psg [preset]|play [fm|psg] [preset]|midi <file> [fm|psg]]\n");
            return 2;
        }
    }
    fill_sine();
    fill_ninc();
    fill_song();
    fill_presets();
    if (mode == 2) {
        return run_midi(argv[2], engine);
    }
    load_preset(engine, preset);
    if (mode == 0) {
        return run_demo(engine);
    }
    return run_play(engine);
}
