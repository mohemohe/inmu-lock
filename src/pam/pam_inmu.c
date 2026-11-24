/*
 * pam_inmu.c — inmu-lock: sudo 認証用 PAM モジュール (arm64 + x86_64)。
 *
 * 挙動 (pam_sm_authenticate):
 *   0. 認証対象ユーザー (PAM_USER) の uid を求める。PAM_RHOST が設定されている
 *      (リモートからの認証) 場合は指紋を使わない。
 *   1. /var/run/inmu/pam.sock (root 専用) へ接続し、peer (デーモン) の
 *      euid == 0 を検証する。
 *      → 接続できない = デーモン不在 → 即 PAM_IGNORE (sudo の挙動は無変更)。
 *   2. "WAIT <timeout_ms> <uid>" を送信し、conversation で
 *      「指紋センサーに触れてください」メッセージを表示。
 *   3. timeout 内にデーモンから "OK\n" (= 指紋 MATCH) → PAM_SUCCESS。
 *      それ以外 ("NG" / "ERR" / 期限切れ / 切断 / 不正な応答) → PAM_IGNORE →
 *      次のモジュール (pam_opendirectory) へフォールバック = パスワード入力。
 *
 * 方針: fail-closed。少しでも想定外のことがあれば指紋を使わず PAM_IGNORE を
 * 返す (PAM_AUTH_ERR は返さない = sudo 自体は壊さない)。
 * 利用者・セッション・画面ロック等の承認判断は core (inmu-cored) が行う。
 * このモジュールの検査は多層防御である。
 *
 * sudo_local の設定 (scripts/install.sh が自動設定):
 *   auth       sufficient     /usr/local/lib/pam/pam_inmu.so timeout_ms=5000
 *
 * ビルド: arm64 + x86_64 (fat) — sudo はネイティブで動くため PAM モジュールは
 * ホスト arch でロードされる必要がある (x86_64 DLL は使わない。socket 通信のみ)。
 */

#include "ipc.h"

#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef PAM_EXTERN
#define PAM_EXTERN
#endif

/* デーモン (peer) とソケットファイル所有者に要求する uid。常に root (0)。
 * 下のマクロは scratchpad のテストハーネス専用で、Makefile のビルドでは
 * 決して定義しない (定義するとビルド時に警告が出る)。 */
#if defined(INMU_PAM_TEST_PEER_UID) || defined(INMU_PAM_TEST_SOCK_OWNER_UID)
#warning "pam_inmu: test build (peer verification relaxed). DO NOT INSTALL"
#endif
#ifndef INMU_PAM_TEST_PEER_UID
#define INMU_PAM_TEST_PEER_UID 0
#endif
#ifndef INMU_PAM_TEST_SOCK_OWNER_UID
#define INMU_PAM_TEST_SOCK_OWNER_UID 0
#endif
#define TRUSTED_PEER_UID ((uid_t)(INMU_PAM_TEST_PEER_UID))
#define TRUSTED_SOCK_OWNER_UID ((uid_t)(INMU_PAM_TEST_SOCK_OWNER_UID))

/* デーモン側の期限に対する読み取り期限の余裕 (ms) */
#define REPLY_SLACK_MS 500

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
            /* 10 進数のみ・1..INMU_PAM_MAX_TIMEOUT_MS。不正値はデフォルトのまま */
            const char *s = argv[i] + 11;
            char *end = NULL;
            errno = 0;
            long v = strtol(s, &end, 10);
            if (errno == 0 && s[0] >= '0' && s[0] <= '9' && *end == '\0' &&
                v >= 1 && v <= INMU_PAM_MAX_TIMEOUT_MS)
                o->timeout_ms = v;
        } else if (strncmp(argv[i], "sock=", 5) == 0) {
            o->sock = argv[i] + 5;
        } else if (strcmp(argv[i], "debug") == 0) {
            o->debug = 1;
        }
    }
}

/* debug 出力。デーモンからの受信内容やユーザー情報は出さない (固定文言のみ)。 */
static void dbg(const struct opts *o, const char *fmt, ...) {
    if (!o->debug) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* 単調時計 (ms)。失敗時 -1 */
static int64_t mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* fd が events 可能になるのを deadline (単調時計 ms) まで待つ。
 * poll を使う (select + FD_SET は fd >= FD_SETSIZE でスタックを壊すため)。
 * 戻り: 1=可能, 0=期限切れ, -1=エラー */
static int wait_fd(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t now = mono_ms();
        if (now < 0) return -1;
        int64_t left = deadline - now;
        if (left <= 0) return 0;
        if (left > INT_MAX) left = INT_MAX;
        struct pollfd p = {.fd = fd, .events = events, .revents = 0};
        int r = poll(&p, 1, (int)left);
        if (r > 0) return (p.revents & POLLNVAL) ? -1 : 1;
        if (r < 0 && errno != EINTR) return -1;
        /* r == 0 または EINTR: 残り時間を再計算して続行 */
    }
}

/* len バイトを書き切る (部分書き込み・EINTR 対応)。
 * fd は O_NONBLOCK、SIGPIPE は SO_NOSIGPIPE で抑止済みであること
 * (connect_daemon が設定する。macOS の Unix ソケットでは send の
 * MSG_DONTWAIT が効かずブロックするため O_NONBLOCK を使う)。
 * 戻り: 0=成功, -1=失敗/期限切れ */
static int send_all(int fd, const char *buf, size_t len, int64_t deadline) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, POLLOUT, deadline) != 1) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* 応答 1 行を '\n' まで読み切る (部分読み・EINTR 対応、全体期限は deadline)。
 * fd は O_NONBLOCK であること。
 * line には改行を除いた内容を入れ、*len に長さを返す (NUL 終端もする)。
 * 改行より後ろに余分なデータがある・行が linesz-1 バイトを超える・改行前に
 * 切断された場合はプロトコル違反としてエラーにする。
 * 戻り: 1=行あり, 0=期限切れ, -1=エラー/切断/不正な応答 */
static int read_line_deadline(int fd, char *line, size_t linesz, size_t *len,
                              int64_t deadline) {
    size_t have = 0;
    for (;;) {
        int w = wait_fd(fd, POLLIN, deadline);
        if (w <= 0) return w;
        ssize_t n = recv(fd, line + have, linesz - 1 - have, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        if (n == 0) return -1; /* 改行前に切断 */
        char *nl = memchr(line + have, '\n', (size_t)n);
        have += (size_t)n;
        if (nl) {
            if (nl != line + have - 1) return -1; /* 改行の後ろに余分なデータ */
            *nl = '\0';
            *len = have - 1;
            return 1;
        }
        if (have >= linesz - 1) return -1; /* 長すぎる行 */
    }
}

/* 認証対象ユーザー (PAM_USER) の uid を得る。プロンプトは出さない。
 * 戻り: 0=成功, -1=取得不可 */
static int target_uid(pam_handle_t *pamh, uid_t *uid) {
    const void *item = NULL;
    if (pam_get_item(pamh, PAM_USER, &item) != PAM_SUCCESS || item == NULL)
        return -1;
    const char *user = item;
    if (user[0] == '\0') return -1;
    struct passwd pw, *res = NULL;
    char buf[4096];
    if (getpwnam_r(user, &pw, buf, sizeof buf, &res) != 0 || res == NULL)
        return -1;
    *uid = res->pw_uid;
    return 0;
}

/* デーモンのソケットへ接続し、相手が本物 (root) であることを確認する。
 * 戻り: 接続済み fd, 失敗時 -1 */
static int connect_daemon(const struct opts *o) {
    struct sockaddr_un addr;
    size_t plen = strlen(o->sock);
    if (plen == 0 || plen >= sizeof addr.sun_path) return -1;

    /* 多層防御: ソケットファイルが root 所有のソケットであること
     * (最終的な判定は接続後の getpeereid で行う)。 */
    struct stat st;
    if (lstat(o->sock, &st) != 0) {
        dbg(o, "pam_inmu: daemon 未起動 → PAM_IGNORE");
        return -1;
    }
    if (!S_ISSOCK(st.st_mode) || st.st_uid != TRUSTED_SOCK_OWNER_UID) {
        dbg(o, "pam_inmu: ソケットの所有者/種別が不正 → PAM_IGNORE");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    /* 子プロセスへ fd を漏らさない。相手が切断しても SIGPIPE を発生させない
     * (sudo プロセスのシグナル設定には触れない)。 */
    int one = 1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, o->sock, plen);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        dbg(o, "pam_inmu: daemon 未起動 → PAM_IGNORE");
        close(fd);
        return -1;
    }

    /* デーモンの真正性: peer の euid が root であること */
    uid_t peer_uid = (uid_t)-1;
    gid_t peer_gid = (gid_t)-1;
    if (getpeereid(fd, &peer_uid, &peer_gid) != 0 ||
        peer_uid != TRUSTED_PEER_UID) {
        dbg(o, "pam_inmu: 接続先が root のデーモンではない → PAM_IGNORE");
        close(fd);
        return -1;
    }

    /* 以降の送受信は poll + 非ブロッキング I/O で期限管理する */
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 「指紋センサーに触れてください」を conversation で表示する。
 * 戻り: 0=成功, -1=conversation 不可/失敗 */
static int show_prompt(pam_handle_t *pamh, const struct opts *o) {
    const void *item = NULL;
    if (pam_get_item(pamh, PAM_CONV, &item) != PAM_SUCCESS || item == NULL)
        return -1;
    const struct pam_conv *conv = item;
    if (conv->conv == NULL) return -1;

    char msg[256];
    snprintf(msg, sizeof msg,
             "[inmu] 指紋センサーに触れてください "
             "(触れない場合は約%ld秒後にパスワード入力になります)",
             (o->timeout_ms + 999) / 1000);
    struct pam_message m = {.msg_style = PAM_TEXT_INFO, .msg = msg};
    const struct pam_message *mp = &m;
    struct pam_response *resp = NULL;
    int rc = conv->conv(1, &mp, &resp, conv->appdata_ptr);
    /* 失敗時は conversation 側が解放済み (resp には触れない)。
     * 成功時は配列と各応答文字列の解放が呼び出し側の責任。 */
    if (rc != PAM_SUCCESS) return -1;
    if (resp) {
        free(resp[0].resp);
        free(resp);
    }
    return 0;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                                   const char *argv[]) {
    (void)flags;
    struct opts o;
    parse_opts(argc, argv, &o);

    /* 0) リモートからの認証 (PAM_RHOST あり) では指紋を使わない。
     *    (SSH 経由の sudo では PAM_RHOST が付かないことが多い。その遮断は
     *     core が要求元の audit session で行う。ここは多層防御。) */
    {
        const void *rhost = NULL;
        if (pam_get_item(pamh, PAM_RHOST, &rhost) != PAM_SUCCESS) {
            dbg(&o, "pam_inmu: PAM_RHOST を取得できない → PAM_IGNORE");
            return PAM_IGNORE;
        }
        if (rhost != NULL && ((const char *)rhost)[0] != '\0') {
            dbg(&o, "pam_inmu: リモートからの認証 → PAM_IGNORE");
            return PAM_IGNORE;
        }
    }

    /* 認証対象ユーザーの uid (core が登録オーナー・コンソールユーザーと照合) */
    uid_t uid;
    if (target_uid(pamh, &uid) != 0) {
        dbg(&o, "pam_inmu: 認証対象ユーザーを特定できない → PAM_IGNORE");
        return PAM_IGNORE;
    }

    /* 1) デーモンへ接続 (できない = デーモン不在 → 即フォールバック) */
    int fd = connect_daemon(&o);
    if (fd < 0) return PAM_IGNORE;

    /* 全体の期限 (単調時計)。デーモン側の期限 + 余裕 */
    int64_t now = mono_ms();
    if (now < 0) {
        close(fd);
        return PAM_IGNORE;
    }
    int64_t deadline = now + o.timeout_ms + REPLY_SLACK_MS;

    /* 2) WAIT 送信 + ユーザーへのメッセージ表示 */
    {
        char req[INMU_IPC_LINE_MAX];
        int n = snprintf(req, sizeof req, "WAIT %ld %lu\n", o.timeout_ms,
                         (unsigned long)uid);
        if (n <= 0 || (size_t)n >= sizeof req ||
            send_all(fd, req, (size_t)n, deadline) != 0) {
            dbg(&o, "pam_inmu: 要求を送信できない → PAM_IGNORE");
            close(fd);
            return PAM_IGNORE;
        }
    }
    if (show_prompt(pamh, &o) != 0) {
        /* 利用者に案内できないまま指紋で承認しない */
        dbg(&o, "pam_inmu: conversation 失敗 → PAM_IGNORE");
        close(fd);
        return PAM_IGNORE;
    }

    /* 3) 結果待ち: "OK" と完全一致 → PAM_SUCCESS / それ以外 → PAM_IGNORE */
    char line[INMU_IPC_LINE_MAX];
    size_t len = 0;
    int r = read_line_deadline(fd, line, sizeof line, &len, deadline);
    close(fd);
    if (r == 1 && len == 2 && memcmp(line, "OK", 2) == 0) {
        dbg(&o, "pam_inmu: MATCH → PAM_SUCCESS");
        return PAM_SUCCESS;
    }
    dbg(&o, "pam_inmu: 不一致/timeout/エラー (r=%d) → PAM_IGNORE", r);
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
