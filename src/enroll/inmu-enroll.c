/*
 * inmu-enroll.c — inmu-lock: 指紋登録ツール (CLI)。
 *
 * 使い方:
 *   sudo ./build/inmu-enroll                     # /usr/local/etc/inmu-lock/template.bin へ
 *   sudo ./build/inmu-enroll --out PATH          # 任意のパスへ
 *   ./build/inmu-enroll --out /tmp/my.bin        # ユーザー領域へ (root不要)
 *   --samples N                                   # 受理目標を変更 (デフォルト 8)
 *   --engine DLL                                  # matcher DLL を明示指定 (既定は INMU_ENGINE_DLL)
 *   --owner-uid N                                 # 登録オーナー (指紋の持ち主) の uid を明示指定
 *   --owner-file PATH                             # 登録オーナー uid の書き出し先を明示指定
 *
 * 登録オーナー uid:
 *   出力先が既定 (INMU_TEMPLATE_FILE) の場合、テンプレートと一緒に INMU_OWNER_FILE へ
 *   登録者の uid を 10 進 1 行で書く (inmu-cored はこの uid の要求だけを承認する)。
 *   登録者 uid は、root 実行時は sudo が設定する SUDO_UID、非 root なら getuid()。
 *   出力先が既定以外の時は --owner-file を指定した場合のみ書く。
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
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_OUT INMU_TEMPLATE_FILE
#define DEFAULT_ENGINE INMU_ENGINE_DLL
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

/* 10 進数字だけからなる uid 文字列を厳密に解釈する (符号・空白・接頭辞・空文字は不可)。
 * (uid_t)-1 は「変更なし」を意味する特殊値なので受け付けない。成功で 0。 */
static int parse_uid_strict(const char *s, uid_t *out) {
    if (!s || !*s) return -1;
    uint64_t v = 0;
    size_t n = 0;
    for (const char *p = s; *p; p++, n++) {
        if (*p < '0' || *p > '9' || n >= 10) return -1;
        v = v * 10 + (uint64_t)(*p - '0');
    }
    if (v >= UINT32_MAX) return -1;
    *out = (uid_t)v;
    return 0;
}

/* path と同じディレクトリに一時ファイル (O_CREAT|O_EXCL|O_NOFOLLOW, 0600) を作り、
 * data を全量書き込んで fsync する。成功で 0 を返し tmp に一時ファイル名を入れる
 * (呼び出し側が rename で確定するか unlink で破棄する)。失敗時は一時ファイルを残さない。
 * 最終的なモード mode は fchmod で設定する (umask に依存しない)。 */
static int write_tmp_file(const char *path, const void *data, size_t len, mode_t mode,
                          char *tmp, size_t tmpsz) {
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
        int n = snprintf(tmp, tmpsz, "%s.tmp.%ld.%08x", path, (long)getpid(), arc4random());
        if (n < 0 || (size_t)n >= tmpsz) {
            fprintf(stderr, "[enroll] パスが長すぎます: %s\n", path);
            return -1;
        }
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd < 0) {
        fprintf(stderr, "[enroll] 一時ファイルを作成できません: %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, p + done, len - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (w == 0) break; /* 進まない書き込みは失敗として扱う */
        done += (size_t)w;
    }
    int ok = (done == len);
    if (!ok) fprintf(stderr, "[enroll] 書き込み失敗 (%zu/%zu bytes): %s: %s\n", done, len, tmp,
                     strerror(errno));
    if (ok && fchmod(fd, mode) != 0) {
        fprintf(stderr, "[enroll] fchmod 失敗: %s: %s\n", tmp, strerror(errno));
        ok = 0;
    }
    if (ok && fsync(fd) != 0) {
        fprintf(stderr, "[enroll] fsync 失敗: %s: %s\n", tmp, strerror(errno));
        ok = 0;
    }
    if (close(fd) != 0 && ok) {
        fprintf(stderr, "[enroll] close 失敗: %s: %s\n", tmp, strerror(errno));
        ok = 0;
    }
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* テンプレート (0600) と、owner_path が非 NULL なら登録オーナー uid (0644) を保存する。
 * どちらも一時ファイルへ全量書いてから rename で置き換える (書きかけや、
 * シンボリックリンクの先を他人が読める/壊せる状態を作らない)。
 *
 * 2 つのファイルは同時には置き換えられないので、途中で失敗しても
 * 「新しいテンプレート + 古いオーナー」(= 別人の指紋で旧オーナーを承認) の組が
 * 残らない順序にする: 両方の一時ファイルを用意 → 旧 owner を削除 → テンプレートを
 * 置換 → owner を置換。途中で失敗すると owner が無い状態になり、inmu-cored は
 * 一切承認しない (fail-closed)。成功で 0。 */
static int save_enrollment(const char *out_path, const uint8_t *tmpl, size_t tmpllen,
                           const char *owner_path, uid_t owner_uid) {
    char tmp_t[PATH_MAX], tmp_o[PATH_MAX];
    if (write_tmp_file(out_path, tmpl, tmpllen, 0600, tmp_t, sizeof tmp_t) != 0) return -1;
    if (owner_path) {
        char line[32];
        int n = snprintf(line, sizeof line, "%u\n", (unsigned)owner_uid);
        if (n < 0 || (size_t)n >= sizeof line ||
            write_tmp_file(owner_path, line, (size_t)n, 0644, tmp_o, sizeof tmp_o) != 0) {
            unlink(tmp_t);
            return -1;
        }
        if (unlink(owner_path) != 0 && errno != ENOENT) {
            fprintf(stderr, "[enroll] 旧オーナーファイルを削除できません: %s: %s\n", owner_path,
                    strerror(errno));
            unlink(tmp_t);
            unlink(tmp_o);
            return -1;
        }
    }
    if (rename(tmp_t, out_path) != 0) {
        fprintf(stderr, "[enroll] テンプレートを配置できません: %s: %s\n", out_path,
                strerror(errno));
        unlink(tmp_t);
        if (owner_path) unlink(tmp_o);
        return -1;
    }
    if (owner_path && rename(tmp_o, owner_path) != 0) {
        fprintf(stderr, "[enroll] オーナーファイルを配置できません: %s: %s\n"
                        "[enroll] オーナー未登録のため指紋認証は無効です。登録をやり直してください\n",
                owner_path, strerror(errno));
        unlink(tmp_o);
        return -1;
    }
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--out template.bin] [--engine DLL] [--samples N]\n"
            "          [--owner-uid N] [--owner-file PATH]\n"
            "  --samples:    受理目標 (1-%d, デフォルト %d)\n"
            "  --engine:     matcher DLL (デフォルト %s)\n"
            "  --owner-uid:  登録オーナーの uid (デフォルト: sudo 実行時は SUDO_UID、それ以外は自分)\n"
            "  --owner-file: 登録オーナー uid の書き出し先\n"
            "                (--out が既定の時のデフォルト %s)\n",
            argv0, ENGINE_MAX_SAMPLES, DEFAULT_SAMPLES, DEFAULT_ENGINE, INMU_OWNER_FILE);
}

int main(int argc, char **argv) {
    const char *out_path = DEFAULT_OUT;
    const char *engine_path = DEFAULT_ENGINE;
    const char *owner_path = NULL;    /* --owner-file */
    const char *owner_uid_arg = NULL; /* --owner-uid */
    long samples = DEFAULT_SAMPLES;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine_path = argv[++i];
        else if (!strcmp(argv[i], "--owner-file") && i + 1 < argc) owner_path = argv[++i];
        else if (!strcmp(argv[i], "--owner-uid") && i + 1 < argc) owner_uid_arg = argv[++i];
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) {
            samples = atol(argv[++i]);
            if (samples < 1) samples = 1;
            if (samples > ENGINE_MAX_SAMPLES) samples = ENGINE_MAX_SAMPLES;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        else {
            /* 不明な引数や値の欠けたオプションを黙って無視しない
             * (例: 値なしの --out で既定のシステムテンプレートを上書きしてしまう) */
            fprintf(stderr, "[enroll] 不正な引数: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (!*out_path || strlen(out_path) >= PATH_MAX - 40) {
        fprintf(stderr, "[enroll] --out のパスが不正です\n");
        return 2;
    }

    /* 登録オーナー uid の書き出し先と値を、センサーに触る前に確定する。
     * 出力先が既定以外の時は --owner-file を指定した場合のみ書く。 */
    if (!owner_path && !strcmp(out_path, DEFAULT_OUT)) owner_path = INMU_OWNER_FILE;
    uid_t owner_uid = 0;
    if (owner_path) {
        if (!*owner_path || strlen(owner_path) >= PATH_MAX - 40) {
            fprintf(stderr, "[enroll] --owner-file のパスが不正です\n");
            return 2;
        }
        if (owner_uid_arg) {
            if (parse_uid_strict(owner_uid_arg, &owner_uid) != 0) {
                fprintf(stderr, "[enroll] --owner-uid が不正です: %s\n", owner_uid_arg);
                return 2;
            }
        } else if (geteuid() == 0) {
            /* root 実行: sudo が設定した SUDO_UID を登録者とする。無い・不正・0 の時は
             * 誰の指紋か判断できないので推測しない。 */
            if (parse_uid_strict(getenv("SUDO_UID"), &owner_uid) != 0 || owner_uid == 0) {
                fprintf(stderr, "[enroll] 登録オーナーの uid を特定できません (SUDO_UID が"
                                "無い・不正・0)。--owner-uid N で指紋の持ち主の uid を"
                                "指定してください\n");
                return 2;
            }
        } else {
            owner_uid = getuid();
        }
    } else if (owner_uid_arg) {
        fprintf(stderr, "[enroll] --owner-uid は --owner-file を指定した時 (または既定の出力先の時) "
                        "のみ有効です\n");
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    log_step("=== 指紋登録 ===");
    log_step("出力: %s / エンジン: %s", out_path, engine_path);
    if (owner_path) log_step("登録オーナー: uid %u → %s", (unsigned)owner_uid, owner_path);
    else log_step("登録オーナー: 記録しません (出力先が既定以外のため。必要なら --owner-file)");

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

    size_t img_bytes = (size_t)ft.width * ft.height;
    uint8_t *img = img_bytes ? malloc(img_bytes) : NULL;
    if (!img) {
        fprintf(stderr, "[enroll] 画像バッファを確保できません (%ux%u)\n", ft.width, ft.height);
        ft_usb_close(&u);
        return 1;
    }
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
            char dirbuf[PATH_MAX];
            snprintf(dirbuf, sizeof dirbuf, "%s", out_path);
            char *slash = strrchr(dirbuf, '/');
            if (slash && slash != dirbuf) {
                *slash = 0;
                mkdir(dirbuf, 0755); /* 既存なら失敗するが無害 */
            }
            if (save_enrollment(out_path, tmpl, tmpllen, owner_path, owner_uid) == 0) {
                log_step("テンプレート保存: %s (%zu bytes) ✅ (受理 %d / 却下 %d)",
                         out_path, tmpllen, accepted, rejected);
                if (owner_path)
                    log_step("登録オーナー保存: %s (uid %u) ✅", owner_path, (unsigned)owner_uid);
                rc = 0;
                log_step("デーモンへの反映: sudo make restart-daemon");
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
