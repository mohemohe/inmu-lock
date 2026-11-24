/*
 * usb_transport.h — 検証D (T1): FT9201 (VID 2808:93A9) の IOKit ユーザースペース
 * USB トランスポート。
 *
 * 提供機能:
 *   - サービス検索 (IOUSBHostInterface / IOUSBHostDevice を VID/PID で)
 *   - デバイス + インターフェースのオープン (control + bulk の準備)
 *   - vendor control request (T2 の bring-up シーケンスが全面的に使う)
 *   - bulk IN / OUT (タイムアウト付き)
 *   - コンフィグ descriptor のダンプ (エンドポイント確定)
 *
 * API の根拠 (macOS 27 SDK で確認済み):
 *   - IOUSBDeviceInterface942 / IOUSBInterfaceInterface942 (IOUSBLib.h)
 *   - kIOUSBDeviceUserClientTypeID / kIOUSBInterfaceUserClientTypeID
 *   - DeviceRequestTO / ReadPipeTO / WritePipeTO
 *   - Linux 実装 (OMGrant/ft9201-libfprint, LGPL-2.1-or-later) の定数:
 *       EP_OUT = 0x02 (bulk OUT), EP_IN = 0x83 (bulk IN)
 */

#ifndef FT_USB_TRANSPORT_H
#define FT_USB_TRANSPORT_H

#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdint.h>

#define FT_VID 0x2808
#define FT_PID 0x93a9

/* Linux 実装と同じエンドポイントアドレス (docs/03 §2) */
#define FT_EP_OUT_ADDR 0x02
#define FT_EP_IN_ADDR 0x83

typedef struct {
    /* サービス */
    io_service_t device_service; /* IOUSBHostDevice */
    io_service_t intf_service;   /* IOUSBHostInterface@0 */

    /* IOKit インターフェース */
    IOUSBDeviceInterface942 **dev;      /* EP0 control 用 */
    IOUSBInterfaceInterface942 **intf; /* bulk pipe 用 */

    /* パイプ (pipeRef: 1..bNumEndpoints) */
    uint8_t pipe_in;   /* EP 0x83 bulk IN */
    uint8_t pipe_out;  /* EP 0x02 bulk OUT */
    uint16_t max_packet_in;
    uint16_t max_packet_out;
    uint8_t num_endpoints;
} Ft9201Usb;

/* VID/PID でデバイスを検索。見つかれば 0。 */
int ft_usb_find(Ft9201Usb *u);

/* デバイス + インターフェース 0 をオープンし、bulk pipe を特定する。 */
int ft_usb_open(Ft9201Usb *u);

/* クローズ (べき等)。 */
void ft_usb_close(Ft9201Usb *u);

/* vendor control request (recipient=device)。
 * ctrl_out: 要求のみ (wLength=0)。ctrl_in: len バイト読む。
 * 成功で 0、エラーで IOReturn (KERN_RETURN from IOKit) を返す。 */
int ft_usb_ctrl_out(Ft9201Usb *u, uint8_t request, uint16_t value, uint16_t index);
int ft_usb_ctrl_in(Ft9201Usb *u, uint8_t request, uint16_t value, uint16_t index,
                   void *buf, uint16_t len, uint16_t *done);

/* bulk 転送 (タイムアウト ms)。成功で 0。
 * bulk_read の *len は IN/OUT (要求サイズ → 実読みバイト)。 */
int ft_usb_bulk_write(Ft9201Usb *u, const void *buf, uint32_t len, uint32_t timeout_ms);
int ft_usb_bulk_read(Ft9201Usb *u, void *buf, uint32_t *len, uint32_t timeout_ms);

/* 標準 GET_DESCRIPTOR(string) で文字列ディスクリプタを読む (検証用)。 */
int ft_usb_get_string(Ft9201Usb *u, uint8_t index, char *out, size_t outsz);

/* コンフィグディスクリプタを解析してエンドポイント一覧を stdout に印字。 */
int ft_usb_dump_config(Ft9201Usb *u);

#endif /* FT_USB_TRANSPORT_H */
