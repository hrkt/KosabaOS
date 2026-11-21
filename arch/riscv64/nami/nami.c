/* nami.c -- 音声ファイル再生プレーヤー (doc/nami-design.md)
 *
 *   run nami <ファイル>
 *
 * IwaC の C サブセットで書く (将来 MP3 デコーダと同一
 * ツールチェーンで繋ぐため。作法は Ikesu 段6 の
 * main_kosaba.c が前例)。入口は main(argc, argv) で、
 * 戻り値が終了ステータスになる (--kosaba の約束)。
 * 出力は組み込み putc、システムコールは組み込み
 * syscall3 で呼ぶ。
 *
 * 形式は先頭バイト (マジック) で振り分ける。WAV
 * (PCM・16bit・モノラル/ステレオ) と MP3 (MPEG-1
 * Layer III・モノラルのみ) のストリーミング再生:
 * ファイルをメモリに載せ切らず、塊で read → デコード
 * → audio_write (40〜42) を繰り返す。MP3 デコーダは
 * Ikesu プロジェクトで作った独自デコーダ (mp3dec.c と
 * 表) を 2026-11-21 に nami の一部として移したもので、
 * ビルドは表 → mp3dec.c → nami.c の連結 1 本で通す
 * (build-nami.sh)。ステレオ MP3 はデコードが実時間に
 * 届かないため当面は対象外 (status 3)。
 * 浮動小数点と switch は使わない (サブセットの外)。
 * 文字リテラルも使わず、文字コードを数で書く
 * (コーパスの作法)。
 *
 * 終了ステータス: 0 = 再生完了、1 = 開けない・読めない
 * などの失敗、2 = 引数不足、3 = 形式は分かるが対象外
 * (MP3・非 PCM・16bit 以外・レート非対応)、
 * 4 = 形式不明・破損。
 */

#define SYS_OPEN 4
#define SYS_READ 5
#define SYS_LSEEK 17
#define SYS_AUDIO_OPEN 40
#define SYS_AUDIO_WRITE 41
#define SYS_AUDIO_CLOSE 42

/* ---- 出力の道具 -------------------------------------- */

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

/* ---- バイト列からの数値読み (リトルエンディアン) ------ */

int32 rd16(char *p)
{
    return (p[0] & 255) | ((p[1] & 255) << 8);
}

int64 rd32(char *p)
{
    int64 v;
    v = (p[0] & 255);
    v = v | ((int64)(p[1] & 255) << 8);
    v = v | ((int64)(p[2] & 255) << 16);
    v = v | ((int64)(p[3] & 255) << 24);
    return v;
}

/* ---- 小さいバッファ付き読み ---------------------------
 * ヘッダ解析 (少しずつ読む) と data のストリーミング
 * (塊で読む) の両方をこの一本で賄う。読みは全部ここを
 * 通るので、バッファに先読みした分を取りこぼさない。
 * 読みの状態 (fd / 位置 / 長さ) は main のローカル配列
 * st[3] に持ち、ポインタで渡す。ファイルスコープの
 * スカラ変数にしないのは、nori のデータ配置が段の
 * 開始を 8 バイト境界に揃えず、実機で misaligned に
 * なるため (doc/nami-design.md の既知の問題)。
 * バッファはバイト単位でしか触らないのでグローバルで
 * よい。 */

char g_rbuf[1024];

/* 最大 n バイトまで dst へ入れ、入れた数を返す。
 * EOF では 0、エラーでは -1 (少しでも入っていたら
 * その数を返し、エラーは次の呼び出しで出る) */
int32 reader_read(int32 *st, char *dst, int32 n)
{
    int32 got;
    got = 0;
    while (got < n) {
        int64 r;
        int32 avail;
        int32 take;
        int32 i;
        if (st[1] >= st[2]) {
            r = syscall3(SYS_READ, st[0], g_rbuf, 1024);
            if (r < 0) {
                if (got > 0) {
                    return got;
                }
                return -1;
            }
            if (r == 0) {
                return got;
            }
            st[1] = 0;
            st[2] = (int32)r;
        }
        avail = st[2] - st[1];
        take = avail;
        if (n - got < avail) {
            take = n - got;
        }
        i = 0;
        while (i < take) {
            dst[got + i] = g_rbuf[st[1] + i];
            i = i + 1;
        }
        st[1] = st[1] + take;
        got = got + take;
    }
    return got;
}

/* ちょうど n バイト読む。読めなければ -1 */
int32 reader_exact(int32 *st, char *dst, int32 n)
{
    int32 got;
    got = 0;
    while (got < n) {
        int32 r;
        r = reader_read(st, dst + got, n - got);
        if (r <= 0) {
            return -1;
        }
        got = got + r;
    }
    return 0;
}

/* n バイト読み飛ばす。途中で尽きたら -1 */
int32 reader_skip(int32 *st, int64 n)
{
    char scratch[256];
    while (n > 0) {
        int32 want;
        int32 r;
        want = 256;
        if (n < 256) {
            want = (int32)n;
        }
        r = reader_read(st, scratch, want);
        if (r <= 0) {
            return -1;
        }
        n = n - r;
    }
    return 0;
}

/* ---- WAV の再生 ----------------------------------------
 * fmt で得た形式が data の再生条件になる。 */

char g_pbuf[4096];

int32 play_data(int32 *st, int64 data_size, int32 format,
                int32 channels, int32 rate, int32 block_align,
                int32 bits)
{
    int64 total_samples;
    int64 frames;
    int64 remaining;
    int64 r;

    if (format != 1) {
        out_str("nami: unsupported wav (not pcm)\n");
        return 3;
    }
    if (bits != 16) {
        out_str("nami: unsupported wav (not 16 bit)\n");
        return 3;
    }
    if (channels != 1 && channels != 2) {
        out_str("nami: unsupported wav (channels)\n");
        return 3;
    }
    if (block_align != 2 * channels) {
        out_str("nami: broken wav\n");
        return 4;
    }

    r = syscall3(SYS_AUDIO_OPEN, rate, channels, 0);
    if (r != 0) {
        out_str("nami: cannot open audio (rate ");
        out_num(rate);
        out_str(")\n");
        return 3;
    }

    total_samples = data_size / 2;
    frames = total_samples / channels;
    out_str("nami: ");
    out_num(rate);
    out_str(" Hz ");
    if (channels == 1) {
        out_str("mono");
    } else {
        out_str("stereo");
    }
    out_str(", ");
    out_num(total_samples);
    out_str(" samples (");
    if (rate > 0) {
        out_num(frames / rate);
    } else {
        out_num(0);
    }
    out_str(" s)\n");

    remaining = data_size;
    while (remaining > 0) {
        int32 want;
        int32 filled;
        int32 nsamp;
        int32 off;
        if (remaining > 4096) {
            want = 4096;
        } else {
            want = (int32)remaining;
        }
        want = want - (want % block_align);
        if (want == 0) {
            break;   /* 端数の 1 フレーム未満は捨てる */
        }
        filled = 0;
        while (filled < want) {
            int32 n;
            n = reader_read(st, g_pbuf + filled, want - filled);
            if (n < 0) {
                syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
                out_str("nami: read failed\n");
                return 1;
            }
            if (n == 0) {
                break;   /* 宣言より短い: ある分で終える */
            }
            filled = filled + n;
        }
        filled = filled - (filled % block_align);
        if (filled == 0) {
            break;
        }
        nsamp = filled / 2;
        off = 0;
        while (off < nsamp) {
            int64 w;
            w = syscall3(SYS_AUDIO_WRITE, g_pbuf + off * 2,
                         nsamp - off, 0);
            if (w <= 0) {
                syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
                out_str("nami: audio write failed\n");
                return 1;
            }
            off = off + (int32)w;
        }
        remaining = remaining - filled;
        if (filled < want) {
            break;   /* EOF に当たった */
        }
    }

    r = syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
    if (r != 0) {
        out_str("nami: audio close failed\n");
        return 1;
    }
    out_str("nami: done\n");
    return 0;
}

int32 play_wav(int32 *st)
{
    char hdr[8];
    char f[16];
    int32 format;
    int32 channels;
    int32 rate;
    int32 block_align;
    int32 bits;
    int32 have_fmt;
    format = 0;
    channels = 0;
    rate = 0;
    block_align = 0;
    bits = 0;
    have_fmt = 0;

    /* RIFF のサイズ (4 バイト、使わない) と "WAVE" */
    if (reader_exact(st, hdr, 8) != 0 ||
        (hdr[4] & 255) != 87 || (hdr[5] & 255) != 65 ||
        (hdr[6] & 255) != 86 || (hdr[7] & 255) != 69) {
        out_str("nami: unknown format\n");
        return 4;
    }

    while (1) {
        int64 size;
        if (reader_exact(st, hdr, 8) != 0) {
            out_str("nami: broken wav\n");
            return 4;
        }
        size = rd32(hdr + 4);
        if ((hdr[0] & 255) == 102 && (hdr[1] & 255) == 109 &&
            (hdr[2] & 255) == 116 && (hdr[3] & 255) == 32) {
            /* "fmt " */
            if (size < 16 || reader_exact(st, f, 16) != 0) {
                out_str("nami: broken wav\n");
                return 4;
            }
            format = rd16(f);
            channels = rd16(f + 2);
            rate = (int32)rd32(f + 4);
            block_align = rd16(f + 12);
            bits = rd16(f + 14);
            have_fmt = 1;
            if (reader_skip(st, (size - 16) + (size & 1)) != 0) {
                out_str("nami: broken wav\n");
                return 4;
            }
        } else if ((hdr[0] & 255) == 100 && (hdr[1] & 255) == 97 &&
                   (hdr[2] & 255) == 116 && (hdr[3] & 255) == 97) {
            /* "data" */
            if (have_fmt == 0) {
                out_str("nami: broken wav\n");
                return 4;
            }
            return play_data(st, size, format, channels, rate,
                             block_align, bits);
        } else {
            if (reader_skip(st, size + (size & 1)) != 0) {
                out_str("nami: broken wav\n");
                return 4;
            }
        }
    }
}

/* ---- MP3 の再生 ----------------------------------------
 * デコーダ (mp3dec.c) の状態と、読みの状態 st[3] の
 * 複写を 1 つの構造体に集めて持つ。ファイルスコープに
 * 裸のスカラを置かない作法 (nori の配置で misaligned に
 * なる) のためで、先頭の詰め物 g_mpad で構造体を 8
 * バイト境界に寄せる (大きさはビルド後の実測で調整。
 * Ikesu 側 main_twophase.c の MainState と同じ考え方)。 */

char g_mpad[9] = { 0 };

struct NamiMp3 {
    struct Mp3Dec dec;
    int32 st[3];
};

struct NamiMp3 g_mp3;

/* デコーダのバイト源 (mp3dec.c が呼ぶ)。nami の読み
 * (reader_read) をそのまま使う: 最大 len バイトまで
 * 入れて入れた数を返し、EOF では 0。読みエラーは
 * EOF として返す (デコーダ側で同期を失って終わる) */
int32 dec_read(char *dst, int32 len)
{
    int32 r;
    r = reader_read(g_mp3.st, dst, len);
    if (r < 0) {
        return 0;
    }
    return r;
}

int32 play_mp3(int32 *st)
{
    int32 opened;
    int64 r;

    opened = 0;
    /* main がマジックの 4 バイトを既に読んでいるので、
     * デコーダが先頭 (ID3 / 同期) から読めるよう巻き
     * 戻し、読みの状態も組み直す */
    r = syscall3(SYS_LSEEK, st[0], 0, 0);
    if (r < 0) {
        out_str("nami: mp3 seek failed\n");
        return 1;
    }
    g_mp3.st[0] = st[0];
    g_mp3.st[1] = 0;
    g_mp3.st[2] = 0;
    dec_init(&g_mp3.dec);

    while (1) {
        int32 rc;
        rc = dec_next_frame(&g_mp3.dec);
        if (rc == 0) {
            break;   /* EOF: 全フレームを読み切った */
        }
        if (rc < 0) {
            if (opened != 0) {
                syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
            }
            out_str("nami: mp3 decode error ");
            out_num(rc);
            putc(10);
            return 1;
        }
        dec_decode_frame(&g_mp3.dec);
        if (opened == 0) {
            int32 ch;
            int32 rate;
            ch = g_mp3.dec.frame.channels;
            rate = g_mp3.dec.frame.sample_rate;
            if (ch != 1) {
                out_str("nami: mp3 stereo is not supported yet\n");
                return 3;
            }
            r = syscall3(SYS_AUDIO_OPEN, rate, ch, 0);
            if (r != 0) {
                out_str("nami: cannot open audio (rate ");
                out_num(rate);
                out_str(")\n");
                return 3;
            }
            opened = 1;
            out_str("nami: ");
            out_num(rate);
            out_str(" Hz mono (mp3)\n");
        }
        {
            int32 nsamp;
            int32 off;
            nsamp = 1152 * g_mp3.dec.frame.channels;
            off = 0;
            while (off < nsamp) {
                int64 w;
                w = syscall3(SYS_AUDIO_WRITE,
                             (char *)g_mp3.dec.pcm + off * 2,
                             nsamp - off, 0);
                if (w <= 0) {
                    syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
                    out_str("nami: audio write failed\n");
                    return 1;
                }
                off = off + (int32)w;
            }
        }
    }

    if (opened == 0) {
        out_str("nami: mp3 has no frames\n");
        return 4;
    }
    r = syscall3(SYS_AUDIO_CLOSE, 0, 0, 0);
    if (r != 0) {
        out_str("nami: audio close failed\n");
        return 1;
    }
    out_str("nami: done\n");
    return 0;
}

int32 main(int32 argc, char **argv)
{
    char *path;
    char m[4];
    int32 st[3];
    int64 fd;

    if (argc < 2) {
        out_str("usage: nami <file>\n");
        return 2;
    }
    path = argv[1];
    out_str("nami: ");
    out_str(path);
    putc(10);

    fd = syscall3(SYS_OPEN, path, 0, 0);
    if (fd < 0) {
        out_str("nami: cannot open ");
        out_str(path);
        putc(10);
        return 1;
    }
    st[0] = (int32)fd;   /* 読みの状態: fd / 位置 / 長さ */
    st[1] = 0;
    st[2] = 0;

    if (reader_exact(st, m, 4) != 0) {
        out_str("nami: unknown format\n");
        return 4;
    }
    if ((m[0] & 255) == 73 && (m[1] & 255) == 68 &&
        (m[2] & 255) == 51) {
        return play_mp3(st);   /* "ID3" */
    }
    if ((m[0] & 255) == 255 && (m[1] & 224) == 224) {
        return play_mp3(st);   /* フレーム同期 */
    }
    if ((m[0] & 255) != 82 || (m[1] & 255) != 73 ||
        (m[2] & 255) != 70 || (m[3] & 255) != 70) {
        out_str("nami: unknown format\n");
        return 4;            /* "RIFF" でない */
    }
    return play_wav(st);
}
