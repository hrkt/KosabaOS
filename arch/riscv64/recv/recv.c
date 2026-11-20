/* recv.c -- シリアル経由のファイル受信 (doc/recv-design.md)
 *
 *   run recv.elf <NAME>
 *
 * コンソール UART から Kermit の芯だけのプロトコル
 * (stop-and-wait + CRC-16) でバイナリを受け取り、
 * /sd/<NAME> へ書く。Hatoba の file_push が送り側。
 *
 * IwaC の C サブセットで書く (nami と同じ流儀)。
 * 入口は main(argc, argv) で、戻り値が終了ステータス。
 * 出力は組み込み putc、システムコールは組み込み
 * syscall3。文字リテラルは使わず文字コードを数で書く。
 * ファイルスコープには char 配列だけ置く (nori の
 * データ配置が 8 バイト揃えでない件の回避)。
 *
 * プロトコルの要点:
 *   フレーム = SOH(1) + seq(1B) + len(2B LE) +
 *              payload(0..1024) + CRC-16(2B LE)
 *   終了フレームは先頭が ETX(3)、len = 0、seq は
 *   最後のデータブロックの次。ACK は返さず、close
 *   して DONE 行を印字する (送り側は NAK を受けたら
 *   終了フレームを再送する)
 *   CRC は CCITT-FALSE (poly 0x1021、初期値 0xFFFF、
 *   検算値: "123456789" → 0x29B1)、対象は seq + len
 *   + payload、フィールドは下位バイト先 (LE)
 *   ACK = 0x06 + seq、NAK = 0x15 + 期待 seq
 *   seq == 期待値 - 1 の重複フレームは書かずに ACK
 *
 * 終了ステータス: 0 = 完了、1 = ファイル側の失敗、
 * 2 = 引数 (名前) の不正。
 */

#define SYS_GETC 2
#define SYS_CLOSE 6
#define SYS_CREATE 9
#define SYS_WRITE 10

char g_buf[1024];   /* フレームの payload */
char g_path[64];    /* "/sd/" + 名前 + NUL */

void out_str(char *s)
{
    while (*s != 0) {
        putc(*s);
        s = s + 1;
    }
}

void out_dec(int64 v)
{
    char tmp[24];
    int64 n;
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

/* FNV-1a-64 の値を 16 桁 16 進 (小文字) で出す */
void out_fnv(int64 v)
{
    int64 i;
    int64 d;
    i = 0;
    while (i < 16) {
        d = (v >> 60) & 15;
        if (d < 10) {
            putc((int32)(48 + d));
        } else {
            putc((int32)(87 + d));
        }
        v = v << 4;
        i = i + 1;
    }
}

int64 getb(void)
{
    return syscall3(SYS_GETC, 0, 0, 0);
}

void send_ack(int64 seq)
{
    putc(6);
    putc((int32)seq);
}

void send_nak(int64 expect)
{
    putc(21);   /* 0x15 */
    putc((int32)expect);
}

/* CRC-16/CCITT-FALSE へ 1 バイト足す */
int64 crc_add(int64 crc, int64 b)
{
    int64 i;
    crc = crc ^ (b << 8);
    i = 0;
    while (i < 8) {
        if ((crc & 32768) != 0) {
            crc = (crc << 1) ^ 4129;    /* 0x1021 */
        } else {
            crc = crc << 1;
        }
        crc = crc & 65535;
        i = i + 1;
    }
    return crc;
}

int32 main(int32 argc, char **argv)
{
    char *name;
    int64 n;
    int64 i;
    int64 fd;
    int64 expect;
    int64 total;
    int64 fnv;
    int64 fnv_prime;
    int64 done;
    int64 start;
    int64 seq;
    int64 lenlo;
    int64 lenhi;
    int64 len;
    int64 crchi;
    int64 crclo;
    int64 crc;
    int64 w;
    int64 off;

    if (argc < 2) {
        out_str("usage: run recv.elf <NAME>\n");
        return 2;
    }
    name = argv[1];

    /* 名前の検査: 1..40 文字、'/' (47) と制御文字は不可 */
    n = 0;
    while (name[n] != 0) {
        if (name[n] == 47 || name[n] < 32) {
            out_str("recv: bad name\n");
            return 2;
        }
        n = n + 1;
    }
    if (n == 0 || n > 40) {
        out_str("recv: bad name\n");
        return 2;
    }

    /* パス "/sd/<NAME>" を組む (47='/', 115='s', 100='d') */
    g_path[0] = 47;
    g_path[1] = 115;
    g_path[2] = 100;
    g_path[3] = 47;
    i = 0;
    while (i < n) {
        g_path[4 + i] = name[i];
        i = i + 1;
    }
    g_path[4 + n] = 0;

    /* create は同名があれば空にする (上書きの割り切り) */
    fd = syscall3(SYS_CREATE, g_path, 0, 0);
    if (fd < 0) {
        out_str("recv: cannot create ");
        out_str(g_path);
        putc(10);
        return 1;
    }
    out_str("RECV1 READY\n");

    /* FNV の 64 ビット定数はリテラル上限 (32 ビット) を
     * 超えるので 16 ビットずつ組む (wavgen と同じ作法):
     * basis 0xCBF29CE484222325, prime 0x100000001B3 */
    fnv = 52210;
    fnv = (fnv << 16) | 40164;
    fnv = (fnv << 16) | 33826;
    fnv = (fnv << 16) | 8997;
    fnv_prime = ((int64)256 << 32) + 435;

    expect = 0;
    total = 0;
    done = 0;
    while (done == 0) {
        /* 先頭バイト (SOH=1 / ETX=3) を探す。フレームの
         * 途中からは読まないので、ゴミバイトは読み飛ばす */
        start = getb();
        while (start != 1 && start != 3) {
            start = getb();
        }
        seq = getb();
        lenlo = getb();
        lenhi = getb();
        len = lenlo + lenhi * 256;
        if (len > 1024) {
            /* 同期ずれ。送り側の再送で先頭から取り直す */
            continue;
        }
        i = 0;
        while (i < len) {
            g_buf[i] = (char)getb();
            i = i + 1;
        }
        /* CRC フィールドは下位バイト先 (LE) */
        crclo = getb();
        crchi = getb();

        crc = 65535;
        crc = crc_add(crc, seq);
        crc = crc_add(crc, lenlo);
        crc = crc_add(crc, lenhi);
        i = 0;
        while (i < len) {
            crc = crc_add(crc, (int64)(g_buf[i] & 255));
            i = i + 1;
        }
        if (crc != crchi * 256 + crclo) {
            send_nak(expect);
            continue;
        }

        if (start == 3) {
            /* 終了フレーム: ACK は返さない。seq が期待値と
             * 合うときだけ終わる (合わなければ NAK。送り
             * 側は終了フレームを再送してくる) */
            if (len != 0 || seq != expect) {
                send_nak(expect);
                continue;
            }
            done = 1;
        } else if (seq == expect) {
            off = 0;
            while (off < len) {
                w = syscall3(SYS_WRITE, fd, g_buf + off, len - off);
                if (w <= 0) {
                    out_str("\nRECV1 ERROR write\n");
                    syscall3(SYS_CLOSE, fd, 0, 0);
                    return 1;
                }
                off = off + w;
            }
            i = 0;
            while (i < len) {
                fnv = (fnv ^ (int64)(g_buf[i] & 255)) * fnv_prime;
                i = i + 1;
            }
            total = total + len;
            expect = (expect + 1) & 255;
            send_ack(seq);
        } else if (seq == ((expect + 255) & 255)) {
            /* 重複 (ACK 紛失で送り側が再送した)。書かずに ACK */
            send_ack(seq);
        } else {
            send_nak(expect);
        }
    }

    syscall3(SYS_CLOSE, fd, 0, 0);
    out_str("RECV1 DONE size=");
    out_dec(total);
    out_str(" fnv=");
    out_fnv(fnv);
    putc(10);
    return 0;
}
