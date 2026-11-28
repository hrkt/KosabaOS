/* smfplay.c -- Standard MIDI File (SMF) プレーヤー
 * (doc/karei-design.md の SMF プレーヤーの節)
 *
 *   run smfplay <file.mid>            イベントを時刻つきログに出す
 *   run smfplay <file.mid> file <p>   生 MIDI バイト列を時刻どおり
 *                                     ファイルへ書いていく
 *
 * 役割は「仮想 MIDI デバイス」: SMF を解析し、テンポマップに
 * 従って実時間に刻みながら (uptime=31 で読み、sleep_ms=34 で
 * 待つ)、生の MIDI バイト列を送り先へ出す。ファイル内の
 * デルタタイムは SMF 形式そのものなので当然使うが、送り先へ
 * デルタを渡して受け手に数えさせることはしない (本物の MIDI
 * の線にタイムスタンプは無く、バイト列が演奏時刻に届くのが
 * 実物。受け手の karei は届いたバイト列をそのまま解釈する)。
 *
 * 送り先は send_event() の関数境界の向こうにあり、本命の
 * 受け口 (OS 側 realtime-prep 段4 の L2 入力キュー + 注入
 * syscall、doc/realtime-audio-design.md) が来たら、そこへ
 * 差し替える。暫定経路はこのファイル (file モード) で、
 * 終了マーカーとして private SysEx F0 7D 00 F7 を最後に書く。
 *
 * 対応範囲: format 0 / 1、division は PPQ のみ (SMPTE は
 * 断る)、可変長デルタ、ランニングステータス、チャンネル
 * メッセージ全種 (送出はそのまま)、テンポ設定メタ FF 51、
 * トラック終了 FF 2F。SysEx と他のメタは読み飛ばす。
 * format 1 は全トラックを時刻順にマージする。テンポマップは
 * 同一 tick なら小さいトラック番号を優先する (コンダクター
 * トラック優先の通常の解釈)。
 *
 * IwaC の C サブセットで書く (karei.c と同じ作法): #include /
 * switch / 文字リテラルなし、整数のみ、ファイルスコープは
 * char 配列だけ (状態は rd32 / wr32 のバイト合成で読み書き)。
 * ファイルは丸ごと g_file に読む (上限 32768 バイト)。
 * イベントは最大 4096、テンポは最大 256、トラックは最大 32。
 *
 * 戻り値: 0 = 正常、1 = 開けない/作れない、2 = 引数不正、
 * 3 = 形式が対象外・壊れている
 */

#define SYS_OPEN 4
#define SYS_READ 5
#define SYS_CLOSE 6
#define SYS_CREATE 9
#define SYS_WRITE 10
#define SYS_UPTIME 31
#define SYS_SLEEP 34

#define MAXEV 4096
#define MAXTP 256
#define MAXTRK 32
#define FILEMAX 32768

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

/* 状態と表は全部ファイルスコープの char 配列に置く
 * (karei.c と同じ作法。スカラは rd32 / wr32 で読み書き)。
 * g_st のワード割り当て:
 *   0 = ファイル長   1 = イベント数   2 = テンポ数
 *   3 = division     4 = トラック数   5 = 出力 fd
 *   6 = 出力モード (0=log 1=file)      7 = 送出したイベント数
 *   9 = トラック終了メタを見た旗 (parse_meta -> parse_track) */
char g_file[32768];
char g_st[128];
char g_evt[16384];  /* イベントの絶対 tick (MAXEV x 4) */
char g_evd[16384];  /* イベントのバイト列 (MAXEV x 4:
                       status, data1, data2, 全長) */
char g_tpt[1024];   /* テンポイベントの tick (MAXTP x 4) */
char g_tpv[1024];   /* テンポ値 us/四分音符 (MAXTP x 4) */
char g_tpk[256];    /* テンポイベントのトラック番号 */
char g_trk[256];    /* トラック表 (32 x 2 ワード: 開始, 数) */
char g_trp[128];    /* 再生ポインタ (32 ワード) */
char g_ob[8];       /* 出力用の小バッファ */

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

int32 fb(int32 pos)
{
    return g_file[pos] & 255;
}

int32 u16be(int32 pos)
{
    return (fb(pos) << 8) | fb(pos + 1);
}

int32 u32be(int32 pos)
{
    return (fb(pos) << 24) | (fb(pos + 1) << 16) |
           (fb(pos + 2) << 8) | fb(pos + 3);
}

/* 可変長数 (デルタタイムと長さ)。値を *out に入れ、
 * 読み進めた先の位置を返す */
int32 rd_vlq(int32 pos, int32 *out)
{
    int32 v;
    int32 b;
    v = 0;
    b = fb(pos);
    pos = pos + 1;
    v = b & 127;
    while ((b & 128) != 0) {
        b = fb(pos);
        pos = pos + 1;
        v = (v << 7) | (b & 127);
    }
    *out = v;
    return pos;
}

/* チャンネルメッセージを 1 件、イベント表へ積む。
 * pos は先頭データバイトの位置。読み進めた先を返す */
int32 parse_chan(int32 pos, int32 tick, int32 status)
{
    int32 hi;
    int32 nd;
    int32 d1;
    int32 d2;
    int32 n;
    hi = status & 240;
    if (hi == 192 || hi == 208) {
        nd = 1;
    } else {
        nd = 2;
    }
    d1 = fb(pos);
    d2 = 0;
    if (nd == 2) {
        d2 = fb(pos + 1);
    }
    n = rd32(g_st, 1);
    if (n < MAXEV) {
        wr32(g_evt, n, tick);
        g_evd[n * 4] = (char)status;
        g_evd[n * 4 + 1] = (char)d1;
        g_evd[n * 4 + 2] = (char)d2;
        g_evd[n * 4 + 3] = (char)(nd + 1);
        wr32(g_st, 1, n + 1);
    }
    return pos + nd;
}

/* メタイベント。pos は FF の位置。テンポ (51) だけ拾い、
 * 終了 (2F) は g_st ワード 9 で呼び出し側へ伝える。
 * 他は読み飛ばす。読み進めた先を返す */
int32 parse_meta(int32 pos, int32 tick, int32 tidx)
{
    int32 type;
    int32 len;
    int32 n;
    type = fb(pos + 1);
    pos = rd_vlq(pos + 2, &len);
    wr32(g_st, 9, 0);
    if (type == 81 && len == 3) {
        n = rd32(g_st, 2);
        if (n < MAXTP) {
            wr32(g_tpt, n, tick);
            wr32(g_tpv, n, (fb(pos) << 16) |
                   (fb(pos + 1) << 8) | fb(pos + 2));
            g_tpk[n] = (char)tidx;
            wr32(g_st, 2, n + 1);
        }
    }
    if (type == 47) {
        wr32(g_st, 9, 1);
    }
    return pos + len;
}

/* 1 トラックを解析し、チャンネルイベントを表へ積む。
 * テンポも同時に拾う (parse_meta) */
void parse_track(int32 tidx, int32 pos, int32 end)
{
    int32 tick;
    int32 running;
    int32 start;
    tick = 0;
    running = 0;
    start = rd32(g_st, 1);
    wr32(g_trk, tidx * 2, start);
    wr32(g_trk, tidx * 2 + 1, 0);
    while (pos < end) {
        int32 delta;
        int32 b;
        pos = rd_vlq(pos, &delta);
        tick = tick + delta;
        b = fb(pos);
        if (b < 128) {
            if (running == 0) {
                pos = pos + 1;  /* 壊れたデータは 1 バイト捨てる */
            } else {
                pos = parse_chan(pos, tick, running);
            }
        } else if (b == 255) {
            pos = parse_meta(pos, tick, tidx);
            running = 0;
            if (rd32(g_st, 9) != 0) {
                pos = end;  /* トラック終了メタ */
            }
        } else if (b == 240 || b == 247) {
            int32 len;
            pos = rd_vlq(pos + 1, &len);
            pos = pos + len;  /* SysEx は読み飛ばす */
            running = 0;
        } else if (b < 240) {
            running = b;
            pos = parse_chan(pos + 1, tick, b);
        } else {
            /* システムコモン (F1..F6): データだけ読み飛ばす */
            running = 0;
            pos = pos + 1;
            if (b == 241 || b == 243) {
                pos = pos + 1;
            }
            if (b == 242) {
                pos = pos + 2;
            }
        }
    }
    wr32(g_trk, tidx * 2 + 1, rd32(g_st, 1) - start);
}

/* tick 時点で有効なテンポ (us/四分音符)。テンポイベントが
 * 無ければ既定 500000。同一 tick は小さいトラック番号が
 * 優先 (同じトラック内では後のものが勝つ) */
int32 tempo_at(int32 tick)
{
    int32 best;
    int32 bt;
    int32 bk;
    int32 i;
    int32 n;
    best = 500000;
    bt = 0 - 1;
    bk = 999;
    n = rd32(g_st, 2);
    i = 0;
    while (i < n) {
        int32 t;
        t = rd32(g_tpt, i);
        if (t <= tick) {
            if (t > bt || (t == bt && (g_tpk[i] & 255) <= bk)) {
                bt = t;
                bk = g_tpk[i] & 255;
                best = rd32(g_tpv, i);
            }
        }
        i = i + 1;
    }
    return best;
}

/* cur より後で一番近いテンポイベントの tick。無ければ -1 */
int32 next_tempo_tick(int32 cur)
{
    int32 best;
    int32 i;
    int32 n;
    best = -1;
    n = rd32(g_st, 2);
    i = 0;
    while (i < n) {
        int32 t;
        t = rd32(g_tpt, i);
        if (t > cur) {
            if (best < 0 || t < best) {
                best = t;
            }
        }
        i = i + 1;
    }
    return best;
}

/* 起動起点 start (uptime の ms) から target ms になるまで待つ。
 * 期限の計算は uptime の読み (1ms 分解能) で行い、待ちは
 * sleep_ms。最後の数 ms は 1ms ずつ刻んで寄せる */
void wait_until(int64 start, int64 target)
{
    while (1) {
        int64 now;
        int64 rem;
        now = syscall3(SYS_UPTIME, 0, 0, 0) - start;
        rem = target - now;
        if (rem <= 0) {
            return;
        }
        if (rem > 3) {
            syscall3(SYS_SLEEP, rem - 2, 0, 0);
        } else {
            syscall3(SYS_SLEEP, 1, 0, 0);
        }
    }
}

/* 送り先への 1 イベントの出力。送り先の差し替えはここ
 * (と main の開く/閉じる) だけ: 本命は OS 側段4 の入力
 * キューへの注入 syscall に替える。
 * log モード: 時刻つきで印字する (検証用)。
 * file モード: 生バイト列をそのまま書く (暫定経路)。
 * 失敗したら -1 */
int32 send_event(int32 eidx, int32 tms)
{
    int32 status;
    int32 d1;
    int32 d2;
    int32 len;
    status = g_evd[eidx * 4] & 255;
    d1 = g_evd[eidx * 4 + 1] & 255;
    d2 = g_evd[eidx * 4 + 2] & 255;
    len = g_evd[eidx * 4 + 3] & 255;
    if (rd32(g_st, 6) == 1) {
        int32 off;
        off = 0;
        while (off < len) {
            int64 w;
            w = syscall3(SYS_WRITE, rd32(g_st, 5),
                         g_evd + eidx * 4 + off, len - off);
            if (w <= 0) {
                return -1;
            }
            off = off + (int32)w;
        }
        return 0;
    }
    out_str("smfplay: ev t=");
    out_num(tms);
    if ((status & 240) == 144 && d2 != 0) {
        out_str(" noteon ");
        out_num(d1);
        putc(32);
        out_num(d2);
    } else if ((status & 240) == 128 ||
               ((status & 240) == 144 && d2 == 0)) {
        out_str(" noteoff ");
        out_num(d1);
        putc(32);
        out_num(d2);
    } else if ((status & 240) == 192) {
        out_str(" prog ");
        out_num(d1);
    } else if ((status & 240) == 176) {
        out_str(" cc ");
        out_num(d1);
        putc(32);
        out_num(d2);
    } else {
        out_str(" ch ");
        out_num(status);
        putc(32);
        out_num(d1);
        putc(32);
        out_num(d2);
    }
    putc(10);
    return 0;
}

/* 全トラックを時刻順にマージしながら実時間で送る。
 * 時刻の前進は区間ごとに、その区間の開始時点で有効な
 * テンポで換算する (テンポ変更が区間の途中にある場合は
 * そこで区切る)。送出はイベントの予定時刻が来てから。
 * summary を印字して、送れたら 0、書けなければ 1 */
int32 play()
{
    int32 ntrk;
    int32 division;
    int64 start;
    int64 cur_us;
    int32 cur_tick;
    int32 played;
    int32 t;
    ntrk = rd32(g_st, 4);
    division = rd32(g_st, 3);
    t = 0;
    while (t < ntrk) {
        wr32(g_trp, t, rd32(g_trk, t * 2));
        t = t + 1;
    }
    start = syscall3(SYS_UPTIME, 0, 0, 0);
    cur_us = 0;
    cur_tick = 0;
    played = 0;
    while (1) {
        int32 nev;
        int32 ntp;
        int32 seg;
        int32 tempo;
        nev = -1;
        t = 0;
        while (t < ntrk) {
            int32 p;
            p = rd32(g_trp, t);
            if (p < rd32(g_trk, t * 2) + rd32(g_trk, t * 2 + 1)) {
                int32 tk;
                tk = rd32(g_evt, p);
                if (nev < 0 || tk < nev) {
                    nev = tk;
                }
            }
            t = t + 1;
        }
        if (nev < 0) {
            break;
        }
        ntp = next_tempo_tick(cur_tick);
        seg = nev;
        if (ntp >= 0 && ntp < seg) {
            seg = ntp;
        }
        tempo = tempo_at(cur_tick);
        cur_us = cur_us + (int64)(seg - cur_tick) * tempo / division;
        cur_tick = seg;
        wait_until(start, cur_us / 1000);
        if (seg == nev) {
            int64 now;
            now = syscall3(SYS_UPTIME, 0, 0, 0) - start;
            t = 0;
            while (t < ntrk) {
                while (1) {
                    int32 p;
                    p = rd32(g_trp, t);
                    if (p >= rd32(g_trk, t * 2) +
                        rd32(g_trk, t * 2 + 1)) {
                        break;
                    }
                    if (rd32(g_evt, p) != cur_tick) {
                        break;
                    }
                    if (send_event(p, (int32)now) != 0) {
                        out_str("smfplay: write failed\n");
                        return 1;
                    }
                    played = played + 1;
                    wr32(g_trp, t, p + 1);
                }
                t = t + 1;
            }
        }
    }
    wr32(g_st, 7, played);
    out_str("smfplay: events=");
    out_num(played);
    out_str(" sched_ms=");
    out_num(cur_us / 1000);
    out_str(" actual_ms=");
    out_num(syscall3(SYS_UPTIME, 0, 0, 0) - start);
    putc(10);
    return 0;
}

int32 main(int32 argc, char **argv)
{
    char *path;
    char *outpath;
    int32 mode;
    int64 fd;
    int32 flen;
    int32 format;
    int32 ntrks;
    int32 division;
    int32 pos;
    int32 i;
    int32 rc;
    if (argc < 2) {
        out_str("usage: smfplay <file.mid> [file <path>]\n");
        return 2;
    }
    path = argv[1];
    outpath = 0;
    mode = 0;
    if (argc >= 3) {
        if (argc >= 4 && streq(argv[2], "file")) {
            mode = 1;
            outpath = argv[3];
        } else {
            out_str("usage: smfplay <file.mid> [file <path>]\n");
            return 2;
        }
    }
    fd = syscall3(SYS_OPEN, path, 0, 0);
    if (fd < 0) {
        out_str("smfplay: cannot open ");
        out_str(path);
        putc(10);
        return 1;
    }
    flen = 0;
    while (flen < FILEMAX) {
        int64 n;
        n = syscall3(SYS_READ, fd, g_file + flen, 4096);
        if (n < 0) {
            out_str("smfplay: read failed\n");
            syscall3(SYS_CLOSE, fd, 0, 0);
            return 1;
        }
        if (n == 0) {
            break;
        }
        flen = flen + (int32)n;
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (flen >= FILEMAX) {
        out_str("smfplay: file too large\n");
        return 3;
    }
    wr32(g_st, 0, flen);
    if (flen < 14 || fb(0) != 77 || fb(1) != 84 ||
        fb(2) != 104 || fb(3) != 100) {
        out_str("smfplay: not an SMF file\n");
        return 3;
    }
    format = u16be(8);
    ntrks = u16be(10);
    division = u16be(12);
    if ((division & 32768) != 0) {
        out_str("smfplay: SMPTE division not supported\n");
        return 3;
    }
    if (format != 0 && format != 1) {
        out_str("smfplay: unsupported format ");
        out_num(format);
        putc(10);
        return 3;
    }
    if (ntrks < 1 || ntrks > MAXTRK) {
        out_str("smfplay: bad track count ");
        out_num(ntrks);
        putc(10);
        return 3;
    }
    wr32(g_st, 3, division);
    wr32(g_st, 4, ntrks);
    pos = 8 + u32be(4);
    i = 0;
    while (i < ntrks) {
        int32 tlen;
        int32 tend;
        if (pos + 8 > flen || fb(pos) != 77 || fb(pos + 1) != 84 ||
            fb(pos + 2) != 114 || fb(pos + 3) != 107) {
            out_str("smfplay: bad track header\n");
            return 3;
        }
        tlen = u32be(pos + 4);
        tend = pos + 8 + tlen;
        if (tend > flen) {
            tend = flen;
        }
        parse_track(i, pos + 8, tend);
        pos = tend;
        i = i + 1;
    }
    out_str("smfplay: ");
    out_str(path);
    out_str(" format=");
    out_num(format);
    out_str(" tracks=");
    out_num(ntrks);
    out_str(" division=");
    out_num(division);
    putc(10);
    wr32(g_st, 6, mode);
    if (mode == 1) {
        fd = syscall3(SYS_CREATE, outpath, 0, 0);
        if (fd < 0) {
            out_str("smfplay: cannot create ");
            out_str(outpath);
            putc(10);
            return 1;
        }
        wr32(g_st, 5, (int32)fd);
    }
    rc = play();
    if (mode == 1) {
        if (rc == 0) {
            int32 off;
            g_ob[0] = (char)240;
            g_ob[1] = (char)125;
            g_ob[2] = 0;
            g_ob[3] = (char)247;
            off = 0;
            while (off < 4) {
                int64 w;
                w = syscall3(SYS_WRITE, rd32(g_st, 5),
                             g_ob + off, 4 - off);
                if (w <= 0) {
                    rc = 1;
                    break;
                }
                off = off + (int32)w;
            }
        }
        syscall3(SYS_CLOSE, rd32(g_st, 5), 0, 0);
    }
    if (rc != 0) {
        return rc;
    }
    out_str("smfplay: done\n");
    return 0;
}
