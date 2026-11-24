/*
 * capture_test.c — 検証D (T2): FT9201 bring-up + 96×96 画像取得の実機テスト。
 *
 *   ./capture_test              bring-up → 背景(FPN) → 指待ち → 画像取得 → PGM保存
 *   ./capture_test --bg         bring-up → 背景のみ取得して保存 (指なし・非対話)
 *   ./capture_test --loop N     画像を N 枚連続取得 (指を置き直すたびに1枚)
 *   ./capture_test --out PFX    出力ファイル接頭辞 (default: capture)
 *
 * 出力: <prefix>.pgm (P5, 96×96 8bit grayscale — Preview で開ける)
 */

#include "ft9201.h"
#include "usb_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int save_pgm(const char *path, const uint8_t *img, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(img, 1, (size_t)w * h, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    int bg_only = 0;
    int loop = 0;
    int raw = 0;
    const char *prefix = "capture";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bg")) bg_only = 1;
        else if (!strcmp(argv[i], "--loop") && i + 1 < argc) loop = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "--raw")) raw = 1;
    }

    Ft9201Usb u;
    if (ft_usb_find(&u) != 0) {
        fprintf(stderr, "[capture] デバイスが見つからない\n");
        return 1;
    }
    if (ft_usb_open(&u) != 0) {
        fprintf(stderr, "[capture] オープン失敗\n");
        ft_usb_close(&u);
        return 1;
    }

    Ft9201 ft;
    fprintf(stderr, "=== FT9201 bring-up (T2) ===\n");
    if (ft9201_init(&ft, &u) != 0) {
        fprintf(stderr, "[capture] bring-up 失敗 — センサーを抜き差しして再試行\n");
        ft_usb_close(&u);
        return 1;
    }

    /* 背景 (FPN) 取得 — 指なしで取得できる bulk 画像経路の検証 */
    fprintf(stderr, "=== 背景フレーム取得 ===\n");
    if (ft9201_capture_background(&ft) != 0) {
        fprintf(stderr, "[capture] 背景取得失敗 (bulk 画像経路の問題)\n");
        ft_usb_close(&u);
        return 1;
    }
    fprintf(stderr, "[capture] 背景取得 OK (bulk IN %d bytes) ✅\n",
            ft.width * ft.height + 2);
    {
        char path[256];
        snprintf(path, sizeof path, "%s_bg.pgm", prefix);
        save_pgm(path, ft.background_buf, ft.width, ft.height);
        fprintf(stderr, "[capture] 背景を保存: %s\n", path);
    }

    if (bg_only) {
        fprintf(stderr, "[capture] --bg モード完了 (指なしで bulk 画像経路まで実証) ✅\n");
        ft_usb_close(&u);
        return 0;
    }

    uint8_t *img = malloc((size_t)ft.width * ft.height);
    uint8_t quality[2] = {0, 0};
    int count = 0;

    do {
        fprintf(stderr, "=== 指をセンサーに置いてください (最大60秒待機) ===\n");
        if (ft9201_wait_finger(&ft, 60000) != 0) {
            fprintf(stderr, "[capture] 指が検知されない (タイムアウト)\n");
            break;
        }
        fprintf(stderr, "[capture] 指を検知 — セットリング 50ms...\n");
        usleep(50 * 1000);

        if (ft9201_capture_image(&ft, img, quality) != 0) {
            fprintf(stderr, "[capture] 画像取得失敗\n");
            break;
        }
        count++;
        fprintf(stderr, "[capture] 画像取得 OK (quality=0x%02x 0x%02x)\n", quality[0],
                quality[1]);

        char path[256];
        snprintf(path, sizeof path, "%s_%02d.pgm", prefix, count);
        save_pgm(path, img, ft.width, ft.height);
        fprintf(stderr, "[capture] 保存: %s (%dx%d) — Preview で開けます\n", path,
                ft.width, ft.height);
        if (raw) {
            /* FPN補正+伸長済みの生画像 (engine_test への入力用) */
            snprintf(path, sizeof path, "%s_%02d.bin", prefix, count);
            FILE *rf = fopen(path, "wb");
            if (rf) {
                fwrite(img, 1, (size_t)ft.width * ft.height, rf);
                fclose(rf);
                fprintf(stderr, "[capture] raw 保存: %s\n", path);
            }
        }

        fprintf(stderr, "[capture] 指を離してください...\n");
        ft9201_wait_lift(&ft, 10000);
    } while (count < loop);

    free(img);
    ft_usb_close(&u);
    fprintf(stderr, "[capture] 完了 (%d 枚)\n", count);
    return count > 0 ? 0 : 1;
}
