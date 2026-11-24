/*
 * pam_inmu.c — inmu-lock MVP (T5): sudo 認証用 PAM モジュール (arm64 ネイティブ)。
 *
 * 挙動 (pam_sm_authenticate):
 *   1. /var/run/inmu/pam.sock (root 専用) へ接続。
 *      → 接続できない = デーモン不在 → 即 PAM_IGNORE (sudo の挙動は無変更)。
 *   2. "WAIT <timeout_ms>" を送信し、conversation で
 *      「指紋センサーに触れてください」メッセージを表示。
 *   3. timeout 内にデーモンから "OK" (= 指紋 MATCH) → PAM_SUCCESS。
 *      "NG" / 期限切れ / 不一致 → PAM_IGNORE → 次のモジュール
 *      (pam_opendirectory) へフォールバック = パスワード入力。
 *
 * sudo_local の設定 (scripts/install-sudo.sh が自動設定):
 *   auth       sufficient     /usr/local/lib/pam/pam_inmu.so timeout_ms=3000
 *
 * ビルド: arm64 + x86_64 (fat) — sudo はネイティブで動くため PAM モジュールは
 * ホスト arch でロードされる必要がある (x86_64 DLL は使わない。socket 通信のみ)。
 */

#include "ipc.h"

#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef PAM_EXTERN
#define PAM_EXTERN
#endif

#define LINEBUF 128

/* モジュール引数: timeout_ms=N, sock=/path, debug */
struct opts {
    long timeout_ms;
    const char *sock;
    int debug;
};

static void parse_opts(int argc, const char **argv, struct opts *o) {
    o->timeout_ms = INMU_PAM_DEFAULT_TIMEOUT_MS;
    o->sock = INMU_PAM_SOCK;
    o->debug = 0;
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "timeout_ms=", 11) == 0) {
            long v = atol(argv[i] + 11);
            if (v > 0 && v < 60000) o->timeout_ms = v;
        } else if (strncmp(argv[i], "sock=", 5) == 0) {
            o->sock = argv[i] + 5;
        } else if (strcmp(argv[i], "debug") == 0) {
            o->debug = 1;
        }
    }
}

static void dbg(const struct opts *o, const char *fmt, ...) {
    if (!o->debug) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* 1行読み (blocking, select で timeout 制御)。
 * 戻り: 1=行あり, 0=timeout, -1=エラー/切断 */
static int read_line_to(int fd, char *line, size_t linesz, long timeout_ms) {
    struct timeval tv = {.tv_sec = timeout_ms / 1000,
                         .tv_usec = (timeout_ms % 1000) * 1000};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    int r = select(fd + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) return 0;
    ssize_t n = read(fd, line, linesz - 1);
    if (n <= 0) return -1;
    line[n] = 0;
    /* 末尾改行を除去 */
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    return 1;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                                   const char *argv[]) {
    (void)flags;
    struct opts o;
    parse_opts(argc, argv, &o);

    /* 1) デーモンへ接続 (できない = デーモン不在 → 即フォールバック) */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return PAM_IGNORE;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", o.sock);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        dbg(&o, "pam_inmu: daemon 未起動 (%s) → PAM_IGNORE", o.sock);
        close(fd);
        return PAM_IGNORE;
    }

    /* 2) WAIT 送信 + ユーザーへのメッセージ表示 */
    {
        char req[LINEBUF];
        snprintf(req, sizeof req, "WAIT %ld\n", o.timeout_ms);
        if (write(fd, req, strlen(req)) < 0) {
            close(fd);
            return PAM_IGNORE;
        }
    }
    {
        struct pam_conv *conv = NULL;
        if (pam_get_item(pamh, PAM_CONV, (const void **)&conv) == PAM_SUCCESS &&
            conv && conv->conv) {
            char msg[256];
            snprintf(msg, sizeof msg, "[inmu] 指紋センサーに触れてください "
                                     "(触れない場合は約%ld秒後にパスワード入力になります)",
                     o.timeout_ms / 1000);
            struct pam_message m = {.msg_style = PAM_TEXT_INFO, .msg = msg};
            const struct pam_message *mp = &m;
            struct pam_response *resp = NULL;
            conv->conv(1, &mp, &resp, conv->appdata_ptr);
            if (resp) free(resp);
        }
    }

    /* 3) 結果待ち: OK → PAM_SUCCESS / NG・timeout → PAM_IGNORE */
    char line[LINEBUF];
    int r = read_line_to(fd, line, sizeof line, o.timeout_ms + 500 /*余裕*/);
    close(fd);
    if (r == 1 && strncmp(line, "OK", 2) == 0) {
        dbg(&o, "pam_inmu: MATCH → PAM_SUCCESS");
        return PAM_SUCCESS;
    }
    dbg(&o, "pam_inmu: 不一致/timeout (r=%d line=%s) → PAM_IGNORE", r, r == 1 ? line : "-");
    return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc,
                              const char *argv[]) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}
