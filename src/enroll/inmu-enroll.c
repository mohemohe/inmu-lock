/*
 * inmu-enroll.c — inmu-lock: 指紋登録ツール (CLI)。
 *
 * 使い方:
 *   sudo ./build/inmu-enroll                     # /usr/local/etc/inmu-lock/template.bin へ
 *   sudo ./build/inmu-enroll --out PATH          # 任意のパスへ
 *   ./build/inmu-enroll --out /tmp/my.bin        # ユーザー領域へ (root不要)
 *   --samples N                                   # 受理目標を変更 (デフォルト 8)
 *
 * 受理目標について:
 *   Linux 版ドライバ (OMGrant/ft9201-libfprint) は固定 8 ステージでコミットする
 *   方式であり、その方式で genuine 3/3 MATCH / imposter 3/3 reject を検証済み。
 *   vendor engine の「完成報告」(coverage) は実測では常に 0 のまま届かないため、
 *   engine の報告を待たずに固定受理数でコミットする。
 *   却下 (重複/低品質) は受理数に含まれない。指の位置をずらすと受理されやすい。
 *   Ctrl-C で中断した場合も、その時点の受理でコミットする。
 */

#include "ft9201.h"
#include "usb_transport.h"
#include "ft_engine.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_OUT "/usr/local/etc/inmu-lock/template.bin"
#define DEFAULT_SAMPLES 8   /* Linux 版ドライバと同じ固定ステージ数 (検証C実績値) */
#define ENGINE_MAX_SAMPLES 18 /* engine 側の enroll_max_templates */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void log_step(const char *fmt, ...) {
    fprintf(stderr, "[enroll] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

int main(int argc, char **argv) {
    const char *out_path = DEFAULT_OUT;
    const char *engine_path = "/usr/local/libexec/inmu-lock/ftWbioEngineAdapter.dll";
    long samples = DEFAULT_SAMPLES;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine_path = argv[++i];
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) {
            samples = atol(argv[++i]);
            if (samples < 1) samples = 1;
            if (samples > ENGINE_MAX_SAMPLES) samples = ENGINE_MAX_SAMPLES;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr,
                    "usage: %s [--out template.bin] [--engine DLL] [--samples N]\n"
                    "  --samples: 受理目標 (1-%d, デフォルト %d)\n",
                    argv[0], ENGINE_MAX_SAMPLES, DEFAULT_SAMPLES);
            return 0;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* エンジン DLL がなければ開発用の位置にフォールバック */
    if (access(engine_path, R_OK) != 0) {
        const char *dev = "vendor/ftWbioEngineAdapter.dll";
        if (access(dev, R_OK) == 0) {
            engine_path = dev;
            log_step("開発用エンジンDLLを使用: %s", dev);
        }
    }

    log_step("=== 指紋登録 ===");
    log_step("出力: %s / エンジン: %s", out_path, engine_path);

    /* センサー bring-up */
    Ft9201Usb u;
    if (ft_usb_find(&u) != 0) {
        fprintf(stderr, "[enroll] FT9201 が見つかりません (接続を確認)\n");
        return 1;
    }
    if (ft_usb_open(&u) != 0) {
        fprintf(stderr, "[enroll] USB オープン失敗\n");
        return 1;
    }
    Ft9201 ft;
    if (ft9201_init(&ft, &u) != 0) {
        fprintf(stderr, "[enroll] bring-up 失敗 — センサーを抜き差しして再試行\n");
        ft_usb_close(&u);
        return 1;
    }
    if (ft9201_capture_background(&ft) != 0) {
        fprintf(stderr, "[enroll] 背景 (FPN) 取得失敗\n");
        ft_usb_close(&u);
        return 1;
    }
    log_step("センサー準備完了 (%ux%u) — 背景取得済み", ft.width, ft.height);

    /* matcher */
    if (ft_engine_open(engine_path) != 0) {
        fprintf(stderr, "[enroll] matcher DLL ロード失敗: %s\n", engine_path);
        ft_usb_close(&u);
        return 1;
    }
    ft_engine_enroll_begin();
    log_step("matcher 準備完了。指紋の登録を開始します。");

    uint8_t *img = malloc((size_t)ft.width * ft.height);
    uint8_t quality[2] = {0, 0};
    int accepted = 0, rejected = 0, complete = 0;

    log_step("受理目標: %ld 回 (却下はカウントしません。指の位置を少しずつ"
             "ずらすと受理されやすい。Ctrl-C で中断してもその時点で保存します)",
             samples);

    while (!g_stop && accepted < samples) {
        log_step("指をセンサーに置いてください [受理 %d/%ld ・ 却下 %d]",
                 accepted, samples, rejected);
        if (ft9201_wait_finger(&ft, 60000) != 0) {
            log_step("指が検知されない (タイムアウト) — 終了します");
            break;
        }
        usleep(50 * 1000); /* settle */

        if (ft9201_capture_image(&ft, img, quality) != 0) {
            log_step("!! 画像取得失敗 — 再試行");
            ft9201_wait_lift(&ft, 3000);
            continue;
        }

        uint32_t hr = ft_engine_accept(img, ft.width, ft.height, 4 /* enroll */);
        if (hr != 0) {
            rejected++;
            log_step("品質不足で却下 (hr=%#x) — 指の位置を変えて置き直してください", hr);
            ft9201_wait_lift(&ft, 3000);
            continue;
        }

        int st = ft_engine_enroll_update();
        if (st == 2) {
            rejected++;
            log_step("却下 (重複/低品質) — 少し指の位置をずらして置き直してください");
        } else {
            accepted++;
            log_step("受理 (%d/%ld) %s", accepted, samples,
                     st == 0 ? "— engine が完成を報告しました ✅"
                             : (accepted >= samples ? "— 受理目標に到達 ✅" : "— さらに採取します"));
        }
        ft9201_wait_lift(&ft, 5000);
        if (st == 0) {
            complete = 1;
            break;
        }
    }

    int rc = 1;
    if (accepted > 0) {
        if (complete) log_step("engine 完成報告によりコミットします");
        else if (g_stop) log_step("中断されました — この時点の受理でコミットします");
        else log_step("受理目標 (%ld) に到達 — Linux版ドライバと同じ固定ステージ方式で"
                      "コミットします", samples);
        uint8_t *tmpl = NULL;
        size_t tmpllen = 0;
        int cr = ft_engine_enroll_commit(&tmpl, &tmpllen);
        if (cr == 0 && tmpl) {
            /* 親ディレクトリを best-effort で作成 (/usr/local/etc/inmu-lock 等) */
            char dirbuf[512];
            snprintf(dirbuf, sizeof dirbuf, "%s", out_path);
            char *slash = strrchr(dirbuf, '/');
            if (slash && slash != dirbuf) {
                *slash = 0;
                mkdir(dirbuf, 0755); /* 既存なら失敗するが無害 */
            }
            FILE *f = fopen(out_path, "wb");
            if (f) {
                fwrite(tmpl, 1, tmpllen, f);
                fclose(f);
                chmod(out_path, 0600);
                log_step("テンプレート保存: %s (%zu bytes) ✅ (受理 %d / 却下 %d)",
                         out_path, tmpllen, accepted, rejected);
                rc = 0;
                log_step("デーモンへの反映: sudo launchctl kickstart -k system/com.mohemohe.inmu-lock.cored");
                log_step("(手動起動の場合は inmu-cored を再起動してください)");
            } else {
                fprintf(stderr, "[enroll] テンプレート書き込み失敗: %s (root が必要なら sudo で)\n",
                        out_path);
            }
            free(tmpl);
        } else {
            fprintf(stderr, "[enroll] 登録コミット失敗 (受理が足りない可能性)\n");
        }
    } else {
        fprintf(stderr, "[enroll] 1枚も受理されず終了\n");
    }

    free(img);
    ft_usb_close(&u);
    return rc;
}
