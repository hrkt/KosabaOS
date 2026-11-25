/* synth.c -- KosabaOS のシンセ音源アプリ (doc/synth-design.md 段1)
 *
 *   run synth          FM デモ曲 (引数なしの既定)
 *   run synth fm       FM デモ曲
 *   run synth psg      PSG デモ曲 (同じ曲)
 *   run synth play fm  シリアル鍵盤で FM を弾く
 *   run synth play psg シリアル鍵盤で PSG を弾く
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

/* ボイス状態 (g_vs の中): 1 ボイス 16 ワード、4 ボイス分。
 *   +0 active  +1 type (0=FM 1=矩形 2=ノイズ)  +2 note
 *   +3 搬送位相  +4 変調位相  +5 env (0..65535)
 *   +6 env stage (0=A 1=D 2=S 3=R)  +7 gate 残りサンプル
 *   +8 変調 env  +9 LFSR  +10 搬送増分  +11 変調増分
 * trig の引数 rr が次に使うボイス番号 (ラウンドロビン) */
void trig(int32 rr, int32 note, int32 gate, int32 type)
{
    int32 b;
    b = rr * 16;
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
    if (note >= 36 && note <= 96) {
        wr32(g_vs, b + 10, rd32(g_ninc, note - 36));
    } else {
        wr32(g_vs, b + 10, 0);
    }
    wr32(g_vs, b + 11, (rd32(g_vs, b + 10) * 7) / 2);
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
            outv = 0;
            if (stg == 0) {
                env = env + 600;
                if (env >= 65535) {
                    env = 65535;
                    stg = 1;
                }
            } else if (stg == 1) {
                env = env - 20;
                if (env <= 40000) {
                    env = 40000;
                    stg = 2;
                }
            } else if (stg == 3) {
                env = env - 80;
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
                    menv = menv - 200;
                } else {
                    menv = menv - 30;
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
                    depth = (20000 * menv) >> 16;
                    off = (mod * depth) >> 15;
                    pc = (pc + inc_c) & 65535;
                    car = sine_at((pc + off) & 65535);
                    outv = (car * env) >> 16;
                    sum = sum + (outv >> 2);
                } else if (typ == 1) {
                    int32 sq;
                    pc = (pc + inc_c) & 65535;
                    if (pc < 32768) {
                        sq = 32767;
                    } else {
                        sq = 0 - 32767;
                    }
                    outv = (sq * env) >> 16;
                    sum = sum + (outv >> 2);
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
                    sum = sum + (outv >> 3);
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
    step = 0;
    while (step < NSTEPS) {
        int32 remain;
        if (g_mel[step] != 0) {
            trig(rr, g_mel[step] & 255, STEP_LEN, engine);
            rr = (rr + 1) & 3;
        }
        if ((step & 1) == 0 && g_bas[step] != 0) {
            trig(rr, g_bas[step] & 255, STEP_LEN * 2, engine);
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
    out_str("synth: play mode. keys=z..m/q..i, o/p=octave, ESC=end\n");
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
        } else if (c > 0) {
            int32 semi;
            semi = key_semi((int32)c);
            if (semi >= 0) {
                trig(rr, base + semi, 16000, engine);
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

int32 main(int32 argc, char **argv)
{
    int32 mode;
    int32 engine;
    mode = 0;
    engine = 0;
    if (argc >= 2) {
        if (streq(argv[1], "fm")) {
            mode = 0;
            engine = 0;
        } else if (streq(argv[1], "psg")) {
            mode = 0;
            engine = 1;
        } else if (streq(argv[1], "play")) {
            mode = 1;
            engine = 0;
            if (argc >= 3) {
                if (streq(argv[2], "psg")) {
                    engine = 1;
                } else if (streq(argv[2], "fm")) {
                    engine = 0;
                } else {
                    out_str("usage: synth [fm|psg|play [fm|psg]]\n");
                    return 2;
                }
            }
        } else {
            out_str("usage: synth [fm|psg|play [fm|psg]]\n");
            return 2;
        }
    }
    fill_sine();
    fill_ninc();
    fill_song();
    if (mode == 0) {
        return run_demo(engine);
    }
    return run_play(engine);
}
