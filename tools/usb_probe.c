/*
 * usb_probe.c — 検証D: FT9201 の USB 構成 (エンドポイント) を実機から確認する。
 *
 *   1. VID/PID で IOUSBHostInterface / IOUSBHostDevice を検索
 *   2. レジストリプロパティ表示
 *   3. デバイス + インターフェースをオープン
 *   4. コンフィグディスクリプタを解析してエンドポイント一覧を表示
 *      (T1 の主目的: bulk IN/OUT のアドレス確定)
 *   5. 文字列ディスクリプタ取得 (標準 control request の動作確認)
 */

#include "usb_transport.h"

#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

static void print_registry_props(Ft9201Usb *u) {
    const char *keys[] = {"USB Product Name", "USB Vendor Name", "idVendor",
                          "idProduct", "bcdDevice", "locationID", "sessionID"};
    printf("[probe] --- レジストリプロパティ (device) ---\n");
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        CFStringRef keyRef =
            CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
        if (!keyRef) continue;
        CFTypeRef v = IORegistryEntryCreateCFProperty(u->device_service, keyRef,
                                                      kCFAllocatorDefault, 0);
        CFRelease(keyRef);
        if (!v) continue;
        char buf[256] = {0};
        if (CFGetTypeID(v) == CFStringGetTypeID()) {
            CFStringGetCString((CFStringRef)v, buf, sizeof buf, kCFStringEncodingUTF8);
            printf("[probe] %s = %s\n", keys[i], buf);
        } else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
            long long n = 0;
            CFNumberGetValue(v, kCFNumberLongLongType, &n);
            printf("[probe] %s = %lld (0x%llx)\n", keys[i], n, n);
        } else if (CFGetTypeID(v) == CFDataGetTypeID()) {
            CFDataRef d = (CFDataRef)v;
            const uint8_t *bytes = CFDataGetBytePtr(d);
            CFIndex len = CFDataGetLength(d);
            printf("[probe] %s = data(%ld bytes)", keys[i], (long)len);
            for (CFIndex j = 0; j < len && j < 16; j++) printf(" %02x", bytes[j]);
            printf("\n");
        }
        CFRelease(v);
    }
}

int main(void) {
    Ft9201Usb u;
    printf("[probe] FT9201 (VID=%04x PID=%04x) を検索...\n", FT_VID, FT_PID);

    int r = ft_usb_find(&u);
    if (r != 0) {
        printf("[probe] 見つからない (r=%d)。センサーが挿さっているか確認。\n", r);
        return 1;
    }
    printf("[probe] 見つかった: intf=%#x device=%#x\n", u.intf_service, u.device_service);
    print_registry_props(&u);

    r = ft_usb_open(&u);
    if (r != 0) {
        printf("[probe] オープン失敗 (r=%d)\n", r);
        ft_usb_close(&u);
        return 1;
    }

    r = ft_usb_dump_config(&u);
    if (r != 0) printf("[probe] descriptor ダンプ失敗\n");

    /* 標準 GET_DESCRIPTOR(string): control 経路の動作確認 */
    char s[128];
    for (uint8_t idx = 1; idx <= 3; idx++) {
        if (ft_usb_get_string(&u, idx, s, sizeof s) == 0) {
            printf("[probe] string[%u] = \"%s\"\n", idx, s);
        } else {
            printf("[probe] string[%u] = (取得できず)\n", idx);
        }
    }

    ft_usb_close(&u);
    printf("[probe] 完了\n");
    return 0;
}
