/* mp3dec.c -- 仕様から書く MPEG-1 Layer III デコーダ
 *
 * 設計 (doc/native-decoder-plan.md):
 * - IwaC のサブセット内で書き、同時に通常の C (C99) としても
 *   正しくする。ホストではこのファイルとハーネスを連結して
 *   gcc でビルドし、IwaC では同じ連結を iwac に渡す
 * - 固定小数点のみ (float / double は使わない)
 * - 状態は struct Mp3Dec に集め、ポインタで渡す
 * - バイト列は char の配列で扱い、読むときに & 0xFF する
 *   (IwaC に uint8 はなく、char の読みはゼロ拡張される)
 * - switch / enum / do-while / sizeof(式) / 引数付きマクロ /
 *   関数ポインタ / 共用体 / goto / 再帰は使わない
 *
 * このファイルは段5 まで: バイト源、フレーム同期、ヘッダと
 * サイド情報の解釈、ビットリザーバ、スケールファクタと
 * ハフマン復号、再量子化 (Q18) とステレオ処理、並べ替え、
 * エイリアス低減、IMDCT と符号反転、ポリフェーズ合成。
 */

/* ハーネス側が用意するバイト源: dst へ最大 len バイト読み、
 * 読めた数を返す (0 = EOF)。逐次読み専用 */
int32 dec_read(char *dst, int32 len);

/* ---- 定数 ---------------------------------------------------- */

#define SRCBUF_SIZE 4096   /* バイト源のバッファ */
#define SI_MAX 32          /* サイド情報の最大バイト数 (2ch) */
#define RES_SIZE 8192      /* ビットリザーバ (511 + 最大フレーム分) */
#define TREE_NODES 16384   /* ハフマン木のノード池 (ペア表用) */
#define QTREE_NODES 512    /* ハフマン木のノード池 (quad 表用) */
#define TREE_EMPTY 999999  /* 木の子が無い印 */
/* 多ビット先行表の幅は 9 ビット、項数は 512。IwaC の
 * #define 上限 (16、定義済みマクロ込み) に触れるため、
 * ここでは定数として直接書く */

/* MPEG-1 Layer III のビットレート表 (kbps、index 1..14) */
static int32 br_table[15] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320
};
/* MPEG-1 のサンプルレート表 (index 0..2) */
static int32 sr_table[3] = { 44100, 48000, 32000 };

/* ---- 構造体 -------------------------------------------------- */

/* サイド情報 (グラニュール×チャンネルの配列はフラットに置く。
 * 添字は (gr * 2 + ch)、table_select は ((gr * 2 + ch) * 3 + r)、
 * subblock_gain は ((gr * 2 + ch) * 3 + w)、scfsi は (ch * 4 + b) */
struct Mp3Side {
    int32 main_data_begin;
    int32 scfsi[8];
    int32 part2_3_length[4];       /* グラニュール×チャンネル毎 */
    int32 big_values[4];
    int32 global_gain[4];
    int32 scalefac_compress[4];
    int32 window_switching[4];
    int32 block_type[4];           /* 0=long 1=start 2=short 3=stop */
    int32 mixed_block[4];
    int32 table_select[12];
    int32 subblock_gain[12];
    int32 region0_count[4];
    int32 region1_count[4];
    int32 preflag[4];
    int32 scalefac_scale[4];
    int32 count1table[4];
};

/* 1 フレーム分の解釈結果 */
struct Mp3Frame {
    int64 offset;        /* ファイル先頭からのバイト位置 */
    int32 frame_len;     /* ヘッダ込みの総バイト数 */
    int32 bitrate_kbps;
    int32 sample_rate;
    int32 sr_idx;        /* 0=44.1k 1=48k 2=32k (表の行) */
    int32 padding;
    int32 mode;          /* 0=stereo 1=joint 2=dual 3=mono */
    int32 mode_ext;
    int32 channels;
    int32 has_crc;
    int32 is_xing;       /* Xing / Info ヘッダフレーム */
    int32 main_bytes;    /* メインデータのバイト数 */
    struct Mp3Side side;
};

/* デコーダの状態 (段が進むとここに履歴が増える) */
struct Mp3Dec {
    /* バイト源 */
    char srcbuf[SRCBUF_SIZE];
    int32 src_pos;       /* 次に読む位置 */
    int32 src_len;       /* バッファ内の有効バイト数 */
    int32 src_eof;       /* 1 = もう読めない */
    int64 consumed;      /* これまでに消費したバイト数 */
    /* サイド情報のビット読み */
    char sibuf[SI_MAX];
    int32 si_bitpos;
    /* 直近フレーム */
    struct Mp3Frame frame;
    int32 frames_seen;
    /* ビットリザーバの帳簿 (ビット単位) */
    int64 appended_bits;   /* 受け取ったメインデータの累計 */
    int64 prev_end_bits;   /* 前フレームのデータ消費末尾 */
    int32 max_gap_bits;    /* 消費末尾→次データ先頭の最大間隔 */
    int32 err;             /* 0 以外 = 異常の種別 */
    /* ビットリザーバ本体: 先頭は常に「現在のフレームの
     * データ先頭」のバイトに合わせてある */
    char reservoir[RES_SIZE];
    int32 res_len;         /* 有効バイト数 */
    int32 br_pos;          /* ビット読み位置 (リザーバ内) */
    /* ハフマン木 (dec_init で構築)。子が負なら葉で、
     * -(1 + シンボル) を入れる。シンボルは (x<<4)|y、
     * quad では値4個を4ビットに詰めたもの */
    int32 tree_ch0[TREE_NODES];
    int32 tree_ch1[TREE_NODES];
    int32 tree_nodes;
    int32 tree_root[32];
    int32 qtree_ch0[QTREE_NODES];
    int32 qtree_ch1[QTREE_NODES];
    int32 qtree_nodes;
    int32 qtree_root[2];
    /* 多ビット先行表 (dec_init で構築): 次の 9 ビットを
     * 引いて (len << 8) | sym が返れば符号語は len ビットで
     * 確定する。-1 の項は符号語が 9 ビットより長く、
     * 木を辿る従来経路に落とす。表ごとに 512 項、
     * 共有表 (17..23 は 16、25..31 は 24) は中身を複製する */
    int32 huff_fast[16384];   /* 32 表 × 512 項 */
    int32 qhuff_fast[1024];   /* 2 表 × 512 項 */
    /* 復号結果: is は (gr*2+ch)*576、scf は (gr*2+ch)*40 */
    int32 is[2304];
    int32 scf[160];
    /* ist は scf と並行に保つインテンシティ用の生値
     * (右チャンネルの分を段3 で使う)。xr は再量子化後の
     * スペクトル (Q18)、is と同じ並び */
    int32 ist[160];
    int32 xr[2304];
    /* IMDCT のオーバーラップ状態 (ch 毎に 9*32、Q18) と
     * 並べ替え用スクラッチ。xr は段4 の変換でサブバンド
     * サンプルに置き換わる */
    int32 overlap[576];
    int32 scratch[576];
    int32 mdct_co[9];
    int32 mdct_si[9];
    int32 mdct_tmp[18];
    /* 段チェック用: 何段まで進めるか (通常は 99 = 全部) */
    int32 stage_limit;
    /* ポリフェーズ合成の V 履歴 (ch 毎に 16 ブロック×64
     * 行、Q18 のリング) と書き込み位置 */
    int32 v_hist[2048];
    int32 v_head;
    /* フレームの PCM 出力 (int16、インターリーブ) */
    int16 pcm[2304];
};

/* ---- バイト源 ------------------------------------------------ */

/* 1 バイト読む (0..255)。EOF は -1 */
int32 src_getbyte(struct Mp3Dec *d)
{
    int32 v;
    if (d->src_pos >= d->src_len) {
        if (d->src_eof) {
            return -1;
        }
        d->src_len = dec_read(d->srcbuf, SRCBUF_SIZE);
        d->src_pos = 0;
        if (d->src_len <= 0) {
            d->src_eof = 1;
            return -1;
        }
    }
    v = d->srcbuf[d->src_pos] & 0xFF;
    d->src_pos = d->src_pos + 1;
    d->consumed = d->consumed + 1;
    return v;
}

/* n バイト読み飛ばす。途中で EOF なら -1 */
int32 src_skip(struct Mp3Dec *d, int32 n)
{
    int32 i;
    int32 v;
    i = 0;
    while (i < n) {
        v = src_getbyte(d);
        if (v < 0) {
            return -1;
        }
        i = i + 1;
    }
    return 0;
}

/* ---- サイド情報のビット読み (MSB ファースト) ------------------ */

int32 si_bits(struct Mp3Dec *d, int32 n)
{
    int32 v;
    int32 i;
    int32 byte;
    int32 bit;
    v = 0;
    i = 0;
    while (i < n) {
        byte = d->sibuf[d->si_bitpos >> 3] & 0xFF;
        bit = (byte >> (7 - (d->si_bitpos & 7))) & 1;
        v = (v << 1) | bit;
        d->si_bitpos = d->si_bitpos + 1;
        i = i + 1;
    }
    return v;
}

/* ---- ビットリーダ (リザーバ上、MSB ファースト) -----------------
 * 範囲外は 0 を返す (壊れた入力でも暴走しないため) */

int32 br_bits(struct Mp3Dec *d, int32 n)
{
    int32 v;
    int32 i;
    int32 byte;
    int32 bit;
    v = 0;
    i = 0;
    while (i < n) {
        if ((d->br_pos >> 3) < d->res_len) {
            byte = d->reservoir[d->br_pos >> 3] & 0xFF;
            bit = (byte >> (7 - (d->br_pos & 7))) & 1;
        } else {
            bit = 0;
        }
        v = (v << 1) | bit;
        d->br_pos = d->br_pos + 1;
        i = i + 1;
    }
    return v;
}

int32 br_bit(struct Mp3Dec *d)
{
    return br_bits(d, 1);
}

/* br_pos から n ビットを消費せずに読む (先行読み)。
 * 範囲外は br_bits と同じく 0。状態を持たないので、
 * br_pos を直接書き換える箇所があっても壊れない */
int32 br_peek(struct Mp3Dec *d, int32 n)
{
    int32 bytepos;
    int32 bitpos;
    int32 w;
    int32 i;
    int32 b;
    bytepos = d->br_pos >> 3;
    bitpos = d->br_pos & 7;
    w = 0;
    i = 0;
    while (i < 4) {
        b = 0;
        if (bytepos + i < d->res_len) {
            b = d->reservoir[bytepos + i] & 0xFF;
        }
        w = (w << 8) | b;
        i = i + 1;
    }
    return (w >> (32 - bitpos - n)) & ((1 << n) - 1);
}

/* ---- ハフマン木の構築 -------------------------------------------
 * 正規形の表 (hufftab.c) から二分木を作る。ノード池は
 * ペア表と quad 表で分ける。戻り値は根のノード番号 */

int32 tree_new_node(struct Mp3Dec *d, int32 quad)
{
    int32 idx;
    if (quad) {
        idx = d->qtree_nodes;
        d->qtree_nodes = d->qtree_nodes + 1;
        d->qtree_ch0[idx] = TREE_EMPTY;
        d->qtree_ch1[idx] = TREE_EMPTY;
    } else {
        idx = d->tree_nodes;
        d->tree_nodes = d->tree_nodes + 1;
        d->tree_ch0[idx] = TREE_EMPTY;
        d->tree_ch1[idx] = TREE_EMPTY;
    }
    return idx;
}

int32 tree_get_child(struct Mp3Dec *d, int32 quad, int32 node,
    int32 bit)
{
    if (quad) {
        if (bit) {
            return d->qtree_ch1[node];
        }
        return d->qtree_ch0[node];
    }
    if (bit) {
        return d->tree_ch1[node];
    }
    return d->tree_ch0[node];
}

void tree_set_child(struct Mp3Dec *d, int32 quad, int32 node,
    int32 bit, int32 v)
{
    if (quad) {
        if (bit) {
            d->qtree_ch1[node] = v;
        } else {
            d->qtree_ch0[node] = v;
        }
    } else {
        if (bit) {
            d->tree_ch1[node] = v;
        } else {
            d->tree_ch0[node] = v;
        }
    }
}

/* 符号表 (off から cnt 個、詰め形) を木に挿入する */
int32 tree_build(struct Mp3Dec *d, int32 quad, int32 off,
    int32 cnt, int32 is_quad_tab)
{
    int32 root;
    int32 i;
    int32 node;
    int32 len;
    int32 code;
    int32 sym;
    int32 depth;
    int32 bit;
    int32 w;
    root = tree_new_node(d, quad);
    i = 0;
    while (i < cnt) {
        if (is_quad_tab) {
            w = huff_quad_all[off + i];
        } else {
            w = huff_pair_all[off + i];
        }
        len = (w >> 27) & 31;
        code = (w >> 8) & 0x7FFFF;
        sym = (w >> 4) & 15;
        if (is_quad_tab == 0) {
            sym = (sym << 4) | (w & 15);
        }
        node = root;
        depth = 0;
        while (depth < len - 1) {
            bit = (code >> (len - 1 - depth)) & 1;
            if (tree_get_child(d, quad, node, bit) == TREE_EMPTY) {
                tree_set_child(d, quad, node, bit,
                    tree_new_node(d, quad));
            }
            node = tree_get_child(d, quad, node, bit);
            depth = depth + 1;
        }
        bit = (code >> 0) & 1;
        tree_set_child(d, quad, node, bit, -(1 + sym));
        i = i + 1;
    }
    return root;
}

void build_trees(struct Mp3Dec *d)
{
    int32 t;
    int32 src;
    d->tree_nodes = 0;
    d->qtree_nodes = 0;
    t = 0;
    while (t < 32) {
        d->tree_root[t] = -1;
        t = t + 1;
    }
    /* 17..23 は 16 と、25..31 は 24 と符号表を共有する */
    t = 0;
    while (t < 32) {
        if (huff_pair_cnt[t] > 0) {
            src = t;
            if (t >= 17 && t <= 23) {
                src = 16;
            }
            if (t >= 25 && t <= 31) {
                src = 24;
            }
            if (d->tree_root[src] < 0) {
                d->tree_root[src] = tree_build(d, 0,
                    huff_pair_off[src], huff_pair_cnt[src], 0);
            }
            d->tree_root[t] = d->tree_root[src];
        }
        t = t + 1;
    }
    d->qtree_root[0] = tree_build(d, 1, huff_quad_off[0],
        huff_quad_cnt[0], 1);
    d->qtree_root[1] = tree_build(d, 1, huff_quad_off[1],
        huff_quad_cnt[1], 1);
}

/* 多ビット先行表の構築: 長さ 9 以下の符号語は、
 * その符号語を上位に置いた全ての索引へ (len << 8) | sym を
 * 散らす。長い符号語の索引は -1 のまま (木に落とす)。
 * 接頭符号なので、短い符号語と長い符号語が同じ索引を
 * 取り合うことはない */
void build_fast(struct Mp3Dec *d)
{
    int32 t;
    int32 src;
    int32 i;
    int32 j;
    int32 base;
    int32 w;
    int32 len;
    int32 code;
    int32 sym;
    int32 start;
    int32 fill;
    i = 0;
    while (i < 32 * 512) {
        d->huff_fast[i] = -1;
        i = i + 1;
    }
    i = 0;
    while (i < 2 * 512) {
        d->qhuff_fast[i] = -1;
        i = i + 1;
    }
    t = 0;
    while (t < 32) {
        if (huff_pair_cnt[t] > 0) {
            src = t;
            if (t >= 17 && t <= 23) {
                src = 16;
            }
            if (t >= 25 && t <= 31) {
                src = 24;
            }
            base = t * 512;
            i = 0;
            while (i < huff_pair_cnt[src]) {
                w = huff_pair_all[huff_pair_off[src] + i];
                len = (w >> 27) & 31;
                code = (w >> 8) & 0x7FFFF;
                sym = (((w >> 4) & 15) << 4) | (w & 15);
                if (len <= 9) {
                    start = code << (9 - len);
                    fill = 1 << (9 - len);
                    j = 0;
                    while (j < fill) {
                        d->huff_fast[base + start + j] =
                            (len << 8) | sym;
                        j = j + 1;
                    }
                }
                i = i + 1;
            }
        }
        t = t + 1;
    }
    t = 0;
    while (t < 2) {
        base = t * 512;
        i = 0;
        while (i < huff_quad_cnt[t]) {
            w = huff_quad_all[huff_quad_off[t] + i];
            len = (w >> 27) & 31;
            code = (w >> 8) & 0x7FFFF;
            sym = (w >> 4) & 15;
            if (len <= 9) {
                start = code << (9 - len);
                fill = 1 << (9 - len);
                j = 0;
                while (j < fill) {
                    d->qhuff_fast[base + start + j] =
                        (len << 8) | sym;
                    j = j + 1;
                }
            }
            i = i + 1;
        }
        t = t + 1;
    }
}

/* ---- 初期化 --------------------------------------------------- */

void dec_init(struct Mp3Dec *d)
{
    d->src_pos = 0;
    d->src_len = 0;
    d->src_eof = 0;
    d->consumed = 0;
    d->si_bitpos = 0;
    d->frames_seen = 0;
    d->appended_bits = 0;
    d->prev_end_bits = 0;
    d->max_gap_bits = 0;
    d->err = 0;
    d->res_len = 0;
    d->br_pos = 0;
    d->stage_limit = 99;
    d->v_head = 0;
    {
        int32 i;
        i = 0;
        while (i < 576) {
            d->overlap[i] = 0;
            i = i + 1;
        }
        i = 0;
        while (i < 2048) {
            d->v_hist[i] = 0;
            i = i + 1;
        }
    }
    build_trees(d);
    build_fast(d);
}

/* ---- フレーム同期 --------------------------------------------- */

/* 妥当な Layer III ヘッダか (w は 4 バイトを並べた作業配列) */
int32 header_ok(int32 *w)
{
    int32 bri;
    int32 sri;
    if (w[0] != 0xFF) {
        return 0;
    }
    if ((w[1] & 0xE0) != 0xE0) {
        return 0;                    /* 同期語の下位 3 bit */
    }
    if (((w[1] >> 3) & 3) != 3) {
        return 0;                    /* MPEG-1 のみ */
    }
    if (((w[1] >> 1) & 3) != 1) {
        return 0;                    /* Layer III のみ */
    }
    bri = (w[2] >> 4) & 15;
    if (bri < 1 || bri > 14) {
        return 0;
    }
    sri = (w[2] >> 2) & 3;
    if (sri > 2) {
        return 0;
    }
    return 1;
}

/* ---- サイド情報の解釈 ------------------------------------------ */

void parse_side(struct Mp3Dec *d, int32 channels)
{
    struct Mp3Side *s;
    int32 gr;
    int32 ch;
    int32 i;
    int32 gc;
    s = &d->frame.side;
    d->si_bitpos = 0;
    i = 0;
    while (i < 4) {
        s->part2_3_length[i] = 0;   /* mono では使わない分を 0 に */
        i = i + 1;
    }
    s->main_data_begin = si_bits(d, 9);
    if (channels == 1) {
        si_bits(d, 5);               /* private bits */
    } else {
        /* 2ch では main_data_begin の後ろから granule まで
         * 11 ビットある (private 2 + scfsi 8 + 予備 1)。
         * 仕様書の素朴な合計 (255 ビット) より 1 ビット多く、
         * 実績ある実装 (minimp3 の読み方) と突き合わせて
         * 「scfsi は最後の 8 ビット」と確定した。先頭 3 ビットは
         * 読み飛ばす */
        si_bits(d, 3);
    }
    ch = 0;
    while (ch < channels) {
        i = 0;
        while (i < 4) {
            s->scfsi[ch * 4 + i] = si_bits(d, 1);
            i = i + 1;
        }
        ch = ch + 1;
    }
    gr = 0;
    while (gr < 2) {
        ch = 0;
        while (ch < channels) {
            gc = gr * 2 + ch;
            /* part2_3_length は (gr, ch) 毎にあり、その ch の
             * スケールファクタ+ハフマンデータのビット数 */
            s->part2_3_length[gc] = si_bits(d, 12);
            s->big_values[gc] = si_bits(d, 9);
            s->global_gain[gc] = si_bits(d, 8);
            s->scalefac_compress[gc] = si_bits(d, 4);
            s->window_switching[gc] = si_bits(d, 1);
            if (s->window_switching[gc]) {
                s->block_type[gc] = si_bits(d, 2);
                s->mixed_block[gc] = si_bits(d, 1);
                i = 0;
                while (i < 2) {
                    s->table_select[gc * 3 + i] = si_bits(d, 5);
                    i = i + 1;
                }
                s->table_select[gc * 3 + 2] = 0;
                i = 0;
                while (i < 3) {
                    s->subblock_gain[gc * 3 + i] = si_bits(d, 3);
                    i = i + 1;
                }
                s->region0_count[gc] = 0;
                s->region1_count[gc] = 0;
            } else {
                s->block_type[gc] = 0;
                s->mixed_block[gc] = 0;
                i = 0;
                while (i < 3) {
                    s->table_select[gc * 3 + i] = si_bits(d, 5);
                    i = i + 1;
                }
                s->region0_count[gc] = si_bits(d, 4);
                s->region1_count[gc] = si_bits(d, 3);
                i = 0;
                while (i < 3) {
                    s->subblock_gain[gc * 3 + i] = 0;
                    i = i + 1;
                }
            }
            s->preflag[gc] = si_bits(d, 1);
            s->scalefac_scale[gc] = si_bits(d, 1);
            s->count1table[gc] = si_bits(d, 1);
            ch = ch + 1;
        }
        gr = gr + 1;
    }
}

/* ---- フレーム反復子 ---------------------------------------------
 * 戻り値: 1 = フレームを1つ解釈した (d->frame に結果)
 *         0 = きれいに EOF (末尾の余りは d->consumed で分かる)
 *        -1 = フレームの途中で EOF (切断)
 *        -2 = main_data_begin が受け取り済みを超えた
 *        -3 = part2_3_length が未来のデータを要求した
 *        -4 = 前フレームの消費末尾とデータ先頭が重なった
 */
int32 dec_next_frame(struct Mp3Dec *d)
{
    struct Mp3Frame *f;
    int32 w[4];
    int32 b;
    int32 i;
    int32 bri;
    int32 sri;
    int32 side_len;
    int64 appended_before;
    int64 data_start;
    int64 consumed_end;
    int64 gap;

    f = &d->frame;

    /* 同期スキャン: 4 バイトの窓を1バイトずつずらす */
    w[0] = 0; w[1] = 0; w[2] = 0; w[3] = 0;
    i = 0;
    while (1) {
        b = src_getbyte(d);
        if (b < 0) {
            return 0;
        }
        w[0] = w[1]; w[1] = w[2]; w[2] = w[3]; w[3] = b;
        i = i + 1;
        if (i >= 4 && header_ok(w)) {
            break;
        }
    }
    f->offset = d->consumed - 4;

    /* ヘッダ解釈 */
    bri = (w[2] >> 4) & 15;
    sri = (w[2] >> 2) & 3;
    f->bitrate_kbps = br_table[bri];
    f->sample_rate = sr_table[sri];
    f->sr_idx = sri;
    f->padding = (w[2] >> 1) & 1;
    f->mode = (w[3] >> 6) & 3;
    f->mode_ext = (w[3] >> 4) & 3;
    f->has_crc = 1 - (w[1] & 1);
    if (f->mode == 3) {
        f->channels = 1;
    } else {
        f->channels = 2;
    }
    f->frame_len = (144 * f->bitrate_kbps * 1000) / f->sample_rate
        + f->padding;

    if (f->has_crc) {
        if (src_skip(d, 2) < 0) {
            return -1;
        }
    }

    /* サイド情報を読み、解釈する */
    if (f->channels == 1) {
        side_len = 17;
    } else {
        side_len = 32;
    }
    i = 0;
    while (i < side_len) {
        b = src_getbyte(d);
        if (b < 0) {
            return -1;
        }
        d->sibuf[i] = b;
        i = i + 1;
    }
    parse_side(d, f->channels);

    f->main_bytes = f->frame_len - 4 - side_len;
    if (f->has_crc) {
        f->main_bytes = f->main_bytes - 2;
    }

    /* メインデータをビットリザーバへ追加する。
     * まず main_data_begin より手前 (前フレームまでに
     * 消費し終えた分) を捨てて原点をこのフレームの
     * データ先頭に合わせ、続けてこのフレーム分を読む */
    {
        int32 discard;
        int32 slot_start;
        int32 v;
        discard = d->res_len - f->side.main_data_begin;
        if (discard < 0) {
            discard = d->res_len;   /* 異常時は帳簿側で -2 になる */
        }
        if (discard > 0) {
            i = 0;
            while (i < d->res_len - discard) {
                d->reservoir[i] = d->reservoir[i + discard];
                i = i + 1;
            }
            d->res_len = d->res_len - discard;
        }
        slot_start = d->res_len;
        i = 0;
        while (i < f->main_bytes) {
            v = src_getbyte(d);
            if (v < 0) {
                return -1;
            }
            d->reservoir[d->res_len] = v;
            d->res_len = d->res_len + 1;
            i = i + 1;
        }

        /* Xing / Info の判定: このフレームのスロット先頭 */
        f->is_xing = 0;
        if (f->main_bytes >= 4) {
            int32 m0;
            int32 m1;
            int32 m2;
            int32 m3;
            m0 = d->reservoir[slot_start] & 0xFF;
            m1 = d->reservoir[slot_start + 1] & 0xFF;
            m2 = d->reservoir[slot_start + 2] & 0xFF;
            m3 = d->reservoir[slot_start + 3] & 0xFF;
            if ((m0 == 0x58 && m1 == 0x69 && m2 == 0x6E
                    && m3 == 0x67)
                || (m0 == 0x49 && m1 == 0x6E && m2 == 0x66
                    && m3 == 0x6F)) {
                f->is_xing = 1;
            }
        }
    }

    /* ビットリザーバの帳簿: 不変条件を検査する */
    appended_before = d->appended_bits;
    d->appended_bits = d->appended_bits + (int64)f->main_bytes * 8;
    if ((int64)f->side.main_data_begin * 8 > appended_before) {
        d->err = -2;
        return -2;
    }
    data_start = appended_before - (int64)f->side.main_data_begin * 8;
    consumed_end = data_start
        + (int64)f->side.part2_3_length[0]
        + (int64)f->side.part2_3_length[1]
        + (int64)f->side.part2_3_length[2]
        + (int64)f->side.part2_3_length[3];
    if (consumed_end > d->appended_bits) {
        d->err = -3;
        return -3;
    }
    gap = data_start - d->prev_end_bits;
    if (gap < 0 && d->frames_seen > 0) {
        d->err = -4;
        return -4;
    }
    if (gap > d->max_gap_bits) {
        d->max_gap_bits = (int32)gap;
    }
    d->prev_end_bits = consumed_end;
    d->frames_seen = d->frames_seen + 1;
    return 1;
}

/* ---- スケールファクタとハフマン (段2) --------------------------
 * バンド種別: 0=long 1=short 2=mixed。start / stop ブロックは
 * スケールファクタとバンド幅は long として扱う */

/* scalefac_compress -> (slen1, slen2) を1つに詰めた表 */
static int32 scfc_decode[16] = {
    0, 1, 2, 3, 12, 5, 6, 7, 9, 10, 11, 13, 14, 15, 18, 19
};
/* 伝送グループ毎の個数 (種別 × グループ4つ)。
 * long は scfsi のグループと一致する */
static int32 part_cnt[3][4] = {
    { 6, 5, 5, 5 },
    { 9, 9, 6, 12 },
    { 8, 9, 6, 12 }
};

int32 block_kind(struct Mp3Dec *d, int32 gc)
{
    struct Mp3Side *s;
    s = &d->frame.side;
    if (s->window_switching[gc] == 0) {
        return 0;
    }
    if (s->block_type[gc] != 2) {
        return 0;                    /* start / stop は long 扱い */
    }
    if (s->mixed_block[gc]) {
        return 2;
    }
    return 1;
}

int32 sfb_width(struct Mp3Dec *d, int32 kind, int32 i)
{
    int32 sr;
    sr = d->frame.sr_idx;
    if (kind == 1) {
        return sfb_short[sr][i];
    }
    if (kind == 2) {
        return sfb_mixed[sr][i];
    }
    return sfb_long[sr][i];
}

/* 1 グラニュール×チャンネルのスケールファクタを復号し、
 * d->scf (gc*40 先頭) に生の値を置く。再利用 (scfsi) は
 * 同じフレームのグラニュール0の値から写す */
void decode_scalefactors(struct Mp3Dec *d, int32 gc)
{
    struct Mp3Side *s;
    int32 kind;
    int32 part;
    int32 sizes[4];
    int32 gr;
    int32 ch;
    int32 g;
    int32 k;
    int32 cnt;
    int32 idx;
    int32 use_scfsi;
    int32 out;
    int32 prev;
    s = &d->frame.side;
    kind = block_kind(d, gc);
    gr = gc >> 1;
    ch = gc & 1;
    part = scfc_decode[s->scalefac_compress[gc]];
    sizes[0] = part >> 2;
    sizes[1] = part >> 2;
    sizes[2] = part & 3;
    sizes[3] = part & 3;
    use_scfsi = 0;
    if (gr == 1 && kind != 1) {
        use_scfsi = 1;
    }
    out = gc * 40;
    prev = ((gr - 1) * 2 + ch) * 40;
    idx = 0;
    g = 0;
    while (g < 4) {
        cnt = part_cnt[kind][g];
        if (use_scfsi && s->scfsi[ch * 4 + g]) {
            k = 0;
            while (k < cnt) {
                d->scf[out + idx + k] = d->scf[prev + idx + k];
                d->ist[out + idx + k] = d->ist[prev + idx + k];
                k = k + 1;
            }
        } else if (sizes[g] == 0) {
            k = 0;
            while (k < cnt) {
                d->scf[out + idx + k] = 0;
                d->ist[out + idx + k] = 0;
                k = k + 1;
            }
        } else {
            k = 0;
            while (k < cnt) {
                d->scf[out + idx + k] = br_bits(d, sizes[g]);
                d->ist[out + idx + k] = d->scf[out + idx + k];
                k = k + 1;
            }
        }
        idx = idx + cnt;
        g = g + 1;
    }
    /* 送られてこない末尾バンド (long の21番、short の12番、
     * mixed の最後) は 0 */
    d->scf[out + idx] = 0;
    d->scf[out + idx + 1] = 0;
    d->scf[out + idx + 2] = 0;
    d->ist[out + idx] = 0;
    d->ist[out + idx + 1] = 0;
    d->ist[out + idx + 2] = 0;
}

/* 木を1ビットずつ辿ってシンボルを得る。戻り値は
 * シンボル (0 以上) または -1 (木が壊れている) */
int32 tree_walk(struct Mp3Dec *d, int32 quad, int32 root)
{
    int32 node;
    int32 nxt;
    int32 bit;
    node = root;
    while (node >= 0) {
        bit = br_bit(d);
        nxt = tree_get_child(d, quad, node, bit);
        if (nxt == TREE_EMPTY) {
            return -1;
        }
        if (nxt < 0) {
            return -nxt - 1;
        }
        node = nxt;
    }
    return -1;
}

/* ペアを1つ復号して *px, *py に置く (符号と linbits 込み) */
void huff_pair(struct Mp3Dec *d, int32 tab, int32 *px, int32 *py)
{
    int32 sym;
    int32 x;
    int32 y;
    int32 lin;
    int32 e;
    if (tab == 0 || d->tree_root[tab] < 0) {
        *px = 0;
        *py = 0;
        return;
    }
    /* 先行表で引ければ符号語はそこで確定。引けない
     * (長い符号語) ときだけ木を辿る */
    e = d->huff_fast[tab * 512 + br_peek(d, 9)];
    if (e >= 0) {
        d->br_pos = d->br_pos + (e >> 8);
        sym = e & 255;
    } else {
        sym = tree_walk(d, 0, d->tree_root[tab]);
        if (sym < 0) {
            *px = 0;
            *py = 0;
            return;
        }
    }
    x = (sym >> 4) & 15;
    y = sym & 15;
    lin = huff_linbits[tab];
    if (x == 15) {
        x = 15 + br_bits(d, lin);
    }
    if (x != 0 && br_bit(d)) {
        x = -x;
    }
    if (y == 15) {
        y = 15 + br_bits(d, lin);
    }
    if (y != 0 && br_bit(d)) {
        y = -y;
    }
    *px = x;
    *py = y;
}

/* 1 グラニュール×チャンネルのハフマン復号。
 * gen_end はこのグラニュールのデータ末尾 (ビット位置) */
void decode_huffman(struct Mp3Dec *d, int32 gc, int32 gen_end)
{
    struct Mp3Side *s;
    int32 kind;
    int32 base;
    int32 pos;
    int32 bv;
    int32 band;
    int32 ireg;
    int32 budgets[3];
    int32 tab;
    int32 budget;
    int32 width;
    int32 pairs;
    int32 k;
    int32 x;
    int32 y;
    s = &d->frame.side;
    kind = block_kind(d, gc);
    base = gc * 576;
    pos = 0;
    while (pos < 576) {
        d->is[base + pos] = 0;
        pos = pos + 1;
    }
    pos = 0;
    /* region のバンド数は count + 1 (規格の region0_count は
     * 「バンド数 - 1」で格納される)。window_switching 時は
     * region0 = 先頭 36 本ぶん: long / mixed では 8 項、
     * short では 9 項 (バンド3つ×3窓) */
    if (s->window_switching[gc]) {
        if (kind == 1) {
            budgets[0] = 9;
        } else {
            budgets[0] = 8;
        }
        budgets[1] = 1000;
        budgets[2] = 0;
    } else {
        budgets[0] = s->region0_count[gc] + 1;
        budgets[1] = s->region1_count[gc] + 1;
        budgets[2] = 1000;
    }
    bv = s->big_values[gc];
    band = 0;
    ireg = 0;
    while (bv > 0 && ireg < 3) {
        tab = s->table_select[gc * 3 + ireg];
        budget = budgets[ireg];
        ireg = ireg + 1;
        while (bv > 0 && budget > 0) {
            width = sfb_width(d, kind, band);
            band = band + 1;
            budget = budget - 1;
            pairs = width / 2;
            if (pairs > bv) {
                pairs = bv;
            }
            k = 0;
            while (k < pairs) {
                huff_pair(d, tab, &x, &y);
                d->is[base + pos] = x;
                d->is[base + pos + 1] = y;
                pos = pos + 2;
                k = k + 1;
            }
            bv = bv - pairs;
        }
    }
    /* count1 領域: quad を末尾まで。末尾を越える quad は
     * 符号語だけ読んで捨てる (参照実装の規則) */
    while (1) {
        int32 sym;
        int32 vals[4];
        int32 q;
        int32 e;
        e = d->qhuff_fast[s->count1table[gc] * 512 +
            br_peek(d, 9)];
        if (e >= 0) {
            d->br_pos = d->br_pos + (e >> 8);
            sym = e & 15;
        } else {
            sym = tree_walk(d, 1,
                d->qtree_root[s->count1table[gc]]);
            if (sym < 0) {
                break;
            }
        }
        if (d->br_pos > gen_end) {
            break;
        }
        q = 0;
        while (q < 4) {
            vals[q] = 0;
            if ((sym >> (3 - q)) & 1) {
                vals[q] = 1;
                if (br_bit(d)) {
                    vals[q] = -1;
                }
            }
            q = q + 1;
        }
        q = 0;
        while (q < 4) {
            if (pos < 576) {
                d->is[base + pos] = vals[q];
            }
            pos = pos + 1;
            q = q + 1;
        }
    }
}

/* 段3・段4 の関数の前置宣言 (定義は後ろ) */
void requant(struct Mp3Dec *d, int32 gc);
void stereo_process(struct Mp3Dec *d, int32 gr);
void stage4(struct Mp3Dec *d, int32 gc);
void synth_granule(struct Mp3Dec *d, int32 gr);

/* 現在のフレームの全グラニュール×チャンネルを復号する */
int32 dec_decode_frame(struct Mp3Dec *d)
{
    int32 gr;
    int32 ch;
    int32 gc;
    int32 gpos;
    gpos = 0;
    gr = 0;
    while (gr < 2) {
        ch = 0;
        while (ch < d->frame.channels) {
            gc = gr * 2 + ch;
            d->br_pos = gpos;
            decode_scalefactors(d, gc);
            decode_huffman(d, gc,
                gpos + d->frame.side.part2_3_length[gc]);
            gpos = gpos + d->frame.side.part2_3_length[gc];
            ch = ch + 1;
        }
        ch = 0;
        while (ch < d->frame.channels) {
            requant(d, gr * 2 + ch);
            ch = ch + 1;
        }
        stereo_process(d, gr);
        if (d->stage_limit >= 4) {
            ch = 0;
            while (ch < d->frame.channels) {
                stage4(d, gr * 2 + ch);
                ch = ch + 1;
            }
        }
        if (d->stage_limit >= 5) {
            synth_granule(d, gr);
        }
        gr = gr + 1;
    }
    return 0;
}

/* ---- 再量子化とステレオ (段3) -----------------------------------
 * xr は Q18 (値 × 262144)。再量子化は
 *   xr = |is|^(4/3) * 2^((global_gain-210)/4
 *        - scalefac * (scalefac_scale ? 1 : 0.5))
 * を指数の四分音単位で計算する */

static int32 pretab[10] = { 1, 1, 1, 1, 2, 2, 3, 3, 3, 2 };

int32 n_entries(int32 kind)
{
    if (kind == 1) {
        return 39;
    }
    if (kind == 2) {
        return 38;
    }
    return 22;
}

int32 n_long_entries(int32 kind)
{
    if (kind == 1) {
        return 0;
    }
    if (kind == 2) {
        return 8;
    }
    return 22;
}

int32 sat32(int64 v)
{
    if (v > 2147483647) {
        return 2147483647;
    }
    if (v < -2147483647 - 1) {
        return -2147483647 - 1;
    }
    return (int32)v;
}

int32 frame_is_ms(struct Mp3Dec *d)
{
    if (d->frame.mode == 1 && (d->frame.mode_ext & 2)) {
        return 1;
    }
    return 0;
}

void requant(struct Mp3Dec *d, int32 gc)
{
    struct Mp3Side *s;
    int32 kind;
    int32 nent;
    int32 nlong;
    int32 ge_q;
    int32 pos;
    int32 e;
    int32 k;
    s = &d->frame.side;
    kind = block_kind(d, gc);
    nent = n_entries(kind);
    nlong = n_long_entries(kind);
    ge_q = s->global_gain[gc] - 210;
    if (frame_is_ms(d)) {
        ge_q = ge_q - 2;
    }
    pos = 0;
    e = 0;
    while (e < nent) {
        int32 width;
        int32 scf_e;
        int32 ex;
        int32 q;
        int32 r;
        int32 frac;
        int32 shift;
        width = sfb_width(d, kind, e);
        scf_e = d->scf[gc * 40 + e];
        if (kind == 0 && s->preflag[gc] && e >= 11 && e <= 20) {
            scf_e = scf_e + pretab[e - 11];
        }
        if (kind != 0 && e >= nlong) {
            int32 w;
            w = (e - nlong) % 3;
            scf_e = scf_e + (s->subblock_gain[gc * 3 + w]
                << (2 - s->scalefac_scale[gc]));
        }
        ex = ge_q - (scf_e << (s->scalefac_scale[gc] + 1));
        if (ex >= 0) {
            q = ex / 4;
        } else {
            q = (ex - 3) / 4;
        }
        r = ex - 4 * q;
        frac = gain_frac_q24[r];
        shift = 24 - q;
        k = 0;
        while (k < width) {
            int32 v;
            int64 mag;
            int64 tmp;
            v = d->is[gc * 576 + pos + k];
            if (v == 0) {
                d->xr[gc * 576 + pos + k] = 0;
            } else {
                /* |v|^(4/3) を Q18 で: v * cbrt(v)。
                 * ゲインを掛けるまで int64 で持つ */
                if (v < 0) {
                    mag = ((int64)(-v) * cbrt_q24[-v] + 32)
                        >> 6;
                } else {
                    mag = ((int64)v * cbrt_q24[v] + 32) >> 6;
                }
                if (shift > 62) {
                    d->xr[gc * 576 + pos + k] = 0;
                } else {
                    tmp = mag * frac;
                    tmp = (tmp + ((int64)1 << (shift - 1)))
                        >> shift;
                    if (v < 0) {
                        tmp = -tmp;
                    }
                    d->xr[gc * 576 + pos + k] = sat32(tmp);
                }
            }
            k = k + 1;
        }
        pos = pos + width;
        e = e + 1;
    }
}

/* pan 係数 (Q24) を掛ける。ms 時はさらに sqrt(2) を掛ける */
int32 mul_pan(int32 v, int32 p, int32 ms)
{
    int64 r;
    r = ((int64)v * p + ((int64)1 << 23)) >> 24;
    if (ms) {
        r = (r * 23726566 + ((int64)1 << 23)) >> 24;
    }
    return sat32(r);
}

/* 1 グラニュールのステレオ処理 (xr に対して行う)。
 * バンドの並びと ist は左チャンネルのバンド構成で解釈する
 * (参照実装と同じ) */
void stereo_process(struct Mp3Dec *d, int32 gr)
{
    int32 i_st;
    int32 ms_t;
    int32 base0;
    int32 base1;
    int32 i;
    i_st = d->frame.mode_ext & 1;
    ms_t = d->frame.mode_ext & 2;
    base0 = (gr * 2) * 576;
    base1 = (gr * 2 + 1) * 576;
    if (d->frame.channels < 2) {
        return;
    }
    if (i_st == 0) {
        if (frame_is_ms(d)) {
            i = 0;
            while (i < 576) {
                int32 a;
                int32 b;
                a = d->xr[base0 + i];
                b = d->xr[base1 + i];
                d->xr[base0 + i] = sat32((int64)a + b);
                d->xr[base1 + i] = sat32((int64)a - b);
                i = i + 1;
            }
        }
        return;
    }
    {
        int32 kind0;
        int32 nent;
        int32 nlong;
        int32 istb;
        int32 max_band[3];
        int32 max_blocks;
        int32 pos;
        int32 e;
        int32 k;
        kind0 = block_kind(d, gr * 2);
        nent = n_entries(kind0);
        nlong = n_long_entries(kind0);
        istb = (gr * 2 + 1) * 40;
        max_band[0] = -1;
        max_band[1] = -1;
        max_band[2] = -1;
        pos = 0;
        e = 0;
        while (e < nent) {
            int32 w;
            int32 found;
            w = sfb_width(d, kind0, e);
            found = 0;
            k = 0;
            while (k < w && found == 0) {
                if (d->xr[base1 + pos + k] != 0
                    || d->xr[base1 + pos + k + 1] != 0) {
                    max_band[e % 3] = e;
                    found = 1;
                }
                k = k + 2;
            }
            pos = pos + w;
            e = e + 1;
        }
        if (nlong > 0) {
            int32 m;
            m = max_band[0];
            if (max_band[1] > m) {
                m = max_band[1];
            }
            if (max_band[2] > m) {
                m = max_band[2];
            }
            max_band[0] = m;
            max_band[1] = m;
            max_band[2] = m;
        }
        if (nent - nlong > 0) {
            max_blocks = 3;
        } else {
            max_blocks = 1;
        }
        i = 0;
        while (i < max_blocks) {
            int32 itop;
            int32 prev;
            itop = nent - max_blocks + i;
            prev = itop - max_blocks;
            if (max_band[i] >= prev) {
                d->ist[istb + itop] = 3;
            } else {
                d->ist[istb + itop] = d->ist[istb + prev];
            }
            i = i + 1;
        }
        pos = 0;
        e = 0;
        while (e < nent) {
            int32 w;
            int32 ipos;
            w = sfb_width(d, kind0, e);
            ipos = d->ist[istb + e];
            if (e > max_band[e % 3] && ipos < 7) {
                int32 kl;
                int32 kr;
                kl = pan_q24[2 * ipos];
                kr = pan_q24[2 * ipos + 1];
                k = 0;
                while (k < w) {
                    int32 l;
                    l = d->xr[base0 + pos + k];
                    d->xr[base0 + pos + k] = mul_pan(l, kl, ms_t);
                    d->xr[base1 + pos + k] = mul_pan(l, kr, ms_t);
                    k = k + 1;
                }
            } else if (ms_t) {
                k = 0;
                while (k < w) {
                    int32 a;
                    int32 b;
                    a = d->xr[base0 + pos + k];
                    b = d->xr[base1 + pos + k];
                    d->xr[base0 + pos + k] = sat32((int64)a + b);
                    d->xr[base1 + pos + k] = sat32((int64)a - b);
                    k = k + 1;
                }
            }
            pos = pos + w;
            e = e + 1;
        }
    }
}

/* ---- 並べ替え・エイリアス低減・IMDCT (段4) ----------------------
 * 入出力とも Q18。係数は Q30 で、積は int64 で計算して
 * 30 ビット戻す。変換は xr の中でインプレースに行い、
 * 結果はサブバンドサンプル (32 バンド×18) になる */

/* Q18 × Q30 -> Q18 (丸め付き) */
int32 mulp(int32 a, int32 c)
{
    int64 v;
    v = ((int64)a * c + ((int64)1 << 29)) >> 30;
    return sat32(v);
}

/* short 領域の並べ替え: (バンド, 窓) 順から
 * (係数, 窓) の交互順へ。gbase は (gr, ch) の先頭、
 * rel はグラニュール内の開始位置 (0 か 36)、
 * entries は sfb 表の開始項、kind は 1/2 */
void reorder(struct Mp3Dec *d, int32 gbase, int32 rel,
    int32 kind, int32 entries)
{
    int32 base;
    int32 i;
    int32 src;
    int32 dst;
    base = gbase + rel;
    i = 0;
    while (i < 576 - rel) {
        d->scratch[i] = d->xr[base + i];
        i = i + 1;
    }
    src = 0;
    dst = 0;
    while (entries < n_entries(kind)) {
        int32 len;
        int32 k;
        len = sfb_width(d, kind, entries);
        k = 0;
        while (k < len) {
            d->xr[base + dst] = d->scratch[src + k];
            d->xr[base + dst + 1] = d->scratch[src + len + k];
            d->xr[base + dst + 2] = d->scratch[src + 2 * len + k];
            dst = dst + 3;
            k = k + 1;
        }
        src = src + 3 * len;
        entries = entries + 3;
    }
}

/* エイリアス低減: 隣り合うバンド境界 nbands 組に
 * バタフライを掛ける */
void antialias(struct Mp3Dec *d, int32 base, int32 nbands)
{
    int32 b;
    int32 i;
    b = 0;
    while (b < nbands) {
        int32 o;
        o = base + b * 18;
        i = 0;
        while (i < 8) {
            int32 u;
            int32 dn;
            u = d->xr[o + 18 + i];
            dn = d->xr[o + 17 - i];
            d->xr[o + 18 + i] = sat32(
                (int64)mulp(u, aa_cs_q30[i])
                - mulp(dn, aa_ca_q30[i]));
            d->xr[o + 17 - i] = sat32(
                (int64)mulp(u, aa_ca_q30[i])
                + mulp(dn, aa_cs_q30[i]));
            i = i + 1;
        }
        b = b + 1;
    }
}

/* 9 点の DCT-III カーネル (Q18 のまま計算) */
void dct3_9(int32 *y)
{
    int32 s0;
    int32 s1;
    int32 s2;
    int32 s3;
    int32 s4;
    int32 s5;
    int32 s6;
    int32 s7;
    int32 s8;
    int32 t0;
    int32 t2;
    int32 t4;
    s0 = y[0];
    s2 = y[2];
    s4 = y[4];
    s6 = y[6];
    s8 = y[8];
    t0 = sat32((int64)s0 + mulp(s6, 536870912));
    s0 = sat32((int64)s0 - s6);
    t4 = mulp(sat32((int64)s4 + s2), C9_A);
    t2 = mulp(sat32((int64)s8 + s2), C9_B);
    s6 = mulp(sat32((int64)s4 - s8), C9_C);
    s4 = sat32((int64)s4 + s8 - s2);
    s2 = sat32((int64)s0 - mulp(s4, 536870912));
    y[4] = sat32((int64)s4 + s0);
    s8 = sat32((int64)t0 - t2 + s6);
    s0 = sat32((int64)t0 - t4 + t2);
    s4 = sat32((int64)t0 + t4 - s6);
    s1 = y[1];
    s3 = y[3];
    s5 = y[5];
    s7 = y[7];
    s3 = mulp(s3, C9_D);
    t0 = mulp(sat32((int64)s5 + s1), C9_E);
    t4 = mulp(sat32((int64)s5 - s7), C9_F);
    t2 = mulp(sat32((int64)s1 + s7), C9_G);
    s1 = mulp(sat32((int64)s1 - s5 - s7), C9_D);
    s5 = sat32((int64)t0 - s3 - t2);
    s7 = sat32((int64)t4 - s3 - t0);
    s3 = sat32((int64)t4 + s3 - t2);
    y[0] = sat32((int64)s4 - s7);
    y[1] = sat32((int64)s2 + s1);
    y[2] = sat32((int64)s0 - s3);
    y[3] = sat32((int64)s8 + s5);
    y[5] = sat32((int64)s8 - s5);
    y[6] = sat32((int64)s0 + s3);
    y[7] = sat32((int64)s2 - s1);
    y[8] = sat32((int64)s4 + s7);
}

/* バンド 1 個ぶん (18 個) が全部ゼロかどうか。
 * ゼロバンドの IMDCT は新しい和が全部 0 になるので、
 * 前バンドの尻尾への窓掛けだけに畳める (imdct36 と
 * imdct_short_band の近道で使う)。最初に非ゼロを
 * 見つけたらすぐ戻るので、密なバンドでは点検の
 * コストは小さい */
int32 band_all_zero(struct Mp3Dec *d, int32 x)
{
    int32 i;
    i = 0;
    while (i < 18) {
        if (d->xr[x + i] != 0) {
            return 0;
        }
        i = i + 1;
    }
    return 1;
}

/* 長い IMDCT (36 点) を nbands バンドぶん行う。
 * win は窓表 (Q30、18 個) */
void imdct36(struct Mp3Dec *d, int32 base, int32 ob,
    int32 *win, int32 nbands)
{
    int32 j;
    int32 i;
    j = 0;
    while (j < nbands) {
        int32 x;
        int32 o;
        x = base + j * 18;
        o = ob + j * 9;
        if (band_all_zero(d, x)) {
            /* 入力が全ゼロなら新しい和は全部 0 で、
             * 出力は前の尻尾 (overlap) に窓を掛けた
             * だけ、新しい状態は全部 0。消える項は
             * mulp(0, ..) = 0 のものだけなので、
             * 出力は通常路とビット単位で同じ */
            i = 0;
            while (i < 9) {
                int32 ovl;
                ovl = d->overlap[o + i];
                d->xr[x + i] = mulp(ovl, win[i]);
                d->xr[x + 17 - i] = mulp(ovl, win[9 + i]);
                d->overlap[o + i] = 0;
                i = i + 1;
            }
        } else {
        d->mdct_co[0] = sat32(-(int64)d->xr[x]);
        d->mdct_si[0] = d->xr[x + 17];
        i = 0;
        while (i < 4) {
            d->mdct_si[8 - 2 * i] = sat32(
                (int64)d->xr[x + 4 * i + 1] - d->xr[x + 4 * i + 2]);
            d->mdct_co[1 + 2 * i] = sat32(
                (int64)d->xr[x + 4 * i + 1] + d->xr[x + 4 * i + 2]);
            d->mdct_si[7 - 2 * i] = sat32(
                (int64)d->xr[x + 4 * i + 4] - d->xr[x + 4 * i + 3]);
            d->mdct_co[2 + 2 * i] = sat32(
                -((int64)d->xr[x + 4 * i + 3]
                  + d->xr[x + 4 * i + 4]));
            i = i + 1;
        }
        dct3_9(d->mdct_co);
        dct3_9(d->mdct_si);
        d->mdct_si[1] = sat32(-(int64)d->mdct_si[1]);
        d->mdct_si[3] = sat32(-(int64)d->mdct_si[3]);
        d->mdct_si[5] = sat32(-(int64)d->mdct_si[5]);
        d->mdct_si[7] = sat32(-(int64)d->mdct_si[7]);
        i = 0;
        while (i < 9) {
            int32 ovl;
            int32 sum;
            ovl = d->overlap[o + i];
            sum = sat32(
                (int64)mulp(d->mdct_co[i], twid9_q30[9 + i])
                + mulp(d->mdct_si[i], twid9_q30[i]));
            d->overlap[o + i] = sat32(
                (int64)mulp(d->mdct_co[i], twid9_q30[i])
                - mulp(d->mdct_si[i], twid9_q30[9 + i]));
            d->xr[x + i] = sat32(
                (int64)mulp(ovl, win[i]) - mulp(sum, win[9 + i]));
            d->xr[x + 17 - i] = sat32(
                (int64)mulp(ovl, win[9 + i])
                + mulp(sum, win[i]));
            i = i + 1;
        }
        }
        j = j + 1;
    }
}

/* 3 点の IDCT (短い IMDCT のカーネル) */
void idct3(int32 x0, int32 x1, int32 x2, int32 *dst)
{
    int32 m1;
    int32 a1;
    m1 = mulp(x1, C9_D);
    a1 = sat32((int64)x0 - mulp(x2, 536870912));
    dst[1] = sat32((int64)x0 + x2);
    dst[0] = sat32((int64)a1 + m1);
    dst[2] = sat32((int64)a1 - m1);
}

/* 短い IMDCT (12 点) 1 窓ぶん。入力は mdct_tmp の
 * w, w+3, ..., w+15、状態は overlap[st_off..]、
 * 出力は dst_overlap なら overlap、でなければ xr の
 * dst_off へ 6 個 */
void imdct12(struct Mp3Dec *d, int32 w, int32 dst_overlap,
    int32 dst_off, int32 st_off)
{
    int32 i;
    idct3(sat32(-(int64)d->mdct_tmp[w]),
        sat32((int64)d->mdct_tmp[w + 6] + d->mdct_tmp[w + 3]),
        sat32((int64)d->mdct_tmp[w + 12] + d->mdct_tmp[w + 9]),
        d->mdct_co);
    idct3(d->mdct_tmp[w + 15],
        sat32((int64)d->mdct_tmp[w + 12] - d->mdct_tmp[w + 9]),
        sat32((int64)d->mdct_tmp[w + 6] - d->mdct_tmp[w + 3]),
        d->mdct_si);
    d->mdct_si[1] = sat32(-(int64)d->mdct_si[1]);
    i = 0;
    while (i < 3) {
        int32 ovl;
        int32 sum;
        int32 v0;
        int32 v1;
        ovl = d->overlap[st_off + i];
        sum = sat32(
            (int64)mulp(d->mdct_co[i], twid3_q30[3 + i])
            + mulp(d->mdct_si[i], twid3_q30[i]));
        d->overlap[st_off + i] = sat32(
            (int64)mulp(d->mdct_co[i], twid3_q30[i])
            - mulp(d->mdct_si[i], twid3_q30[3 + i]));
        v0 = sat32((int64)mulp(ovl, twid3_q30[2 - i])
            - mulp(sum, twid3_q30[5 - i]));
        v1 = sat32((int64)mulp(ovl, twid3_q30[5 - i])
            + mulp(sum, twid3_q30[2 - i]));
        if (dst_overlap) {
            d->overlap[dst_off + i] = v0;
            d->overlap[dst_off + 5 - i] = v1;
        } else {
            d->xr[dst_off + i] = v0;
            d->xr[dst_off + 5 - i] = v1;
        }
        i = i + 1;
    }
}

/* 短いバンド 1 個 (3 窓) の IMDCT */
void imdct_short_band(struct Mp3Dec *d, int32 base, int32 ob)
{
    int32 i;
    if (band_all_zero(d, base)) {
        /* 入力が全ゼロのとき: 窓0 の出力は状態の
         * 尻尾 (ob+6..) への窓掛けだけ、窓1 は窓0 が
         * 状態を 0 にした後を読むので出力 0、窓2 は
         * 状態側へ 0 を書く。消える項は全部 0 の項
         * なので、結果は通常路とビット単位で同じ */
        i = 0;
        while (i < 6) {
            d->xr[base + i] = d->overlap[ob + i];
            i = i + 1;
        }
        i = 0;
        while (i < 3) {
            int32 ovl;
            ovl = d->overlap[ob + 6 + i];
            d->xr[base + 6 + i] = mulp(ovl, twid3_q30[2 - i]);
            d->xr[base + 11 - i] = mulp(ovl, twid3_q30[5 - i]);
            d->overlap[ob + 6 + i] = 0;
            i = i + 1;
        }
        i = 0;
        while (i < 6) {
            d->xr[base + 12 + i] = 0;
            d->overlap[ob + i] = 0;
            i = i + 1;
        }
        return;
    }
    i = 0;
    while (i < 18) {
        d->mdct_tmp[i] = d->xr[base + i];
        i = i + 1;
    }
    i = 0;
    while (i < 6) {
        d->xr[base + i] = d->overlap[ob + i];
        i = i + 1;
    }
    imdct12(d, 0, 0, base + 6, ob + 6);
    imdct12(d, 1, 0, base + 12, ob + 6);
    imdct12(d, 2, 1, ob, ob + 6);
}

/* (gr, ch) 1 つに段4 の変換を適用する */
void stage4(struct Mp3Dec *d, int32 gc)
{
    int32 kind;
    int32 base;
    int32 ob;
    int32 nlb;
    int32 bt;
    int32 j;
    kind = block_kind(d, gc);
    base = gc * 576;
    ob = (gc % 2) * 288;
    nlb = 0;
    if (kind == 2) {
        nlb = 2;
    }
    if (kind != 0) {
        if (kind == 2) {
            reorder(d, base, 36, kind, 8);
        } else {
            reorder(d, base, 0, kind, 0);
        }
        if (kind == 2) {
            antialias(d, base, 1);
        }
    } else {
        antialias(d, base, 31);
    }
    if (nlb > 0) {
        imdct36(d, base, ob, win_norm_q30, nlb);
    }
    if (kind == 0) {
        bt = d->frame.side.block_type[gc];
        if (bt == 3) {
            imdct36(d, base, ob, win_stop_q30, 32);
        } else {
            imdct36(d, base, ob, win_norm_q30, 32);
        }
    } else {
        j = nlb;
        while (j < 32) {
            imdct_short_band(d, base + j * 18, ob + j * 9);
            j = j + 1;
        }
    }
    /* 周波数反転: 奇数バンドの奇数位置を反転 */
    j = 1;
    while (j < 32) {
        int32 i;
        i = 1;
        while (i < 18) {
            d->xr[base + j * 18 + i] = sat32(
                -(int64)d->xr[base + j * 18 + i]);
            i = i + 2;
        }
        j = j + 2;
    }
}

/* ---- ポリフェーズ合成 (段5、2026-11-20 に DCT-32 形へ) --------
 * 仕様の形で合成する。ブロック毎に
 *   (1) 行列演算 V[i] = sum_k cos((2k+1)(i+16)pi/64) * S[k]
 *       (i=0..63、cos は Q30、入力 S は Q18、V は Q18)
 *   (2) 窓掛け out[m] = sum_tau D(tau,m) * V_tau[row(tau,m)]
 *       (D は仕様 Table 3-B.3 の整数値、最後に >>19 で
 *        丸めて int16 に飽和させる)
 * 表 (dcttab.c) は段5 の FIR 表 (firtab.c) を因数分解して
 * 作った (host/gen_dcttab.py)。旧 FIR 形との出力差は
 * ホストの単体テスト (host/test_synth.c) で最大 7 LSB。
 * 行列演算は cos 行列の対称性で畳む (独立な行だけを
 * 計算し、入力を事前に組み合わせて項数を減らす)。
 * 窓掛けは添字を進めて回す (strength reduction)。
 * どちらも演算の結合順を変えるだけで、丸めは
 * 変えていないので出力は不変 */

void synth_granule(struct Mp3Dec *d, int32 gr)
{
    int32 nch;
    int32 t;
    int32 ch;
    nch = d->frame.channels;
    t = 0;
    while (t < 18) {
        int32 slot;
        int32 i;
        int32 m;
        slot = d->v_head;
        ch = 0;
        while (ch < nch) {
            int32 gc;
            int32 sb[32];
            int32 voff;
            gc = gr * 2 + ch;
            /* S を連続な局所配列へ集める (xr はバンド
             * 方向に stride 18 で、内側ループで毎回
             * 添字を計算し直すと乗算が支配的になる) */
            i = 0;
            while (i < 32) {
                sb[i] = d->xr[gc * 576 + i * 18 + t];
                i = i + 1;
            }
            /* 行列演算は cos 行列の対称性で畳む
             * (2026-11-20、数値検証済み):
             *   V[17..32] = -V[15..0]、V[49..63] =
             *   V[47..33]、V[16] = 0 なので、独立な
             *   行は 0..15 と 33..48 の 32 行だけ。
             *   各行は列の対称 (k <-> 31-k、符号は
             *   (-1)^行) で入力を畳んで 16 項に。
             *   偶数行はさらに (k <-> 15-k) の対称
             *   (符号は行 mod 4 で決まる) で 8 項に。
             *   奇数行は後半の係数を表から直接読む。
             * 積和は 2048 -> 384 回。int64 の加算順序を
             * 変えるだけで丸めは最後の 1 回だけなので、
             * 出力はバイト不変 */
            voff = (ch * 16 + slot) * 64;
            {
                int64 cpv[16];
                int64 cmv[16];
                int64 dpv[8];
                int64 dqv[8];
                int32 a;
                int32 row;
                int32 k;
                k = 0;
                while (k < 16) {
                    cpv[k] = (int64)sb[k] + sb[31 - k];
                    cmv[k] = (int64)sb[k] - sb[31 - k];
                    k = k + 1;
                }
                k = 0;
                while (k < 8) {
                    dpv[k] = cpv[k] + cpv[15 - k];
                    dqv[k] = cpv[k] - cpv[15 - k];
                    k = k + 1;
                }
                a = 0;
                while (a < 32) {
                    int64 acc;
                    if (a < 16) {
                        row = a;
                    } else {
                        row = a + 17;
                    }
                    acc = 0;
                    k = 0;
                    if ((row & 1) == 0) {
                        if ((row & 3) == 0) {
                            while (k < 8) {
                                acc = acc +
                                    (int64)synth_cos_q30[row * 32 + k] * dpv[k];
                                k = k + 1;
                            }
                        } else {
                            while (k < 8) {
                                acc = acc +
                                    (int64)synth_cos_q30[row * 32 + k] * dqv[k];
                                k = k + 1;
                            }
                        }
                    } else {
                        while (k < 8) {
                            acc = acc +
                                (int64)synth_cos_q30[row * 32 + k] * cmv[k];
                            acc = acc +
                                (int64)synth_cos_q30[row * 32 + (15 - k)] * cmv[15 - k];
                            k = k + 1;
                        }
                    }
                    d->v_hist[voff + row] = sat32(
                        (acc + ((int64)1 << 29)) >> 30);
                    a = a + 1;
                }
                d->v_hist[voff + 16] = 0;
                a = 1;
                while (a < 17) {
                    d->v_hist[voff + 16 + a] =
                        -d->v_hist[voff + 16 - a];
                    a = a + 1;
                }
                a = 0;
                while (a < 15) {
                    d->v_hist[voff + 49 + a] =
                        d->v_hist[voff + 47 - a];
                    a = a + 1;
                }
            }
            ch = ch + 1;
        }
        d->v_head = slot + 1;
        if (d->v_head >= 16) {
            d->v_head = 0;
        }
        ch = 0;
        while (ch < nch) {
            int32 chbase;
            int32 di;
            chbase = ch * 16 * 64;
            di = 0;
            m = 0;
            while (m < 32) {
                int64 acc;
                int32 tau;
                int32 soff;
                acc = 0;
                tau = 0;
                /* 古いブロックへは 64 ずつ戻り、リングの
                 * 底で先頭へ回す。D と行の表は (m, tau)
                 * の順に平坦な添字を進めて読む */
                soff = chbase + slot * 64;
                while (tau < 16) {
                    acc = acc + (int64)synth_d[di]
                        * d->v_hist[soff + synth_row[di]];
                    di = di + 1;
                    soff = soff - 64;
                    if (soff < chbase) {
                        soff = soff + 16 * 64;
                    }
                    tau = tau + 1;
                }
                {
                    int64 v;
                    if (acc >= 0) {
                        v = (acc + ((int64)1 << 18)) >> 19;
                    } else {
                        v = -(((-acc) + ((int64)1 << 18)) >> 19);
                    }
                    if (v > 32767) {
                        v = 32767;
                    }
                    if (v < -32768) {
                        v = -32768;
                    }
                    d->pcm[(gr * 576 + t * 32 + m) * nch + ch] = (int16)v;
                }
                m = m + 1;
            }
            ch = ch + 1;
        }
        t = t + 1;
    }
}
