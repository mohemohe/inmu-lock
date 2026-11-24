/*
 * inmu-cored.c — inmu-lock MVP (T4): Core Service デーモン (root, x86_64/Rosetta)。
 *
 * 責務:
 *   1. FT9201 bring-up (fw転送) と 96×96 画像取得          (検証D の実績コード)
 *   2. vendor matcher DLL の in-process 実行 (Rosetta 2)   (検証C の実績コード)
 *   3. 指検知 → 照合 → MATCH 通知:
 *      - /var/run/inmu/pam.sock   (root専用): PAM の WAIT 要求に OK/NG で応答
 *      - /var/run/inmu/agent.sock (購読自由): MATCH を push
 *
 * 使い方:
 *   sudo ./inmu-cored --template <tmpl.bin> --engine <ftWbioEngineAdapter.dll>
 *
 * 動作: 指が置かれたら画像を撮り、登録済みテンプレートと照合する。
 *   MATCH → agent購読者に "MATCH"、待機中のPAM要求に "OK"
 *   不一致 → ログのみ (PAM はタイムアウトで NG → パスワードフォールバック)
 * 1つの「指→MATCH」イベントが sudo 認証と画面ロック解除の両方に効く。
 */

#include "ipc.h"

#include "ft_engine.h"
#include "ft9201.h"
#include "usb_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_PAM_CLIENTS 16
#define MAX_AGENT_CLIENTS 8
#define LINEBUF 128

typedef struct {
    int fd;
    long long deadline_ms; /* モノトニック時刻 (ms)。0 = 未WAIT */
} PamClient;

typedef struct {
    int fd;
} AgentClient;

static PamClient pam_clients[MAX_PAM_CLIENTS];
static int n_pam_clients = 0;
static AgentClient agent_clients[MAX_AGENT_CLIENTS];
static int n_agent_clients = 0;

static int g_pam_listen = -1, g_agent_listen = -1;
static volatile sig_atomic_t g_stop = 0;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void log_ts(const char *fmt, ...) {
    char timebuf[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(timebuf, sizeof timebuf, "%H:%M:%S", &tmv);
    fprintf(stderr, "[inmu-cored %s] ", timebuf);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ================================================================ */
/* ソケット                                                       */
/* ================================================================ */

static int make_listener(const char *path, mode_t mode) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    chmod(path, mode);
    if (listen(fd, 8) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ソケットから1行読む (非blocking fd向け、行末'\n'で1行)。
 * 戻り: 1=行あり, 0=まだ, -1=切断/エラー。line は NUL 終端。 */
static int read_line(int fd, char *line, size_t linesz) {
    (void)linesz; /* 内部バッファサイズで運ぶ */
    static char buf[LINEBUF];
    static int len[FD_SETSIZE];
    static int filled[FD_SETSIZE];
    if (!filled[fd]) {
        filled[fd] = 1;
        len[fd] = 0;
    }
    while (len[fd] < LINEBUF - 1) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n == 1) {
            if (ch == '\n') {
                memcpy(line, buf, (size_t)len[fd]);
                line[len[fd]] = 0;
                len[fd] = 0;
                return 1;
            }
            buf[len[fd]++] = ch;
        } else if (n == 0) {
            return -1; /* 切断 */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
    }
    return -1; /* 行が長すぎる */
}

/* ================================================================ */
/* PAM / AGENT クライアント管理                                      */
/* ================================================================ */

static void remove_fd(int fd) {
    close(fd);
}

static void pam_reply(PamClient *c, const char *msg) {
    if (c->fd >= 0) {
        write(c->fd, msg, strlen(msg));
        remove_fd(c->fd);
        c->fd = -1;
    }
}

static void broadcast_match(void) {
    for (int i = 0; i < n_pam_clients; i++) {
        if (pam_clients[i].fd >= 0 && pam_clients[i].deadline_ms > 0) {
            pam_reply(&pam_clients[i], "OK\n");
        }
    }
    for (int i = 0; i < n_agent_clients; i++) {
        if (agent_clients[i].fd >= 0) {
            if (write(agent_clients[i].fd, "MATCH\n", 6) < 0) {
                /* 切断された購読者は除去 */
                remove_fd(agent_clients[i].fd);
                agent_clients[i].fd = -1;
            }
        }
    }
    /* compact */
    int w = 0;
    for (int i = 0; i < n_pam_clients; i++)
        if (pam_clients[i].fd >= 0) pam_clients[w++] = pam_clients[i];
    n_pam_clients = w;
    w = 0;
    for (int i = 0; i < n_agent_clients; i++)
        if (agent_clients[i].fd >= 0) agent_clients[w++] = agent_clients[i];
    n_agent_clients = w;
}

static void service_sockets(void) {
    char line[LINEBUF];

    /* 新規接続の受付 */
    for (int fd = accept(g_pam_listen, NULL, NULL); fd >= 0;
         fd = accept(g_pam_listen, NULL, NULL)) {
        if (n_pam_clients < MAX_PAM_CLIENTS) {
            set_nonblock(fd);
            pam_clients[n_pam_clients].fd = fd;
            pam_clients[n_pam_clients].deadline_ms = 0;
            n_pam_clients++;
            log_ts("PAM クライアント接続 (fd=%d)", fd);
        } else {
            write(fd, "ERR\n", 4);
            close(fd);
        }
    }
    for (int fd = accept(g_agent_listen, NULL, NULL); fd >= 0;
         fd = accept(g_agent_listen, NULL, NULL)) {
        if (n_agent_clients < MAX_AGENT_CLIENTS) {
            set_nonblock(fd);
            agent_clients[n_agent_clients].fd = fd;
            n_agent_clients++;
            log_ts("agent 購読開始 (fd=%d)", fd);
        } else {
            close(fd);
        }
    }

    /* PAM 要求の読み取り */
    for (int i = 0; i < n_pam_clients; i++) {
        int fd = pam_clients[i].fd;
        if (fd < 0) continue;
        int r = read_line(fd, line, sizeof line);
        if (r == 1 && strncmp(line, "WAIT", 4) == 0) {
            long timeout_ms = atol(line + 4);
            if (timeout_ms <= 0) timeout_ms = INMU_PAM_DEFAULT_TIMEOUT_MS;
            pam_clients[i].deadline_ms = (long long)(now_ms() + timeout_ms);
            log_ts("PAM WAIT %ldms 受付 (fd=%d)", timeout_ms, fd);
        } else if (r < 0) {
            remove_fd(fd);
            pam_clients[i].fd = -1;
        }
    }

    /* AGENT SUB の読み取り */
    for (int i = 0; i < n_agent_clients; i++) {
        int fd = agent_clients[i].fd;
        if (fd < 0) continue;
        int r = read_line(fd, line, sizeof line);
        if (r == 1 && strncmp(line, "SUB", 3) == 0) {
            log_ts("agent SUB 確認 (fd=%d)", fd);
        } else if (r < 0) {
            remove_fd(fd);
            agent_clients[i].fd = -1;
        }
    }

    /* PAM 期限切れ → NG (パスワードフォールバック) */
    for (int i = 0; i < n_pam_clients; i++) {
        if (pam_clients[i].fd >= 0 && pam_clients[i].deadline_ms > 0 &&
            now_ms() > (double)pam_clients[i].deadline_ms) {
            log_ts("PAM WAIT 期限切れ → NG (fd=%d)", pam_clients[i].fd);
            pam_reply(&pam_clients[i], "NG\n");
        }
    }

    /* compact */
    int w = 0;
    for (int i = 0; i < n_pam_clients; i++)
        if (pam_clients[i].fd >= 0) pam_clients[w++] = pam_clients[i];
    n_pam_clients = w;
    w = 0;
    for (int i = 0; i < n_agent_clients; i++)
        if (agent_clients[i].fd >= 0) agent_clients[w++] = agent_clients[i];
    n_agent_clients = w;
}

/* ================================================================ */
/* センサー照合ループ                                               */
/* ================================================================ */

static int finger_loop(Ft9201 *ft, const uint8_t *tmpl, size_t tmpllen) {
    uint8_t reg[4];
    uint8_t *img = malloc((size_t)ft->width * ft->height);
    if (!img) return -1;
    uint8_t quality[2] = {0, 0};

    log_ts("照合待機開始 (指を検知したら撮影→照合します)");

    while (!g_stop) {
        service_sockets();

        if (ft9201_reg_read(ft, REG_FINGER, reg, 4) != 0) {
            log_ts("!! 指状態読み取り失敗 — USB エラー。終了します");
            free(img);
            return -1;
        }
        if (reg[0] != 0xa0) {
            usleep(100 * 1000);
            continue;
        }

        /* 指を検知: セットリング → 撮影 → 照合 */
        usleep(50 * 1000);
        if (ft9201_capture_image(ft, img, quality) != 0) {
            log_ts("!! 画像取得失敗");
            ft9201_wait_lift(ft, 3000);
            continue;
        }
        log_ts("画像取得 (quality=%02x %02x)", quality[0], quality[1]);

        uint32_t hr = ft_engine_accept(img, ft->width, ft->height, 1 /* verify */);
        if (hr != 0) {
            log_ts("accept reject (hr=%#x) — 品質不足。指を置き直してください", hr);
            ft9201_wait_lift(ft, 3000);
            continue;
        }

        int m = ft_engine_verify(tmpl, tmpllen);
        if (m) {
            log_ts("★ 指紋 MATCH — PAM/agent へ通知");
            broadcast_match();
        } else {
            log_ts("指紋 不一致 (登録されていない指)");
        }
        ft9201_wait_lift(ft, 5000);
    }

    free(img);
    return 0;
}

/* ================================================================ */
/* main                                                             */
/* ================================================================ */

static uint8_t *load_file(const char *path, size_t *outlen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)len);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    *outlen = (size_t)len;
    return p;
}

int main(int argc, char **argv) {
    const char *template_path = NULL;
    const char *engine_path = NULL;
    const char *sockdir = INMU_SOCKET_DIR;
    char pam_sock[256], agent_sock[256];

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--template") && i + 1 < argc) template_path = argv[++i];
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine_path = argv[++i];
        else if (!strcmp(argv[i], "--sockdir") && i + 1 < argc) sockdir = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr,
                    "usage: sudo %s --template tmpl.bin --engine DLL "
                    "[--sockdir /var/run/inmu]\n"
                    "  (--sockdir はテスト用: 非 root でユーザー領域にソケットを置ける)\n",
                    argv[0]);
            return 0;
        }
    }
    if (!template_path || !engine_path) {
        fprintf(stderr, "--template と --engine が必要です (-h 参照)\n");
        return 2;
    }
    snprintf(pam_sock, sizeof pam_sock, "%s/pam.sock", sockdir);
    snprintf(agent_sock, sizeof agent_sock, "%s/agent.sock", sockdir);
    /* root 必須は /var/run 既定の場合のみ (--sockdir 指定時はユーザー実行可) */
    if (geteuid() != 0 && !strcmp(sockdir, INMU_SOCKET_DIR)) {
        fprintf(stderr, "/var/run 既定のソケットには root が必要です (sudo で実行、"
                        "または --sockdir でユーザー領域を指定)\n");
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    log_ts("inmu-cored 起動 (template=%s, engine=%s)", template_path, engine_path);

    /* テンプレート読み込み */
    size_t tmpllen = 0;
    uint8_t *tmpl = load_file(template_path, &tmpllen);
    if (!tmpl) {
        fprintf(stderr, "テンプレートが読めません: %s\n", template_path);
        return 1;
    }
    log_ts("テンプレート読み込み: %zu bytes", tmpllen);

    /* センサー bring-up */
    Ft9201Usb u;
    if (ft_usb_find(&u) != 0) {
        fprintf(stderr, "FT9201 が見つかりません (接続を確認)\n");
        return 1;
    }
    if (ft_usb_open(&u) != 0) {
        fprintf(stderr, "USB オープン失敗\n");
        return 1;
    }
    Ft9201 ft;
    if (ft9201_init(&ft, &u) != 0) {
        fprintf(stderr, "bring-up 失敗 — センサーを抜き差しして再試行\n");
        ft_usb_close(&u);
        return 1;
    }
    if (ft9201_capture_background(&ft) != 0) {
        fprintf(stderr, "背景 (FPN) 取得失敗\n");
        ft_usb_close(&u);
        return 1;
    }

    /* matcher (vendor DLL, Rosetta 2) */
    if (ft_engine_open(engine_path) != 0) {
        fprintf(stderr, "matcher DLL ロード失敗: %s\n", engine_path);
        ft_usb_close(&u);
        return 1;
    }
    /* テンプレートを engine のストレージに読み込ませる (verify 用) */
    (void)ft_engine_verify(tmpl, tmpllen);
    log_ts("matcher 準備完了");

    /* ソケット */
    mkdir(sockdir, 0755);
    g_pam_listen = make_listener(pam_sock, 0600);
    g_agent_listen = make_listener(agent_sock, 0666);
    if (g_pam_listen < 0 || g_agent_listen < 0) {
        fprintf(stderr, "ソケット作成失敗 (%s)\n", sockdir);
        return 1;
    }
    log_ts("IPC 準備完了: %s (0600, rootのみ) / %s (0666, 購読自由)", pam_sock,
           agent_sock);

    int r = finger_loop(&ft, tmpl, tmpllen);

    log_ts("終了中...");
    unlink(pam_sock);
    unlink(agent_sock);
    ft_usb_close(&u);
    free(tmpl);
    return r == 0 ? 0 : 1;
}
