/*
 * ft9201.h — 検証D (T2): FT9201 bring-up (fw転送・initシーケンス) と画像取得。
 *
 * シーケンスの根拠: OMGrant/ft9201-libfprint src/ft9201.c (LGPL-2.1) の
 * init/scan 状態機械を同期版として移植 (docs/03 §4)。
 */

#ifndef FT9201_H
#define FT9201_H

#include <stdint.h>
#include "usb_transport.h"

/* vendor control request コード (Linux実装と同一) */
#define REQ_WAKEUP 0x22        /* OUT val=0x70 idx=0x70 */
#define REQ_HW_REG 0x03        /* OUT hw register write */
#define REQ_BULK_ARM 0x34      /* OUT arm / reset bulk endpoint */
#define REQ_BULK_SIZE 0x35     /* OUT bulk chunk size */
#define REQ_SUI_CMD 0x64       /* OUT SUI command */
#define REQ_SUI_READ 0x60      /* IN  SUI register readback */
#define REQ_STATUS 0x30        /* IN  sensor status */
#define REQ_MCU_DL_MODE 0x57   /* OUT enter MCU download mode */
#define REQ_SPI_WRITE 0x66     /* OUT SPI write (sensor type detection) */
#define REQ_SPI_READ 0x65      /* IN  SPI read */
#define REQ_AFE_READ 0x3a       /* IN  AFE register read (wIndex=reg) */
#define REQ_AFE_WRITE 0x3b     /* OUT AFE register write (wValue=val, wIndex=reg) */
#define REQ_SENSOR_RESET 0x40  /* OUT hardware MCU reset */

/* AFE レジスタ (Linux実装と同一) */
#define REG_DIM_W 0x14
#define REG_DIM_H 0x15
#define REG_MCU_STATUS 0x20 /* [0xa5,0x5a] → MCU ready */
#define REG_DATA_READY 0x30 /* 0xbb → image available */
#define REG_FINGER 0x1d    /* 0xa0 → finger present */
#define REG_VARIANT_CFG1 0x22
#define REG_VARIANT_CFG2 0x23
#define REG_POWER_1F 0x1f
#define REG_POWER_1E 0x1e

/* タイムアウト (Linux実装と同一) */
#define FT_CTRL_TIMEOUT_MS 5000
#define FT_BULK_TIMEOUT_MS 5000
#define FT_BG_BULK_TIMEOUT_MS 500

/* デフォルト寸法 */
#define FT_DEFAULT_WIDTH 96
#define FT_DEFAULT_HEIGHT 96

typedef struct {
    Ft9201Usb *usb;
    uint8_t width, height;
    uint8_t sensor_variant; /* SPI detection の型 (2/3 = FT9348W) */
    /* FPN 背景フレーム (capture 毎に最初に取得) */
    uint8_t *background_buf;
    int has_background;
} Ft9201;

/* 完全な init シーケンス (wake → SPI型判定 → fw転送 → AFE設定 → reset →
 * MCU ready poll → 寸法読み → 型別 power-on)。
 * 進捗は stderr に印字。成功で 0。 */
int ft9201_init(Ft9201 *f, Ft9201Usb *u);

/* AFE レジスタ読み書き (scan ループ用)。read は len バイト読み buf[0] が値。 */
int ft9201_reg_read(Ft9201 *f, uint8_t reg, uint8_t *buf, uint16_t len);
int ft9201_reg_write(Ft9201 *f, uint8_t reg, uint8_t value);

/* FPN 背景フレームを取得 (指なしで取得できる — bulk 画像経路の検証にも使える)。 */
int ft9201_capture_background(Ft9201 *f);

/* 指を待つ (reg 0x1d == 0xa0)。timeout_ms 内に検知で 0。 */
int ft9201_wait_finger(Ft9201 *f, int timeout_ms);

/* 指が離れるのを待つ (reg 0x1d != 0xa0)。 */
int ft9201_wait_lift(Ft9201 *f, int timeout_ms);

/* 画像を取得 (事前に capture_background 済みなら FPN 補正 + ヒストグラム伸長
 * を適用)。quality[2] にチップ品質バイトを返す。成功で 0。 */
int ft9201_capture_image(Ft9201 *f, uint8_t *img, uint8_t quality[2]);

#endif /* FT9201_H */
