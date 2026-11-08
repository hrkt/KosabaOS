/* wavgen.c -- テスト用 WAV を作る小さな生成プログラム
 *
 *   run wavgen.elf /sd/TEST.WAV
 *
 * nami (doc/nami-design.md) の実機確認用。nami と同じく
 * IwaC の C サブセットで書く。実機のカードへ WAV を
 * 置く手段が Nish の write (テキスト前提) では用意
 * できないので、サイン波 WAV を create / write で直接
 * 書く。形式は 44100Hz・モノラル 16bit・440Hz・2 秒
 * (88200 サンプル)。
 *
 * サイン波は固定小数点の漸化式で作る (IwaC に浮動
 * 小数点は無い): y[n] = 2cosθ・y[n-1] − y[n-2] を
 * Q16 (値を 65536 倍して整数で持つ) で回す。定数は
 * θ = 2π・440/44100 に対する 2cosθ と振幅 8000 の
 * sinθ をホストで計算したもの。88200 サンプル回して
 * 振幅のずれは 0.1% 程度 (テスト音源には十分)。
 *
 * 自分が書いた PCM の FNV-1a 64bit を計算して表示
 * する。nami で再生した後の dmesg の "audio: close"
 * 行の samples / fnv と突き合わせるための申告で、
 * 照合は装置の中で閉じる (ホスト生成の WAV と
 * サンプル列が一致する必要はない)。
 * 成功は 0、失敗は理由を出して 1、引数不足は 2。
 */

#define SYS_CLOSE 6
#define SYS_CREATE 9
#define SYS_WRITE 10

#define RATE 44100
#define TOTAL_SAMPLES 88200   /* 2 秒 */
#define CHUNK_SAMPLES 2048
#define COEF_Q16 130815       /* 2cosθ を Q16 で */
#define Y1_Q16 32845765       /* 8000・sinθ を Q16 で */

int64 g_fnv;
int64 g_fnv_prime;

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

void put_u16le(char *p, int32 v)
{
    p[0] = (char)(v & 255);
    p[1] = (char)((v >> 8) & 255);
}

void put_u32le(char *p, int64 v)
{
    p[0] = (char)(v & 255);
    p[1] = (char)((v >> 8) & 255);
    p[2] = (char)((v >> 16) & 255);
    p[3] = (char)((v >> 24) & 255);
}

/* fd へ全部書く (部分書き込みを繰り返す)。失敗は -1 */
int32 write_all(int32 fd, char *p, int32 n)
{
    int32 done;
    done = 0;
    while (done < n) {
        int64 w;
        w = syscall3(SYS_WRITE, fd, p + done, n - done);
        if (w <= 0) {
            return -1;
        }
        done = done + (int32)w;
    }
    return 0;
}

char g_sbuf[4096];   /* CHUNK_SAMPLES * 2。大きさは数で書く
                      * (IwaC は配列の大きさに式を書けない) */

int32 main(int32 argc, char **argv)
{
    char *path;
    char hdr[44];
    int64 fd;
    int64 y_prev2;
    int64 y_prev1;
    int32 sent;
    int32 i;

    if (argc < 2) {
        out_str("usage: wavgen.elf <path>\n");
        return 2;
    }
    path = argv[1];
    fd = syscall3(SYS_CREATE, path, 0, 0);
    if (fd < 0) {
        out_str("wavgen: cannot create ");
        out_str(path);
        putc(10);
        return 1;
    }

    /* FNV の 64 ビット定数は IwaC のリテラル上限
     * (32 ビット) を超えるので 16 ビットずつ組む
     * (Ikesu 段6 の main_kosaba.c と同じ作法):
     * basis 0xCBF29CE484222325, prime 0x100000001B3 */
    {
        int64 v;
        v = 52210;
        v = (v << 16) | 40164;
        v = (v << 16) | 33826;
        v = (v << 16) | 8997;
        g_fnv = v;
    }
    g_fnv_prime = ((int64)256 << 32) + 435;

    /* RIFF ヘッダ (44 バイト、PCM・モノラル 16bit)。
     * 文字コードは数で書く: R=82 I=73 F=70 W=87 A=65
     * V=86 E=69 f=102 m=109 t=116 d=100 a=97 */
    hdr[0] = 82; hdr[1] = 73; hdr[2] = 70; hdr[3] = 70;
    put_u32le(hdr + 4, 36 + TOTAL_SAMPLES * 2);
    hdr[8] = 87; hdr[9] = 65; hdr[10] = 86; hdr[11] = 69;
    hdr[12] = 102; hdr[13] = 109; hdr[14] = 116; hdr[15] = 32;
    put_u32le(hdr + 16, 16);        /* fmt の大きさ */
    put_u16le(hdr + 20, 1);         /* PCM */
    put_u16le(hdr + 22, 1);         /* モノラル */
    put_u32le(hdr + 24, RATE);
    put_u32le(hdr + 28, RATE * 2);  /* バイトレート */
    put_u16le(hdr + 32, 2);         /* ブロック境界 */
    put_u16le(hdr + 34, 16);        /* ビット深度 */
    hdr[36] = 100; hdr[37] = 97; hdr[38] = 116; hdr[39] = 97;
    put_u32le(hdr + 40, TOTAL_SAMPLES * 2);
    if (write_all((int32)fd, hdr, 44) != 0) {
        out_str("wavgen: write failed ");
        out_str(path);
        putc(10);
        return 1;
    }

    /* サイン波を塊で書きながら、書いたバイト列の
     * FNV を数える */
    y_prev2 = 0;
    y_prev1 = Y1_Q16;
    sent = 0;
    while (sent < TOTAL_SAMPLES) {
        int32 n;
        n = TOTAL_SAMPLES - sent;
        if (n > CHUNK_SAMPLES) {
            n = CHUNK_SAMPLES;
        }
        i = 0;
        while (i < n) {
            int64 y;
            int64 s;
            if (sent + i == 0) {
                y = 0;
            } else if (sent + i == 1) {
                y = y_prev1;
            } else {
                y = ((COEF_Q16 * y_prev1) >> 16) - y_prev2;
                y_prev2 = y_prev1;
                y_prev1 = y;
            }
            if (y >= 0) {
                s = (y + 32768) >> 16;
            } else {
                s = -(((-y) + 32768) >> 16);
            }
            g_sbuf[i * 2] = (char)(s & 255);
            g_sbuf[i * 2 + 1] = (char)((s >> 8) & 255);
            g_fnv = (g_fnv ^ (g_sbuf[i * 2] & 255)) * g_fnv_prime;
            g_fnv = (g_fnv ^ (g_sbuf[i * 2 + 1] & 255)) *
                    g_fnv_prime;
            i = i + 1;
        }
        if (write_all((int32)fd, g_sbuf, n * 2) != 0) {
            out_str("wavgen: write failed ");
            out_str(path);
            putc(10);
            return 1;
        }
        sent = sent + n;
    }

    syscall3(SYS_CLOSE, fd, 0, 0);
    out_str("wavgen: wrote ");
    out_str(path);
    putc(10);
    out_str("wavgen: samples=");
    out_num(TOTAL_SAMPLES);
    out_str(" fnv=");
    out_hex64(g_fnv);
    putc(10);
    out_str("wavgen: done\n");
    return 0;
}
