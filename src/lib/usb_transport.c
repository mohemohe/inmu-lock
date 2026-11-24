/*
 * usb_transport.c — 検証D (T1): IOKit ユーザースペース USB トランスポートの実装。
 *
 * 設計メモ:
 *   - control 転送はデバイスインターフェース (EP0) 経由、bulk はインターフェース
 *     インターフェース経由 (IOUSBLib の伝統的な COM 形式 API)。
 *   - インターフェースは macOS 27 SDK の最新 kIOUSB{Device,Interface}InterfaceID942
 *     を使用 (古い kIOUSBDeviceInterfaceID マクロは ID100 を指すため明示的に 942 を使う)。
 *   - descriptor の解析は IOKit の構造体に依存せず自前の packed 構造体で行う
 *     (USB 標準レイアウト、エンディアンは little を手動で読む)。
 */

#include "usb_transport.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/USB.h>
#include <IOKit/usb/USBSpec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- USB descriptor (標準レイアウト、自前定義) ---- */
#pragma pack(push, 1)
typedef struct {
    uint8_t bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes,
        bMaxPower;
} cfg_desc_t;

typedef struct {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting,
        bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol,
        iInterface;
} intf_desc_t;

typedef struct {
    uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval, bRefresh, bSynchAddress;
} ep_desc_t;
#pragma pack(pop)

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ================================================================ */
/* 検索                                                            */
/* ================================================================ */

int ft_usb_find(Ft9201Usb *u) {
    memset(u, 0, sizeof *u);

    /* クラスのみで列挙し、VID/PID はプロパティを読んで手動フィルタする
     * (プロパティマッチングの型の揺れに依存しない) */
    CFMutableDictionaryRef matching = IOServiceMatching("IOUSBHostInterface");
    if (!matching) return -1;

    io_iterator_t iter = MACH_PORT_NULL;
    IOReturn kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter);
    if (kr != kIOReturnSuccess) return -2;

    io_service_t intf = MACH_PORT_NULL;
    while ((intf = IOIteratorNext(iter)) != MACH_PORT_NULL) {
        CFTypeRef vvid = IORegistryEntryCreateCFProperty(intf, CFSTR("idVendor"),
                                                         kCFAllocatorDefault, 0);
        CFTypeRef vpid = IORegistryEntryCreateCFProperty(intf, CFSTR("idProduct"),
                                                         kCFAllocatorDefault, 0);
        int vid = -1, pid = -1;
        if (vvid) CFNumberGetValue(vvid, kCFNumberIntType, &vid);
        if (vpid) CFNumberGetValue(vpid, kCFNumberIntType, &pid);
        if (vvid) CFRelease(vvid);
        if (vpid) CFRelease(vpid);
        if (vid == FT_VID && pid == FT_PID) {
            u->intf_service = intf;
            break;
        }
        IOObjectRelease(intf);
    }
    IOObjectRelease(iter);
    if (u->intf_service == MACH_PORT_NULL) return -3;

    /* 親デバイス (IOUSBHostDevice) を取得 */
    IOReturn pr =
        IORegistryEntryGetParentEntry(u->intf_service, kIOServicePlane, &u->device_service);
    if (pr != kIOReturnSuccess || u->device_service == MACH_PORT_NULL) {
        return -4;
    }
    return 0;
}

/* ================================================================ */
/* オープン / クローズ                                              */
/* ================================================================ */

static int open_device_interface(Ft9201Usb *u) {
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;

    IOReturn kr = IOCreatePlugInInterfaceForService(
        u->device_service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
        &plugin, &score);
    if (kr != kIOReturnSuccess || !plugin) {
        fprintf(stderr, "[usb] IOCreatePlugInInterfaceForService(device): 0x%x\n", kr);
        return -1;
    }
    kr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID942),
                                   (LPVOID *)&u->dev);
    IODestroyPlugInInterface(plugin);
    if (kr != kIOReturnSuccess || !u->dev) {
        fprintf(stderr, "[usb] QueryInterface(device 942): 0x%x\n", kr);
        return -2;
    }

    kr = (*u->dev)->USBDeviceOpen(u->dev);
    if (kr == kIOReturnExclusiveAccess) {
        /* Seize (排他的オープン奪取) を試みる */
        kr = (*u->dev)->USBDeviceOpenSeize(u->dev);
        if (kr == kIOReturnExclusiveAccess) {
            fprintf(stderr, "[usb] USBDeviceOpen: 他のプロセス (inmu-cored) が"
                    "センサーを排他オープンしています\n"
                    "    sudo launchctl bootout system/com.mohemohe.inmu-lock.cored\n"
                    "    で停止してから再実行してください\n");
            return -3;
        }
    }
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "[usb] USBDeviceOpen: 0x%x\n", kr);
        return -3;
    }
    return 0;
}

static int open_interface(Ft9201Usb *u) {
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;

    IOReturn kr = IOCreatePlugInInterfaceForService(
        u->intf_service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID,
        &plugin, &score);
    if (kr != kIOReturnSuccess || !plugin) {
        fprintf(stderr, "[usb] IOCreatePlugInInterfaceForService(intf): 0x%x\n", kr);
        return -1;
    }
    kr = (*plugin)->QueryInterface(plugin,
                                   CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID942),
                                   (LPVOID *)&u->intf);
    IODestroyPlugInInterface(plugin);
    if (kr != kIOReturnSuccess || !u->intf) {
        fprintf(stderr, "[usb] QueryInterface(intf 942): 0x%x\n", kr);
        return -2;
    }

    kr = (*u->intf)->USBInterfaceOpen(u->intf);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "[usb] USBInterfaceOpen: 0x%x\n", kr);
        return -3;
    }

    /* パイプの特定: bNumEndpoints 分の pipeRef を調べ、bulk IN/OUT を探す */
    UInt8 nend = 0;
    kr = (*u->intf)->GetNumEndpoints(u->intf, &nend);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "[usb] GetNumEndpoints: 0x%x\n", kr);
        return -4;
    }
    u->num_endpoints = (uint8_t)nend;
    printf("[usb] interface 0: %u endpoints\n", nend);

    for (UInt8 pipeRef = 1; pipeRef <= nend; pipeRef++) {
        UInt8 direction = 0, number = 0, transferType = 0, interval = 0;
        UInt16 maxPacket = 0;
        kr = (*u->intf)->GetPipeProperties(u->intf, pipeRef, &direction, &number,
                                          &transferType, &maxPacket, &interval);
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "[usb] GetPipeProperties(%u): 0x%x\n", pipeRef, kr);
            continue;
        }
        const char *dir_s = direction == kUSBIn ? "IN " : "OUT";
        const char *type_s = transferType == kUSBBulk   ? "bulk"
                             : transferType == kUSBInterrupt ? "intr"
                             : transferType == kUSBControl  ? "ctrl"
                                                           : "isoc";
        printf("[usb] pipeRef %u: EP %#04x %s %s, maxPacket=%u\n", pipeRef,
               (direction == kUSBIn ? 0x80 : 0x00) | number, dir_s, type_s, maxPacket);

        if (transferType == kUSBBulk && direction == kUSBIn &&
            number == (FT_EP_IN_ADDR & 0x0f)) {
            u->pipe_in = (uint8_t)pipeRef;
            u->max_packet_in = maxPacket;
        }
        if (transferType == kUSBBulk && direction == kUSBOut &&
            number == (FT_EP_OUT_ADDR & 0x0f)) {
            u->pipe_out = (uint8_t)pipeRef;
            u->max_packet_out = maxPacket;
        }
    }

    if (!u->pipe_in || !u->pipe_out) {
        fprintf(stderr, "[usb] bulk pipe が見つからない (in=%u out=%u)\n", u->pipe_in,
                u->pipe_out);
        return -5;
    }
    printf("[usb] bulk pipes: IN=pipeRef %u (EP %#04x, maxPkt %u), "
           "OUT=pipeRef %u (EP %#04x, maxPkt %u)\n",
           u->pipe_in, FT_EP_IN_ADDR, u->max_packet_in, u->pipe_out, FT_EP_OUT_ADDR,
           u->max_packet_out);
    return 0;
}

int ft_usb_open(Ft9201Usb *u) {
    if (!u->intf_service || !u->device_service) {
        fprintf(stderr, "[usb] ft_usb_find を先に呼ぶこと\n");
        return -1;
    }
    if (open_device_interface(u) != 0) return -2;
    if (open_interface(u) != 0) return -3;
    return 0;
}

void ft_usb_close(Ft9201Usb *u) {
    if (u->intf) {
        (*u->intf)->USBInterfaceClose(u->intf);
        (*u->intf)->Release(u->intf);
        u->intf = NULL;
    }
    if (u->dev) {
        (*u->dev)->USBDeviceClose(u->dev);
        (*u->dev)->Release(u->dev);
        u->dev = NULL;
    }
    if (u->intf_service != MACH_PORT_NULL) {
        IOObjectRelease(u->intf_service);
        u->intf_service = MACH_PORT_NULL;
    }
    if (u->device_service != MACH_PORT_NULL) {
        IOObjectRelease(u->device_service);
        u->device_service = MACH_PORT_NULL;
    }
}

/* ================================================================ */
/* control request                                                  */
/* ================================================================ */

/* vendor request: bmRequestType = 0x40 (vendor, device, OUT) / 0xC0 (... IN) */
int ft_usb_ctrl_out(Ft9201Usb *u, uint8_t request, uint16_t value, uint16_t index) {
    if (!u->dev) return -1;
    IOUSBDevRequestTO req;
    memset(&req, 0, sizeof req);
    req.bmRequestType = 0x40; /* kUSBVendor | kUSBDevice | kUSBOut */
    req.bRequest = request;
    req.wValue = value;
    req.wIndex = index;
    req.wLength = 0;
    req.pData = NULL;
    req.wLenDone = 0;
    req.noDataTimeout = 5000;
    req.completionTimeout = 5000;
    IOReturn kr = (*u->dev)->DeviceRequestTO(u->dev, &req);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "[usb] ctrl_out(req=0x%02x val=0x%04x idx=0x%04x): 0x%x\n",
                request, value, index, kr);
        return (int)kr;
    }
    return 0;
}

int ft_usb_ctrl_in(Ft9201Usb *u, uint8_t request, uint16_t value, uint16_t index,
                   void *buf, uint16_t len, uint16_t *done) {
    if (!u->dev) return -1;
    IOUSBDevRequestTO req;
    memset(&req, 0, sizeof req);
    req.bmRequestType = 0xC0; /* kUSBVendor | kUSBDevice | kUSBIn */
    req.bRequest = request;
    req.wValue = value;
    req.wIndex = index;
    req.wLength = len;
    req.pData = buf;
    req.wLenDone = 0;
    req.noDataTimeout = 5000;
    req.completionTimeout = 5000;
    IOReturn kr = (*u->dev)->DeviceRequestTO(u->dev, &req);
    if (done) *done = (uint16_t)req.wLenDone;
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "[usb] ctrl_in(req=0x%02x val=0x%04x idx=0x%04x len=%u): 0x%x\n",
                request, value, index, len, kr);
        return (int)kr;
    }
    return 0;
}

/* ================================================================ */
/* bulk 転送                                                        */
/* ================================================================ */

int ft_usb_bulk_write(Ft9201Usb *u, const void *buf, uint32_t len, uint32_t timeout_ms) {
    if (!u->intf || !u->pipe_out) return -1;
    IOReturn kr = (*u->intf)->WritePipeTO(u->intf, u->pipe_out, (void *)buf, len,
                                          timeout_ms, timeout_ms);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "[usb] bulk_write(len=%u): 0x%x\n", len, kr);
        return (int)kr;
    }
    return 0;
}

int ft_usb_bulk_read(Ft9201Usb *u, void *buf, uint32_t *len, uint32_t timeout_ms) {
    if (!u->intf || !u->pipe_in) return -1;
    IOReturn kr = (*u->intf)->ReadPipeTO(u->intf, u->pipe_in, buf, len, timeout_ms,
                                         timeout_ms);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "[usb] bulk_read(len=%u): 0x%x\n", *len, kr);
        return (int)kr;
    }
    return 0;
}

/* ================================================================ */
/* descriptor                                                       */
/* ================================================================ */

int ft_usb_get_string(Ft9201Usb *u, uint8_t index, char *out, size_t outsz) {
    uint8_t buf[256];
    if (!u->dev) return -1;

    IOUSBDevRequestTO req;
    memset(&req, 0, sizeof req);
    req.bmRequestType = 0x80;               /* standard IN, device */
    req.bRequest = kUSBRqGetDescriptor;     /* 6 */
    req.wValue = (kUSBStringDesc << 8) | index;
    req.wIndex = 0x0409;                    /* langID: en-US */
    req.wLength = sizeof buf;
    req.pData = buf;
    req.noDataTimeout = 1000;
    req.completionTimeout = 1000;

    IOReturn kr = (*u->dev)->DeviceRequestTO(u->dev, &req);
    if (kr != kIOReturnSuccess || req.wLenDone < 2 || buf[1] != kUSBStringDesc) {
        return -2;
    }
    /* UTF-16LE → ASCII風 (表示用) */
    size_t n = (buf[0] - 2) / 2;
    if (n >= outsz) n = outsz - 1;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = buf[2 + i * 2] | (buf[3 + i * 2] << 8);
        out[i] = (c < 128) ? (char)c : '?';
    }
    out[n] = 0;
    return 0;
}

int ft_usb_dump_config(Ft9201Usb *u) {
    if (!u->dev) return -1;
    IOUSBConfigurationDescriptorPtr cfg = NULL;
    IOReturn kr = (*u->dev)->GetConfigurationDescriptorPtr(u->dev, 0, &cfg);
    if (kr != kIOReturnSuccess || !cfg) {
        fprintf(stderr, "[usb] GetConfigurationDescriptorPtr: 0x%x\n", kr);
        return -1;
    }

    const uint8_t *p = (const uint8_t *)cfg;
    uint16_t total = rd16(p + 2);
    printf("[usb] config descriptor: totalLength=%u, bNumInterfaces=%u, "
           "iConfiguration=%u\n",
           total, cfg->bNumInterfaces, cfg->iConfiguration);

    uint16_t off = 0;
    const intf_desc_t *cur_intf = NULL;
    while (off + 2 <= total) {
        uint8_t bLength = p[off];
        uint8_t bType = p[off + 1];
        if (bLength < 2) break; /* 異常 */

        if (bType == 0x04 && bLength >= 9) { /* interface */
            const intf_desc_t *d = (const intf_desc_t *)(p + off);
            printf("[usb] interface %u alt %u: class=%#x subclass=%#x proto=%#x, "
                   "%u endpoints\n",
                   d->bInterfaceNumber, d->bAlternateSetting, d->bInterfaceClass,
                   d->bInterfaceSubClass, d->bInterfaceProtocol, d->bNumEndpoints);
            cur_intf = d;
        } else if (bType == 0x05 && bLength >= 7) { /* endpoint */
            const ep_desc_t *e = (const ep_desc_t *)(p + off);
            const char *dir = (e->bEndpointAddress & 0x80) ? "IN " : "OUT";
            const char *type = (e->bmAttributes & 0x03) == 2   ? "bulk"
                               : (e->bmAttributes & 0x03) == 3 ? "intr"
                               : (e->bmAttributes & 0x03) == 1 ? "isoc"
                                                               : "ctrl";
            printf("[usb]   EP %#04x %s %s maxPacket=%u interval=%u (intf %u)\n",
                   e->bEndpointAddress, dir, type, rd16((const uint8_t *)&e->wMaxPacketSize),
                   e->bInterval, cur_intf ? cur_intf->bInterfaceNumber : 255);
        }
        off += bLength;
    }
    return 0;
}
