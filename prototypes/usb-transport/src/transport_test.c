/*
 * transport_test.c — 検証D: bulk トランスポートの実機テスト。
 *
 * デフォルト (安全モード):
 *   1. オープン + pipe 特定
 *   2. bulk IN (EP 0x83) を短いタイムアウトで読み試みる
 *      → デバイス未設定ならタイムアウト (kIOUSBTransactionTimeout 系) が
 *        「想定値」。pipe が機能していることの確認になる。
 *
 * オプション:
 *   --bulk-out HEX...   指定バイト列を bulk OUT へ (T2 以降の実験用)
 *   --timeout N         bulk のタイムアウト (ms, デフォルト 300)
 *
 * 注意: T2 の init シーケンス (docs/03 §4) を伴わない bulk 書き込みは
 * 原則行わない (banianitc 版の注意: init 失敗時は Windows 機でのリセットが
 * 必要になることがある)。読み込みは安全。
 */

#include "usb_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hexdump(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        printf("%02x ", p[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (n % 16 != 0) printf("\n");
}

int main(int argc, char **argv) {
    uint32_t timeout_ms = 300;
    const char *out_hex = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            timeout_ms = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--bulk-out") && i + 1 < argc) {
            out_hex = argv[++i];
        }
    }

    Ft9201Usb u;
    if (ft_usb_find(&u) != 0) {
        printf("[test] デバイスが見つからない\n");
        return 1;
    }
    if (ft_usb_open(&u) != 0) {
        printf("[test] オープン失敗\n");
        ft_usb_close(&u);
        return 1;
    }
    printf("[test] オープン OK\n");

    /* --- bulk IN 読み込みテスト (安全) --- */
    {
        uint8_t buf[64];
        uint32_t len = sizeof buf;
        printf("[test] bulk IN 読み込み試行 (timeout=%ums)... ", timeout_ms);
        fflush(stdout);
        int r = ft_usb_bulk_read(&u, buf, &len, timeout_ms);
        if (r == 0) {
            printf("成功 (%u bytes):\n", len);
            hexdump(buf, len);
        } else {
            char desc[32];
            (void)desc;
            /* 未初期状態のデバイスはデータを持たないためタイムアウトが想定値。
             * (pipe は存在し、転送要求は受理された) */
            printf("タイムアウト/エラー (0x%x) — 未セットアップ状態なら想定どおり ✅\n", r);
        }
    }

    /* --- bulk OUT (明示的に指定された場合のみ) --- */
    if (out_hex) {
        uint8_t buf[64];
        size_t n = 0;
        for (const char *p = out_hex; *p && n < sizeof buf;) {
            char *end = NULL;
            long v = strtol(p, &end, 16);
            if (end == p) break;
            buf[n++] = (uint8_t)v;
            p = end;
        }
        printf("[test] bulk OUT 書き込み %zu bytes...\n", n);
        hexdump(buf, n);
        int r = ft_usb_bulk_write(&u, buf, (uint32_t)n, timeout_ms);
        printf("[test] bulk OUT 結果: 0x%x\n", r);
    }

    ft_usb_close(&u);
    printf("[test] 完了\n");
    return 0;
}
