/*
 * pam_inmu_dummy — 検証A: inmu-lock 用ダミーPAMモジュール
 *
 * 目的:
 *   - /etc/pam.d/sudo_local に組み込んで sudo から PAM モジュールが
 *     呼び出されること・フォールバック挙動を安全に確認する。
 *   - 実際の Core Service (Unix domain socket) との通信は Phase 1 で
 *     pam_inmu.so として実装する。ここでは「呼ばれること」と
 *     「返り値と sudo の挙動の対応」の検証のみを行う。
 *
 * 挙動 (pam_sm_authenticate):
 *   1. 呼び出しを /tmp/inmu_pam_dummy.log と syslog に記録する。
 *   2. オプション wait=N があれば N 秒スリープする (タイムアウトUXの確認用)。
 *   3. オプション fail があれば PAM_AUTH_ERR を返す (明示的な失敗の確認用)。
 *   4. オプション demo_success があり、かつ /tmp/inmu_pam_demo_success が
 *      root 所有で存在すれば PAM_SUCCESS を返す (パスワード無しで sudo が
 *      通ることの確認用。一般ユーザーが勝手に arm できないよう root 所有を要求)。
 *   5. それ以外は PAM_IGNORE を返す (次のモジュール = パスワード認証へ
 *      フォールバック)。デフォルト挙動なので、sudo の動作は無変更。
 *
 * ビルド:
 *   cc -O2 -Wall -Wextra -fPIC -dynamiclib -o pam_inmu_dummy.so src/pam_inmu_dummy.c
 */

#define PAM_SM_AUTH

#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef PAM_EXTERN
#define PAM_EXTERN
#endif

#define LOG_PATH "/tmp/inmu_pam_dummy.log"
#define DEMO_SUCCESS_PATH "/tmp/inmu_pam_demo_success"

struct module_opts {
    int debug;
    int demo_success;
    int fail;
    double wait_seconds;
};

static void parse_opts(int argc, const char **argv, struct module_opts *o) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "debug") == 0) {
            o->debug = 1;
        } else if (strcmp(argv[i], "demo_success") == 0) {
            o->demo_success = 1;
        } else if (strcmp(argv[i], "fail") == 0) {
            o->fail = 1;
        } else if (strncmp(argv[i], "wait=", 5) == 0) {
            o->wait_seconds = atof(argv[i] + 5);
            if (o->wait_seconds < 0) o->wait_seconds = 0;
            if (o->wait_seconds > 60) o->wait_seconds = 60;
        }
    }
}

static void nap(double seconds) {
    if (seconds <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

/* syslog と簡易確認用ログファイルの両方に出す */
static void log_msg(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    openlog("pam_inmu_dummy", LOG_PID, LOG_AUTH);
    syslog(LOG_NOTICE, "%s", msg);
    closelog();

    char line[640];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    size_t n = (size_t)strftime(line, sizeof(line), "%Y-%m-%d %H:%M:%S", &tmv);
    if (n > 0) {
        snprintf(line + n, sizeof(line) - n, " [pid %d] %s", (int)getpid(), msg);
    }
    int fd = open(LOG_PATH, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0644);
    if (fd >= 0) {
        (void)write(fd, line, strlen(line));
        (void)write(fd, "\n", 1);
        close(fd);
    }
}

/* セキュリティのため「root 所有のファイル」のみを成功トリガーとして認める */
static int file_exists_and_owned_by_root(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISLNK(st.st_mode)) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    return st.st_uid == 0;
}

static const char *pam_item_str(pam_handle_t *pamh, int item_type) {
    const void *v = NULL;
    if (pam_get_item(pamh, item_type, &v) != PAM_SUCCESS || v == NULL) return "-";
    return (const char *)v;
}

static const char *pam_user_str(pam_handle_t *pamh) {
    const char *user = NULL;
    if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || user == NULL) return "-";
    return user;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                                   const char *argv[]) {
    struct module_opts o = {0, 0, 0, 0.0};
    parse_opts(argc, argv, &o);

    log_msg("authenticate: user=%s service=%s tty=%s flags=0x%x argc=%d%s",
            pam_user_str(pamh), pam_item_str(pamh, PAM_SERVICE),
            pam_item_str(pamh, PAM_TTY), flags, argc,
            o.debug ? " (debug)" : "");

    if (o.wait_seconds > 0) {
        log_msg("simulating daemon round-trip delay: %.1fs", o.wait_seconds);
        nap(o.wait_seconds);
    }

    if (o.fail) {
        log_msg("option 'fail' set -> PAM_AUTH_ERR (次のモジュールへフォールバック)");
        return PAM_AUTH_ERR;
    }

    if (o.demo_success && file_exists_and_owned_by_root(DEMO_SUCCESS_PATH)) {
        log_msg("DEMO: %s exists and is root-owned -> PAM_SUCCESS "
                "(パスワード入力をスキップします)", DEMO_SUCCESS_PATH);
        return PAM_SUCCESS;
    }

    if (o.demo_success) {
        log_msg("demo_success requested but %s is missing or not root-owned "
                "-> PAM_IGNORE", DEMO_SUCCESS_PATH);
    }

    /* デフォルト: 何もせず次へ (パスワード認証へフォールバック) */
    log_msg("no positive result -> PAM_IGNORE (パスワード認証へフォールバック)");
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
