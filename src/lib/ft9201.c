/*
 * ft9201.c — 検証D (T2): FT9201 bring-up (同期版) と 96×96 画像取得。
 *
 * OMGrant/ft9201-libfprint src/ft9201.c (LGPL-2.1-or-later) の init/scan 状態機械を
 * 同期 C に移植したもの。シーケンス・定数・タイムアウトは原則そのまま。
 *
 * 注意 (banianitc 版から引継ぎ): init シーケンスの途中で失敗した場合、
 * デバイスが不安定になることがある → その場合はセンサーを抜き差しして
 * 電源を入れ直すこと。エラー時は継続せず即 abort する設計。
 */

#include "ft9201.h"
#include "ft9201_fw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FT9201_FW_CHUNK 64

static double now_sec(void) {
    static double t0 = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9;
    if (t0 == 0) t0 = t;
    return t - t0;
}

#define LOG(...)                                 \
    do {                                         \
        fprintf(stderr, "[%5.2fs] ", now_sec()); \
        fprintf(stderr, __VA_ARGS__);            \
        fprintf(stderr, "\n");                   \
    } while (0)

/* vendor ctrl_out を 1 ステップとして実行。失敗したら abort。 */
#define STEP(name, request, value, index)                                      \
    do {                                                                       \
        if (ft_usb_ctrl_out(f->usb, (request), (value), (index)) != 0) {       \
            LOG("!! %s 失敗: req=0x%02x val=0x%04x idx=0x%04x", name,          \
                (unsigned)(request), (unsigned)(value), (unsigned)(index));   \
            return -1;                                                         \
        }                                                                      \
    } while (0)

/* ================================================================ */
/* AFE レジスタ                                                     */
/* ================================================================ */

int ft9201_reg_read(Ft9201 *f, uint8_t reg, uint8_t *buf, uint16_t len) {
    uint16_t done = 0;
    memset(buf, 0, len);
    int r = ft_usb_ctrl_in(f->usb, REQ_AFE_READ, 0, reg, buf, len, &done);
    if (r != 0) return -1;
    if (done < len) return -2;
    return 0;
}

int ft9201_reg_write(Ft9201 *f, uint8_t reg, uint8_t value) {
    return ft_usb_ctrl_out(f->usb, REQ_AFE_WRITE, value, reg);
}

static int wake(Ft9201 *f) {
    STEP("wakeup", REQ_WAKEUP, 0x70, 0x70);
    return 0;
}

/* ================================================================ */
/* init シーケンス                                                  */
/* ================================================================ */

int ft9201_init(Ft9201 *f, Ft9201Usb *u) {
    int r;
    uint8_t buf[16];
    uint16_t done = 0;
    memset(f, 0, sizeof *f);
    f->usb = u;

    /* --- preamble --- */
    LOG("--- preamble ---");
    wake(f);
    wake(f);

    STEP("SUI setup (reg 0x90)", REQ_SUI_CMD, 0x9001, 0x0000);
    r = ft_usb_ctrl_in(f->usb, REQ_SUI_READ, 0, 0, buf, 1, &done);
    if (r != 0) {
        LOG("!! SUI read 失敗");
        return -1;
    }
    LOG("SUI read: 0x%02x", buf[0]);

    STEP("F906 cmd", REQ_SUI_CMD, 0x0603, 0x00f9);
    STEP("bulk arm (mode 2)", REQ_BULK_ARM, 0x0002, 0x0000);
    STEP("bulk size 4", REQ_BULK_SIZE, 0x0004, 0x85c0);

    LOG("bulk payload {11 ee 02 00}");
    {
        static const uint8_t payload[4] = {0x11, 0xee, 0x02, 0x00};
        if (ft_usb_bulk_write(u, payload, sizeof payload, FT_BULK_TIMEOUT_MS) != 0) {
            LOG("!! bulk payload 失敗");
            return -1;
        }
    }

    STEP("hw reg a4=1", REQ_HW_REG, 0x0001, 0x00a4);
    LOG("sensor status (ignore)");
    r = ft_usb_ctrl_in(f->usb, REQ_STATUS, 0, 0x85c0, buf, 2, &done);
    if (r != 0) {
        LOG("!! status read 失敗");
        return -1;
    }

    /* --- sensor type detection (new SPI protocol) --- */
    LOG("--- sensor type detection (SPI) ---");
    STEP("MCU download mode", REQ_MCU_DL_MODE, 0, 0);

    STEP("SPI write c8", REQ_SPI_WRITE, 0x00df, 0x00c8);
    r = ft_usb_ctrl_in(f->usb, REQ_SPI_READ, 0, 0x00c8, buf, 1, &done);
    if (r != 0) return -1;

    STEP("SPI write f1", REQ_SPI_WRITE, 0x001d, 0x00f1);
    r = ft_usb_ctrl_in(f->usb, REQ_SPI_READ, 0, 0x00f4, buf, 1, &done);
    if (r != 0) return -1;
    uint8_t byte2 = buf[0];

    STEP("SPI write f4 (byte2|1)", REQ_SPI_WRITE, (uint16_t)(byte2 | 0x01), 0x00f4);
    r = ft_usb_ctrl_in(f->usb, REQ_SPI_READ, 0, 0x00f3, buf, 1, &done);
    if (r != 0) return -1;
    uint8_t raw = buf[0];
    f->sensor_variant = (uint8_t)((raw >> 1) & 0x0f);
    LOG("type_raw=0x%02x → sensor_type=%u (%s)", raw, f->sensor_variant,
        f->sensor_variant == 2   ? "FT9348W"
        : f->sensor_variant == 3 ? "FT9348W(var)"
        : f->sensor_variant == 4 ? "FT9361W"
                                 : "unknown");

    STEP("SPI finalize", REQ_SPI_WRITE, 0x0000, 0x00f4);

    /* --- firmware upload --- */
    LOG("--- firmware upload (%lu bytes, %lu chunks) ---", ft9201_fw_size,
        (ft9201_fw_size + FT9201_FW_CHUNK - 1) / FT9201_FW_CHUNK);
    wake(f);
    wake(f);
    STEP("MCU download mode (fw)", REQ_MCU_DL_MODE, 0, 0);
    STEP("bulk reset (0xff)", REQ_BULK_ARM, 0x00ff, 0x0000);
    STEP("bulk mode 2 (fw)", REQ_BULK_ARM, 0x0002, 0x0000);
    LOG("bridge version (ignore)");
    r = ft_usb_ctrl_in(f->usb, 0x1a, 0, 0, buf, 2, &done);
    if (r != 0) return -1;
    STEP("bulk chunk size 64", REQ_BULK_SIZE, FT9201_FW_CHUNK, 0);

    {
        uint8_t chunk[FT9201_FW_CHUNK];
        unsigned long off = 0;
        while (off < ft9201_fw_size) {
            unsigned long n = ft9201_fw_size - off;
            if (n > FT9201_FW_CHUNK) n = FT9201_FW_CHUNK;
            memset(chunk, 0, sizeof chunk);
            memcpy(chunk, ft9201_fw_data + off, n);
            if (ft_usb_bulk_write(u, chunk, FT9201_FW_CHUNK, FT_BULK_TIMEOUT_MS) != 0) {
                LOG("!! fw chunk %lu 失敗", off / FT9201_FW_CHUNK);
                return -1;
            }
            off += n;
            if ((off / FT9201_FW_CHUNK) % 40 == 0 || off >= ft9201_fw_size)
                LOG("fw: %lu / %lu bytes", off, ft9201_fw_size);
        }
    }
    LOG("firmware upload 完了");

    /* --- MCU config (fw を起動させるレジスタ列) --- */
    LOG("--- MCU config (AFE writes) ---");
    STEP("reg 0x01=0x01", REQ_AFE_WRITE, 0x01, 0x01);
    STEP("reg 0x41=0x0f", REQ_AFE_WRITE, 0x0f, 0x41);
    STEP("reg 0x30=0xbb", REQ_AFE_WRITE, 0xbb, 0x30);
    LOG("reg 0x30 read (ignore)");
    r = ft_usb_ctrl_in(f->usb, REQ_AFE_READ, 0, 0x30, buf, 1, &done);
    if (r != 0) return -1;
    STEP("reg 0x22=0x00", REQ_AFE_WRITE, 0x00, 0x22);
    STEP("reg 0x23=0x0e", REQ_AFE_WRITE, 0x0e, 0x23);
    STEP("reg 0x54=0x01 (SwitchNextSensorWorkMode)", REQ_AFE_WRITE, 0x01, 0x54);
    STEP("reg 0x1f=0x01 (power on)", REQ_AFE_WRITE, 0x01, 0x1f);
    STEP("reg 0x1e=0x01 (power on)", REQ_AFE_WRITE, 0x01, 0x1e);

    /* --- reset & MCU poll --- */
    LOG("--- sensor reset & MCU poll ---");
    STEP("sensor reset (0x40)", REQ_SENSOR_RESET, 0, 0);
    usleep(160 * 1000);
    wake(f);
    wake(f);

    {
        int polls = 0;
        for (;;) {
            usleep(2 * 1000);
            if (ft9201_reg_read(f, REG_MCU_STATUS, buf, 2) != 0) {
                LOG("!! MCU status read 失敗");
                return -1;
            }
            polls++;
            if (buf[0] == 0xa5 && buf[1] == 0x5a) {
                LOG("MCU ready after %d polls ✅ (a5 5a)", polls);
                break;
            }
            if (polls > 200) {
                LOG("!! MCU did not boot (200 polls) — センサーを抜き差ししてください");
                return -1;
            }
        }
    }

    /* --- dimensions --- */
    if (ft9201_reg_read(f, REG_DIM_W, buf, 4) != 0) return -1;
    f->width = buf[0] ? buf[0] : FT_DEFAULT_WIDTH;
    if (ft9201_reg_read(f, REG_DIM_H, buf, 4) != 0) return -1;
    f->height = buf[0] ? buf[0] : FT_DEFAULT_HEIGHT;
    LOG("sensor dimensions: %ux%u", f->width, f->height);

    /* --- variant 2/3 (FT9348W) AFE power-on --- */
    if (f->sensor_variant == 2 || f->sensor_variant == 3) {
        LOG("--- variant power-on (type %u) ---", f->sensor_variant);
        STEP("variant cfg1 (0x22=0x00)", REQ_AFE_WRITE, 0x00, REG_VARIANT_CFG1);
        STEP("variant cfg2 (0x23=0x0e)", REQ_AFE_WRITE, 0x0e, REG_VARIANT_CFG2);
        STEP("power 0x1f=0x01", REQ_AFE_WRITE, 0x01, REG_POWER_1F);
        STEP("power 0x1e=0x01", REQ_AFE_WRITE, 0x01, REG_POWER_1E);
    }

    /* 画像バッファ */
    size_t img_bytes = (size_t)f->width * f->height;
    f->background_buf = calloc(1, img_bytes);
    if (!f->background_buf) return -1;

    LOG("=== bring-up 完了 ✅ ===");
    return 0;
}

/* ================================================================ */
/* scan: 背景取得 / 指待ち / 画像取得                               */
/* ================================================================ */

/* bulk IN で 1 フレーム (w*h+2) を読む。buf は w*h+2 バイト以上。 */
static int read_frame(Ft9201 *f, uint8_t *buf, uint32_t timeout_ms) {
    uint32_t len = (uint32_t)f->width * f->height + 2;
    if (ft_usb_bulk_read(f->usb, buf, &len, timeout_ms) != 0) return -1;
    if (len < (uint32_t)f->width * f->height + 2) {
        LOG("!! short read: %u < %u", len, (uint32_t)f->width * f->height + 2);
        return -2;
    }
    return 0;
}

/* arm シーケンス + bulk size + 読み込み */
static int capture_frame(Ft9201 *f, uint8_t *buf, uint32_t timeout_ms) {
    if (ft_usb_ctrl_out(f->usb, REQ_BULK_ARM, 0xff, 0) != 0) return -1;
    usleep(20 * 1000);
    if (ft_usb_ctrl_out(f->usb, REQ_BULK_ARM, 0x03, 0) != 0) return -2;
    if (ft_usb_ctrl_out(f->usb, REQ_BULK_SIZE,
                        (uint16_t)((uint32_t)f->width * f->height + 2), 0x3400) != 0)
        return -3;
    return read_frame(f, buf, timeout_ms);
}

int ft9201_capture_background(Ft9201 *f) {
    size_t img_bytes = (size_t)f->width * f->height;
    uint8_t *buf = malloc(img_bytes + 2);
    if (!buf) return -1;
    if (capture_frame(f, buf, FT_BG_BULK_TIMEOUT_MS) != 0) {
        free(buf);
        f->has_background = 0;
        return -1;
    }
    memcpy(f->background_buf, buf + 2, img_bytes);
    f->has_background = 1;
    free(buf);
    return 0;
}

int ft9201_wait_finger(Ft9201 *f, int timeout_ms) {
    uint8_t buf[4];
    int waited = 0;
    for (;;) {
        if (ft9201_reg_read(f, REG_FINGER, buf, 4) != 0) return -1;
        if (buf[0] == 0xa0) return 0;
        usleep(20 * 1000);
        waited += 20;
        if (waited >= timeout_ms) return -2;
    }
}

int ft9201_wait_lift(Ft9201 *f, int timeout_ms) {
    uint8_t buf[4];
    int waited = 0;
    for (;;) {
        if (ft9201_reg_read(f, REG_FINGER, buf, 4) != 0) return -1;
        if (buf[0] != 0xa0) return 0;
        usleep(20 * 1000);
        waited += 20;
        if (waited >= timeout_ms) return -2;
    }
}

int ft9201_capture_image(Ft9201 *f, uint8_t *img, uint8_t quality[2]) {
    size_t img_bytes = (size_t)f->width * f->height;
    uint8_t *buf = malloc(img_bytes + 2);
    if (!buf) return -1;

    if (capture_frame(f, buf, FT_BULK_TIMEOUT_MS) != 0) {
        free(buf);
        return -1;
    }

    quality[0] = buf[0];
    quality[1] = buf[1];
    memcpy(img, buf + 2, img_bytes);

    /* FPN 補正 + ヒストグラム伸長 (SCAN_SUBMIT と同じ処理) */
    if (f->has_background) {
        for (size_t i = 0; i < img_bytes; i++) {
            int v = (int)img[i] - (int)f->background_buf[i] + 128;
            img[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    uint8_t lo = 255, hi = 0;
    for (size_t i = 0; i < img_bytes; i++) {
        if (img[i] < lo) lo = img[i];
        if (img[i] > hi) hi = img[i];
    }
    if (hi > lo) {
        for (size_t i = 0; i < img_bytes; i++)
            img[i] = (uint8_t)(((unsigned)(img[i] - lo) * 255) / (unsigned)(hi - lo));
    }

    free(buf);
    return 0;
}
