/* sd.c -- SD カードドライバ (K210、SPI モード) のプロトコル側
 *         (doc/sd-design.md の段2)
 *
 * ハードウェアの細部 (SPI1 のレジスタ、FPIOA、GPIOHS) は
 * sd_hw.c に隔離し、このファイルは sd_hw.h の口だけを使う。
 * そのおかげで、このファイルはホスト上でもそのままコンパイル
 * でき、モックのカード相手に検証できる (spike-k210/step5-sd/
 * host-test/)。
 *
 * 対応する初期化の流れ:
 *   v2 系 (SDHC/SDXC、ブロック単位アドレス) が本線。
 *   v1 系 (SDSC、バイト単位アドレス) は CMD8 が違法扱い
 *   されることで見分け、CMD16 で 512 に固定してから
 *   バイト単位で読む (古い 1GB カードが実在するため)
 * 失敗はすべて有限回で諦める。カードが無くても壊れて
 * いても、カーネルやシェルが止まることはない
 */

#include <stdint.h>
#include "blkdev.h"
#include "sd_hw.h"

#define R1_IDLE 0x01
#define R1_ILLEGAL 0x04

static int card_ready = 0;    /* 初期化が済んだか */
static int card_ccs = 0;      /* 1 = ブロック単位アドレス (v2) */
static uint64_t card_blocks = 0;

/* CS を落としてからコマンドを送り、R1 を返す。
 * bit7 が立つバイト (0xFF の無応答など) が来たら待ち直し、
 * 上限で諦めて負数を返す */
static int send_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t frame[6];
    frame[0] = 0x40 | cmd;
    frame[1] = (uint8_t)(arg >> 24);
    frame[2] = (uint8_t)(arg >> 16);
    frame[3] = (uint8_t)(arg >> 8);
    frame[4] = (uint8_t)arg;
    /* CRC は CMD0 と CMD8 だけ必須 (他はダミー可) */
    frame[5] = (cmd == 0) ? 0x95 : (cmd == 8) ? 0x87 : 0x01;

    sd_cs(0);
    for (int i = 0; i < 6; i++) {
        sd_xfer(frame[i]);
    }
    for (int i = 0; i < 32; i++) {
        uint8_t r = sd_xfer(0xFF);
        if (!(r & 0x80)) {
            return r;
        }
    }
    return -1;
}

/* コマンドの残りの応答と後始末。CS を戻し、バスを
 * 空けるために 1 バイト余分に回す */
static void cmd_end(void)
{
    sd_cs(1);
    sd_xfer(0xFF);
}

/* ACMD41 (CMD55 + CMD41) で起動完了を待つ */
static int wait_ready(int hcs)
{
    for (int i = 0; i < 2000; i++) {
        int r = send_cmd(55, 0);
        cmd_end();
        if (r < 0) {
            return -1;
        }
        r = send_cmd(41, hcs ? 0x40000000u : 0);
        int done = (r == 0);
        cmd_end();
        if (done) {
            return 0;
        }
        sd_delay_ms(1);
    }
    return -1;
}

/* CMD9 で CSD を読み、総ブロック数を出す */
static int read_capacity(void)
{
    uint8_t csd[16];
    int r = send_cmd(9, 0);
    if (r != 0) {
        cmd_end();
        return -1;
    }
    int token = -1;
    for (int i = 0; i < 256; i++) {
        uint8_t b = sd_xfer(0xFF);
        if (b == 0xFE) {
            token = 0;
            break;
        }
    }
    if (token != 0) {
        cmd_end();
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        csd[i] = sd_xfer(0xFF);
    }
    sd_xfer(0xFF);
    sd_xfer(0xFF);   /* CRC */
    cmd_end();

    if ((csd[0] >> 6) == 1) {
        /* CSD v2.0 (SDHC/SDXC): 容量 = (C_SIZE + 1) * 512KB。
         * 512 バイトブロック数に直すと * 1024 */
        uint32_t csize = ((uint32_t)(csd[7] & 0x3F) << 16) |
                         ((uint32_t)csd[8] << 8) | csd[9];
        card_blocks = (uint64_t)(csize + 1) * 1024;
    } else {
        /* CSD v1.0 (SDSC): 容量 =
         * (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN
         * バイト。512 バイトブロック数へは /512 */
        uint32_t bl_len = csd[5] & 0x0F;
        uint32_t csize = ((uint32_t)(csd[6] & 0x03) << 10) |
                         ((uint32_t)csd[7] << 2) |
                         ((uint32_t)csd[8] >> 6);
        uint32_t mult = ((uint32_t)(csd[9] & 0x03) << 1) |
                        ((uint32_t)csd[10] >> 7);
        uint64_t bytes = (uint64_t)(csize + 1) << (mult + 2 + bl_len);
        card_blocks = bytes / 512;
    }
    return 0;
}

static int sd_init(void)
{
    if (card_ready) {
        return 0;
    }
    sd_hw_init();

    /* 電源投入シーケンス: CS 高いまま 74 クロック以上 */
    sd_cs(1);
    for (int i = 0; i < 16; i++) {
        sd_xfer(0xFF);
    }

    /* CMD0: idle へ。有効な R1 が返るまで数回試す */
    int r = -1;
    for (int i = 0; i < 32 && r != R1_IDLE; i++) {
        r = send_cmd(0, 0);
        cmd_end();
    }
    if (r != R1_IDLE) {
        return -1;
    }

    /* CMD8: v2 系なら R7 (R1 + 4 バイト) が返る。
     * v1 系では違法コマンドになり、その場合は古い流れへ */
    r = send_cmd(8, 0x1AA);
    int v2 = (r == R1_IDLE);
    if (v2) {
        for (int i = 0; i < 4; i++) {
            sd_xfer(0xFF);   /* R7 の残り */
        }
    }
    cmd_end();
    if (r < 0) {
        return -1;
    }

    if (wait_ready(v2) != 0) {
        return -1;
    }

    if (v2) {
        /* CMD58 で OCR を読み、CCS (容量の単位) を確定 */
        r = send_cmd(58, 0);
        if (r != 0) {
            cmd_end();
            return -1;
        }
        uint8_t ocr0 = sd_xfer(0xFF);
        sd_xfer(0xFF);
        sd_xfer(0xFF);
        sd_xfer(0xFF);
        cmd_end();
        card_ccs = (ocr0 & 0x40) != 0;
        if (!card_ccs) {
            /* 稀な v2 の SDSC: ブロック長を 512 に固定 */
            r = send_cmd(16, 512);
            cmd_end();
            if (r != 0) {
                return -1;
            }
        }
    } else {
        card_ccs = 0;
        r = send_cmd(16, 512);
        cmd_end();
        if (r != 0) {
            return -1;
        }
    }

    if (read_capacity() != 0) {
        return -1;
    }

    sd_hw_set_fast();
    card_ready = 1;
    return 0;
}

static int sd_read_block(uint64_t lba, unsigned char *buf)
{
    uint32_t arg = card_ccs ? (uint32_t)lba : (uint32_t)(lba * 512);
    int r = send_cmd(17, arg);
    if (r != 0) {
        cmd_end();
        return -1;
    }
    /* データ開始トークン 0xFE を待つ (有限回) */
    int got = 0;
    for (int i = 0; i < 4096; i++) {
        if (sd_xfer(0xFF) == 0xFE) {
            got = 1;
            break;
        }
    }
    if (!got) {
        cmd_end();
        return -1;
    }
    for (int i = 0; i < 512; i++) {
        buf[i] = sd_xfer(0xFF);
    }
    sd_xfer(0xFF);
    sd_xfer(0xFF);   /* CRC */
    cmd_end();
    return 0;
}

/* 1 ブロック書く (CMD24)。データ応答トークンで受理を
 * 確かめ、カードが内部で書き終えるまで (DO が 0 = busy)
 * 有限回待つ (doc/sd-write-design.md 段1) */
static int sd_write_block(uint64_t lba, const unsigned char *buf)
{
    uint32_t arg = card_ccs ? (uint32_t)lba : (uint32_t)(lba * 512);
    int r = send_cmd(24, arg);
    if (r != 0) {
        cmd_end();
        return -1;
    }
    sd_xfer(0xFF);   /* データ開始までの隙間 (1 バイト以上) */
    sd_xfer(0xFE);   /* データ開始トークン */
    for (int i = 0; i < 512; i++) {
        sd_xfer(buf[i]);
    }
    sd_xfer(0xFF);
    sd_xfer(0xFF);   /* CRC (ダミー) */
    /* データ応答トークン: 下位 5 ビットが 00101 で受理 */
    r = sd_xfer(0xFF);
    if ((r & 0x1F) != 0x05) {
        cmd_end();
        return -1;
    }
    /* busy 待ち: 書き込み中は DO が 0 を返す。カードの
     * 書き込み時間は最大でも数百 ms の想定で、有限回で
     * 諦める (数 MHz の転送で 2M 回は秒単位の余裕) */
    {
        int done = 0;
        for (long i = 0; i < 2000000; i++) {
            if (sd_xfer(0xFF) == 0xFF) {
                done = 1;
                break;
            }
        }
        if (!done) {
            cmd_end();
            return -1;
        }
    }
    cmd_end();
    return 0;
}

static int sd_dev_read(struct blkdev *dev, uint64_t lba,
                       unsigned char *buf, uint32_t count)
{
    (void)dev;
    for (uint32_t i = 0; i < count; i++) {
        if (sd_read_block(lba + i, buf + (uint64_t)i * 512) != 0) {
            return -1;
        }
    }
    return 0;
}

static int sd_dev_write(struct blkdev *dev, uint64_t lba,
                        const unsigned char *buf, uint32_t count)
{
    (void)dev;
    for (uint32_t i = 0; i < count; i++) {
        if (sd_write_block(lba + i, buf + (uint64_t)i * 512) != 0) {
            return -1;
        }
    }
    return 0;
}

static struct blkdev sd_dev = {
    .name = "sd0",
    .block_count = 0,
    .read = sd_dev_read,
    .write = sd_dev_write,
};

/* 初期化に成功したら SD の blkdev を返し、失敗なら 0。
 * 初期化は最初の呼び出しでのみ行い (遅延初期化)、失敗は
 * 次の呼び出しで再挑戦する (カードの挿し直しに追随) */
struct blkdev *sd_blkdev(void)
{
    if (sd_init() != 0) {
        return 0;
    }
    sd_dev.block_count = card_blocks;
    return &sd_dev;
}
