/*
 * inmu-agent.c — inmu-lock MVP (T6): 画面ロック解除エージェント (arm64 ネイティブ)。
 *
 * 責務 (docs/01 §4.3 の設計どおり):
 *   1. /var/run/inmu/agent.sock を購読し、デーモンから "MATCH" を受ける
 *   2. MATCH 受信時、CGSessionCopyCurrentDictionary で「自セッションが
 *      コンソール上にあり、かつ CGSSessionScreenIsLocked が true」であることを確認
 *      (満たさなければ何もしない = 誤爆防止 — 検証Bで実証済みの原則)
 *   3. ロック中のみ Keychain からログインパスワードを取得し、
 *      CGEvent でパスワード + Return を注入してロック解除。
 *      注入中も 1 文字ごと・Return の直前にロック状態を再確認し、
 *      ロックが外れていたら即中止する (Return は送らない)
 *
 * 接続の検証: agent.sock の peer (inmu-cored) の euid == 0 を確認する (ipc.h 参照)。
 *
 * 使い方:
 *   ./inmu-agent --set-password     # ログインパスワードをKeychainに保存 (初回1回)
 *   ./inmu-agent                     # 購読モードで起動 (デーモン起動後に)
 *
 * 権限: キー注入にはアクセシビリティ権限 (TCC) が必要。
 *   ターミナルから実行する場合: システム設定 → プライバシーとセキュリティ →
 *   アクセシビリティ に「ターミナル」を追加。
 *
 * ⚠️ パスワードは macOS Keychain に保存されます (docs/01 §4.4 のトレードオフ)。
 */

/* memset_s (最適化で消されないメモリ消去) を使うため、最初の include より前に定義する */
#define __STDC_WANT_LIB_EXT1__ 1

#include "ipc.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <Security/Security.h>

#include <errno.h>
#include <libproc.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/termios.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SERVICE_NAME "dev.mohemohe.inmu-lock"
#define ACCOUNT_NAME "login-password"

/* パスワード 1 行の最大長 (終端 NUL を含むバッファサイズ) */
#define PASSWORD_BUF_MAX 1024
/* 注入後、この秒数の間は次の MATCH を無視する (連続 MATCH による二重注入の防止) */
#define INJECT_COOLDOWN_SEC 5
/* 再接続の待ち時間 (秒): 失敗が続くと BASE から MAX まで伸ばす */
#define RECONNECT_BASE_SEC 2
#define RECONNECT_MAX_SEC 10
/* 接続がこの秒数以上続いたら「安定した接続」とみなし連続失敗数をリセットする */
#define CONNECT_STABLE_SEC 10

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void log_ts(const char *fmt, ...) {
    char timebuf[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(timebuf, sizeof timebuf, "%H:%M:%S", &tmv);
    fprintf(stderr, "[inmu-agent %s] ", timebuf);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* 単調増加時計 (秒)。クールダウン・接続継続時間の計測用 */
static time_t mono_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ts.tv_sec;
}

/* ================================================================ */
/* 秘密情報のメモリ消去                                             */
/* ================================================================ */

/* 最適化で消されない消去 (memset_s) */
static void wipe(void *p, size_t n) {
    if (p && n) memset_s(p, n, 0, n);
}

/* len バイト (+ 終端 NUL) の秘密文字列を消去してから解放する */
static void secret_free(char *s, size_t len) {
    if (!s) return;
    wipe(s, len + 1);
    free(s);
}

/* ================================================================ */
/* UTF-8 の検証・デコード                                           */
/* ================================================================ */

/* s[0..n) の先頭 1 文字を厳密にデコードする。消費バイト数 (1..4) を返す。
 * 不正な UTF-8 (不正な先頭/継続バイト・冗長表現・サロゲート・U+10FFFF 超・
 * 途中で切れている) は 0 を返す。 */
static size_t utf8_decode(const unsigned char *s, size_t n, uint32_t *cp) {
    if (n == 0) return 0;
    unsigned char b0 = s[0];
    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    }
    size_t need;
    uint32_t v;
    uint32_t min;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 2;
        v = b0 & 0x1Fu;
        min = 0x80;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 3;
        v = b0 & 0x0Fu;
        min = 0x800;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 4;
        v = b0 & 0x07u;
        min = 0x10000;
    } else {
        return 0; /* 継続バイト単独 / 0xC0,0xC1 / 0xF5 以上 */
    }
    if (n < need) return 0;
    for (size_t i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if (v < min) return 0;                    /* 冗長表現 */
    if (v >= 0xD800 && v <= 0xDFFF) return 0; /* サロゲート */
    if (v > 0x10FFFF) return 0;
    *cp = v;
    return need;
}

/* 注入できるパスワードか: 空でなく、正しい UTF-8 で、制御文字
 * (C0 / DEL / C1 — 改行やタブを含む) を含まないこと。 */
static bool password_is_valid(const char *s, size_t len) {
    if (!s || len == 0) return false;
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        size_t k = utf8_decode(p + i, len - i, &cp);
        if (k == 0) return false;
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return false;
        i += k;
    }
    return true;
}

/* コードポイントを UTF-16 に変換する。書いた UniChar 数 (1 or 2) を返す */
static UniCharCount utf16_encode(uint32_t cp, UniChar out[2]) {
    if (cp < 0x10000) {
        out[0] = (UniChar)cp;
        return 1;
    }
    cp -= 0x10000;
    out[0] = (UniChar)(0xD800 + (cp >> 10));
    out[1] = (UniChar)(0xDC00 + (cp & 0x3FF));
    return 2;
}

/* ================================================================ */
/* Keychain                                                         */
/* ================================================================ */

static OSStatus keychain_put(const char *secret, size_t len) {
    CFStringRef service = CFSTR(SERVICE_NAME);
    CFStringRef account = CFSTR(ACCOUNT_NAME);
    /* 複製を作らない (kCFAllocatorNull): CFData は呼び出し側のバッファを指すだけで、
     * 消去は呼び出し側が行う */
    CFDataRef data =
        CFDataCreateWithBytesNoCopy(NULL, (const UInt8 *)secret, (CFIndex)len, kCFAllocatorNull);
    if (!data) return errSecAllocate;

    /* 冪等にするため先に削除 */
    const void *delKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void *delVals[] = {kSecClassGenericPassword, service, account};
    CFDictionaryRef del = CFDictionaryCreate(NULL, delKeys, delVals, 3,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    if (!del) {
        CFRelease(data);
        return errSecAllocate;
    }
    SecItemDelete(del);
    CFRelease(del);

    /* 設計書 (docs/01 §4.4) どおり AfterFirstUnlockThisDeviceOnly を指定する。
     * ただしファイルベースの login keychain ではこの属性は効かない (SecItem.h:
     * macOS では kSecUseDataProtectionKeychain / kSecAttrSynchronizable 指定時のみ有効)。
     * data protection keychain はプロビジョニングプロファイルで認可された entitlement が
     * 必要で、ad-hoc 署名のコマンドラインツールからは使えないため使用しない。 */
    const void *addKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData,
                             kSecAttrAccessible};
    const void *addVals[] = {kSecClassGenericPassword, service, account, data,
                             kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    CFDictionaryRef add = CFDictionaryCreate(NULL, addKeys, addVals, 5,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    if (!add) {
        CFRelease(data);
        return errSecAllocate;
    }
    OSStatus st = SecItemAdd(add, NULL);
    CFRelease(add);
    CFRelease(data);
    return st;
}

/* Keychain からパスワードを取得する。成功時は malloc した NUL 終端文字列を返し、
 * *len_out に長さを入れる。呼び出し側は必ず secret_free() で消去・解放すること。 */
static char *keychain_get(size_t *len_out) {
    const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData,
                          kSecMatchLimit};
    const void *vals[] = {kSecClassGenericPassword, CFSTR(SERVICE_NAME), CFSTR(ACCOUNT_NAME),
                          kCFBooleanTrue, kSecMatchLimitOne};
    CFDictionaryRef query =
        CFDictionaryCreate(NULL, keys, vals, 5, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    if (!query) return NULL;
    CFTypeRef result = NULL;
    OSStatus st = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (st != errSecSuccess || !result) return NULL;
    if (CFGetTypeID(result) != CFDataGetTypeID()) {
        CFRelease(result);
        return NULL;
    }
    CFDataRef d = (CFDataRef)result;
    CFIndex len = CFDataGetLength(d);
    char *s = NULL;
    if (len > 0 && len < PASSWORD_BUF_MAX) s = malloc((size_t)len + 1);
    if (s) {
        memcpy(s, CFDataGetBytePtr(d), (size_t)len);
        s[len] = 0;
        *len_out = (size_t)len;
    }
    CFRelease(d);
    return s;
}

/* ================================================================ */
/* tty からのパスワード入力                                         */
/* ================================================================ */

static volatile sig_atomic_t g_tty_sig = 0;
static void on_tty_signal(int sig) { g_tty_sig = sig; }

/* stdin (tty) からエコーなしで 1 行読む (パスワード入力用)。
 * stdin が tty でない・エコーを切れない場合は読まずに NULL を返す。
 * 入力中にシグナルを受けた場合は端末設定を戻してから、そのシグナルを
 * 元の動作で送り直す (既定なら終了する)。
 * 成功時は malloc した NUL 終端文字列を返し、*len_out に長さを入れる。
 * 呼び出し側は必ず secret_free() で消去・解放すること。 */
static char *read_password_tty(const char *prompt, size_t *len_out) {
    static const int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP};
    enum { NSIGS = sizeof sigs / sizeof sigs[0] };
    const int fd = STDIN_FILENO;

    struct termios oldt;
    if (!isatty(fd) || tcgetattr(fd, &oldt) != 0) {
        fprintf(stderr, "標準入力が端末ではないため、パスワードを読み取りません\n");
        return NULL;
    }

    /* 端末設定を変える前にハンドラを入れる (SA_RESTART なし: read を EINTR で戻す) */
    struct sigaction sa, oldsa[NSIGS];
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_tty_signal;
    sigemptyset(&sa.sa_mask);
    g_tty_sig = 0;
    for (int i = 0; i < NSIGS; i++) sigaction(sigs[i], &sa, &oldsa[i]);

    char buf[PASSWORD_BUF_MAX];
    size_t len = 0;
    bool ok = false;

    struct termios newt = oldt;
    newt.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
    struct termios chk;
    /* tcsetattr は一部しか適用できなくても成功を返すので、読み直して確認する */
    if (tcsetattr(fd, TCSAFLUSH, &newt) != 0 || tcgetattr(fd, &chk) != 0 ||
        (chk.c_lflag & (ECHO | ECHONL)) != 0) {
        fprintf(stderr, "端末のエコーを無効にできないため、パスワードを読み取りません\n");
    } else {
        fprintf(stderr, "%s", prompt);
        fflush(stderr);
        /* stdio を通さず 1 バイトずつ読む (stdio の内部バッファに複製を残さない) */
        bool too_long = false;
        for (;;) {
            char c;
            ssize_t n = read(fd, &c, 1);
            if (n < 0) {
                if (errno == EINTR && !g_tty_sig) continue;
                break;
            }
            if (n == 0) break; /* EOF */
            if (c == '\n' || c == '\r') {
                ok = !too_long;
                break;
            }
            if (len < sizeof buf - 1)
                buf[len++] = c;
            else
                too_long = true;
        }
        fprintf(stderr, "\n");
        if (too_long) fprintf(stderr, "パスワードが長すぎます\n");
    }

    /* 端末設定を必ず戻す (途中まで適用された可能性があるので失敗時も戻す) */
    tcsetattr(fd, TCSAFLUSH, &oldt);
    for (int i = 0; i < NSIGS; i++) sigaction(sigs[i], &oldsa[i], NULL);

    char *p = NULL;
    int sig = g_tty_sig;
    if (ok && !sig) {
        p = malloc(len + 1);
        if (p) {
            memcpy(p, buf, len);
            p[len] = 0;
            *len_out = len;
        } else {
            fprintf(stderr, "メモリを確保できません\n");
        }
    }
    wipe(buf, sizeof buf);
    if (sig) {
        /* 端末を戻し終えたので、元のハンドラ (既定なら終了/停止) に処理させる */
        kill(getpid(), sig);
    }
    return p;
}

/* ================================================================ */
/* 画面ロック判定 (検証B と同一の方法)                              */
/* ================================================================ */

/* 辞書の key が CFBoolean の true のときだけ true (キー無し・型違いは false) */
static bool dict_bool_is_true(CFDictionaryRef dict, CFStringRef key) {
    CFTypeRef v = CFDictionaryGetValue(dict, key);
    return v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID() &&
           CFBooleanGetValue((CFBooleanRef)v);
}

/* セッション辞書が「コンソール上にあり、かつ画面ロック中」を示すか。
 * コンソール上にないセッション (ファストユーザスイッチで裏に回った等) では、
 * 合成キーが別ユーザーの画面に届きうるので注入しない。 */
static bool session_dict_is_locked_on_console(CFDictionaryRef dict) {
    if (!dict || CFGetTypeID(dict) != CFDictionaryGetTypeID()) return false;
    return dict_bool_is_true(dict, kCGSessionOnConsoleKey) &&
           dict_bool_is_true(dict, CFSTR("CGSSessionScreenIsLocked"));
}

static bool is_screen_locked(void) {
    CFDictionaryRef dict = CGSessionCopyCurrentDictionary();
    if (!dict) return false; /* GUIセッション外 (SSH等) はロック中ではない扱い */
    bool v = session_dict_is_locked_on_console(dict);
    CFRelease(dict);
    return v;
}

/* ================================================================ */
/* キー注入 (検証B の Keyboard.swift を C に移植)                   */
/* ================================================================ */

#define kVK_Return 0x24

/* 1 文字分 (UTF-16 で 1 or 2 単位) の key down / up を送る。
 * イベントを作れなかった場合は何も送らず false を返す。 */
static bool post_unicode_char(const UniChar *chars, UniCharCount count) {
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, 0, true);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, 0, false);
    bool ok = (down != NULL && up != NULL);
    if (ok) {
        CGEventKeyboardSetUnicodeString(down, count, chars);
        CGEventKeyboardSetUnicodeString(up, count, chars);
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
    }
    if (down) CFRelease(down);
    if (up) CFRelease(up);
    return ok;
}

static bool post_key(UInt32 keycode) {
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)keycode, true);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)keycode, false);
    bool ok = (down != NULL && up != NULL);
    if (ok) {
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
    }
    if (down) CFRelease(down);
    if (up) CFRelease(up);
    return ok;
}

typedef enum {
    TYPE_DONE = 0,     /* パスワード + Return を送った */
    TYPE_INVALID,      /* 不正な UTF-8 / 制御文字 — 1 文字も送っていない */
    TYPE_NOT_LOCKED,   /* 途中でロックが外れた — 中止、Return は送っていない */
    TYPE_STOPPED,      /* 終了シグナル — 中止、Return は送っていない */
    TYPE_EVENT_FAILED, /* イベントを作れなかった — 中止、Return は送っていない */
} type_result_t;

/* パスワードを打って Return を送る。
 * 毎文字の前と Return の前にロック状態を再確認し、ロックが外れていたら即中止する。 */
static type_result_t type_password(const char *s, size_t len) {
    /* 途中まで打ってから不正に気付くことがないよう、先に全体を検証する */
    if (!password_is_valid(s, len)) return TYPE_INVALID;

    const unsigned char *p = (const unsigned char *)s;
    type_result_t res = TYPE_DONE;
    UniChar u[2] = {0, 0};
    uint32_t cp = 0;
    size_t i = 0;
    while (i < len) {
        size_t k = utf8_decode(p + i, len - i, &cp);
        if (k == 0) { /* 検証済みなので起きないはずだが、念のため中止 */
            res = TYPE_INVALID;
            break;
        }
        if (g_stop) {
            res = TYPE_STOPPED;
            break;
        }
        if (!is_screen_locked()) {
            res = TYPE_NOT_LOCKED;
            break;
        }
        UniCharCount n = utf16_encode(cp, u);
        if (!post_unicode_char(u, n)) {
            res = TYPE_EVENT_FAILED;
            break;
        }
        i += k;
        usleep(20000);
    }
    wipe(u, sizeof u);
    wipe(&cp, sizeof cp);
    if (res != TYPE_DONE) return res;

    usleep(20000);
    if (g_stop) return TYPE_STOPPED;
    if (!is_screen_locked()) return TYPE_NOT_LOCKED;
    if (!post_key(kVK_Return)) return TYPE_EVENT_FAILED;
    return TYPE_DONE;
}

/* ================================================================ */
/* 受信行パーサ                                                     */
/* ================================================================ */

/* '\n' 終端の行を組み立てる。read の境界をまたぐ部分行を保持する。 */
struct line_reader {
    char buf[INMU_IPC_LINE_MAX];
    size_t len;
    bool overflow; /* 行が長すぎる — 次の '\n' まで読み捨て中 */
};

/* 受信データを与え、完成した行のうち "MATCH" に完全一致したものの数を返す。
 * 長すぎる行 (INMU_IPC_LINE_MAX 以上) は丸ごと捨てる。 */
static unsigned line_reader_feed(struct line_reader *lr, const char *data, size_t n) {
    unsigned matches = 0;
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            if (!lr->overflow && lr->len == 5 && memcmp(lr->buf, "MATCH", 5) == 0) matches++;
            lr->len = 0;
            lr->overflow = false;
        } else if (lr->overflow) {
            /* 読み捨て */
        } else if (lr->len >= sizeof lr->buf - 1) {
            lr->overflow = true;
        } else {
            lr->buf[lr->len++] = c;
        }
    }
    return matches;
}

/* ================================================================ */
/* デーモンへの接続                                                 */
/* ================================================================ */

typedef enum {
    CONN_OK = 0,
    CONN_ERR_SOCKET,  /* socket() 失敗 */
    CONN_ERR_CONNECT, /* connect() 失敗 (デーモン未起動 / 権限なし) */
    CONN_ERR_PEER,    /* peer が root ではない */
    CONN_ERR_WRITE,   /* SUB を送れない */
} conn_result_t;

/* デーモンへ 1 回だけ購読接続を試みる。成功時は fd、失敗時は -1 を返し、
 * *why に理由、*err に errno を入れる。 */
static int connect_once(conn_result_t *why, int *err) {
    *err = 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        *err = errno;
        *why = CONN_ERR_SOCKET;
        return -1;
    }
    /* 切断済みソケットへの書き込みで SIGPIPE を受けないようにする */
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", INMU_AGENT_SOCK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        *err = errno;
        *why = CONN_ERR_CONNECT;
        close(fd);
        return -1;
    }
    /* peer (core) が root であることを検証する (ipc.h)。
     * root 以外が同じパスで待ち受けていても MATCH を信用しない。 */
    uid_t euid = (uid_t)-1;
    gid_t egid = (gid_t)-1;
    if (getpeereid(fd, &euid, &egid) != 0 || euid != 0) {
        *err = errno;
        *why = CONN_ERR_PEER;
        close(fd);
        return -1;
    }
    if (write(fd, "SUB\n", 4) != 4) {
        *err = errno;
        *why = CONN_ERR_WRITE;
        close(fd);
        return -1;
    }
    *why = CONN_OK;
    return fd;
}

/* 連続失敗 n 回目 (1 始まり) をログに出すか: 最初の 3 回と、以後 30 回ごと */
static bool should_log_failure(unsigned n) { return n <= 3 || n % 30 == 0; }

/* 連続失敗 n 回 (1 以上) の後、次の接続までに待つ秒数 */
static unsigned reconnect_delay_sec(unsigned n) {
    unsigned d = RECONNECT_BASE_SEC;
    for (unsigned i = 3; i < n && d < RECONNECT_MAX_SEC; i++) d *= 2;
    return d < RECONNECT_MAX_SEC ? d : RECONNECT_MAX_SEC;
}

/* 終了シグナルで中断できる待ち */
static void wait_sec(unsigned sec) {
    for (unsigned i = 0; i < sec && !g_stop; i++) sleep(1);
}

/* 自分が INMU_AGENT_PATH 以外から起動されている場合、core に接続を拒否される
 * (ipc.h)。原因が分かるようログに出す。 */
static void warn_if_unexpected_path(void) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    int n = proc_pidpath(getpid(), path, sizeof path);
    if (n <= 0) {
        log_ts("!! 自分の実行ファイルパスを取得できません (errno %d)", errno);
        return;
    }
    if (strcmp(path, INMU_AGENT_PATH) != 0)
        log_ts("!! 実行ファイルが %s ではありません (実際: %s) — "
               "デーモンは接続を拒否するため、指紋によるロック解除は動作しません",
               INMU_AGENT_PATH, path);
}

/* ================================================================ */
/* main                                                             */
/* ================================================================ */

/* MATCH 1 件を処理する: ロック中ならパスワードを注入する。
 * 注入を試みた (キーを 1 つでも送った可能性がある) 場合に true を返す。 */
static bool handle_match(void) {
    if (!is_screen_locked()) {
        log_ts("画面はロックされていないため何もしません (誤爆防止)");
        return false;
    }
    log_ts("画面ロック中 → Keychain からパスワードを取得して注入します");
    size_t len = 0;
    char *pw = keychain_get(&len);
    if (!pw) {
        log_ts("!! パスワード取得失敗 (Keychain)");
        return false;
    }
    type_result_t r = type_password(pw, len);
    secret_free(pw, len);
    switch (r) {
    case TYPE_DONE:
        log_ts("注入完了");
        /* 結果はユーザーが目視 (ロック解除されるはず) */
        break;
    case TYPE_INVALID:
        log_ts("!! 保存済みパスワードが不正 (UTF-8 でない/制御文字を含む) なため注入しません — "
               "--set-password で保存し直してください");
        return false;
    case TYPE_NOT_LOCKED:
        log_ts("注入中に画面ロックが外れたため中止しました (Return は送っていません)");
        break;
    case TYPE_STOPPED:
        log_ts("終了要求を受けたため注入を中止しました");
        break;
    case TYPE_EVENT_FAILED:
        log_ts("!! キーイベントを作成できないため注入を中止しました");
        break;
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--set-password") == 0) {
        fprintf(stderr, "⚠️  ログインパスワードを macOS Keychain に保存します\n"
                        "   (保存は画面ロック解除機能にのみ使われます。docs/01 §4.4 参照)\n");
        size_t len = 0;
        char *pw = read_password_tty("パスワード: ", &len);
        if (!pw) return 1;
        if (len == 0) {
            secret_free(pw, len);
            fprintf(stderr, "空のパスワードは保存できません\n");
            return 1;
        }
        if (!password_is_valid(pw, len)) {
            secret_free(pw, len);
            fprintf(stderr, "パスワードが正しい UTF-8 でないか、制御文字を含むため保存できません\n");
            return 1;
        }
        OSStatus st = keychain_put(pw, len);
        secret_free(pw, len);
        if (st != errSecSuccess) {
            fprintf(stderr, "Keychain 保存に失敗: OSStatus %d\n", (int)st);
            return 1;
        }
        fprintf(stderr, "保存しました (service=%s account=%s) ✅\n", SERVICE_NAME,
                ACCOUNT_NAME);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--unset-password") == 0) {
        CFStringRef service = CFSTR(SERVICE_NAME);
        CFStringRef account = CFSTR(ACCOUNT_NAME);
        const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
        const void *vals[] = {kSecClassGenericPassword, service, account};
        CFDictionaryRef del = CFDictionaryCreate(NULL, keys, vals, 3,
                                                &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
        OSStatus st = SecItemDelete(del);
        CFRelease(del);
        if (st == errSecSuccess)
            fprintf(stderr, "Keychain から削除しました ✅\n");
        else
            fprintf(stderr, "削除対象が見つからないか失敗しました (OSStatus %d)\n", (int)st);
        return st == errSecSuccess ? 0 : 1;
    }

    /* SA_RESTART なし: ブロック中の read を EINTR で戻し、g_stop を見て終了できるようにする */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }
    /* デーモンが切断した直後の書き込みで落ちないようにする */
    signal(SIGPIPE, SIG_IGN);

    /* アクセシビリティ権限の確認 (なければプロンプト表示して終了) */
    {
        CFStringRef key = kAXTrustedCheckOptionPrompt;
        CFBooleanRef val = kCFBooleanTrue;
        CFDictionaryRef opts = CFDictionaryCreate(NULL, (const void **)&key,
                                                  (const void **)&val, 1, NULL, NULL);
        bool trusted = AXIsProcessTrustedWithOptions(opts);
        CFRelease(opts);
        if (!trusted) {
            fprintf(stderr,
                    "⚠️  アクセシビリティ権限がありません。\n"
                    "  システム設定 → プライバシーとセキュリティ → アクセシビリティ\n"
                    "  にこのプロセス (実行元ターミナル) を追加してから再実行してください\n");
            return 1;
        }
    }
    /* パスワード保存の確認 */
    {
        size_t len = 0;
        char *pw = keychain_get(&len);
        if (!pw) {
            fprintf(stderr,
                    "⚠️  パスワードが未保存です。先に:\n"
                    "  %s --set-password\n"
                    "  を実行してください\n",
                    argv[0]);
            return 1;
        }
        secret_free(pw, len);
    }

    warn_if_unexpected_path();
    log_ts("現在のロック状態: %s", is_screen_locked() ? "LOCKED" : "unlocked");

    int fd = -1;
    unsigned failures = 0;     /* 連続失敗数 (接続失敗 + すぐ切れた接続) */
    time_t connected_at = 0;   /* 現在の接続を張った時刻 */
    time_t cooldown_until = 0; /* この時刻までは MATCH を無視する */
    bool injected_once = false;
    struct line_reader lr;
    memset(&lr, 0, sizeof lr);

    while (!g_stop) {
        if (fd < 0) {
            /* デーモンへ購読接続 (起動順序に依存しないよう再試行する)。
             * 失敗・切断の後は必ず待ってから繋ぎ直す (待ちなしの再接続ループ防止)。 */
            if (failures > 0) {
                wait_sec(reconnect_delay_sec(failures));
                if (g_stop) break;
            }
            conn_result_t why;
            int err;
            fd = connect_once(&why, &err);
            if (fd < 0) {
                failures++;
                if (should_log_failure(failures)) {
                    unsigned next = reconnect_delay_sec(failures);
                    switch (why) {
                    case CONN_ERR_PEER:
                        log_ts("!! %s の接続先が root ではないため切断しました — "
                               "%u秒後に再試行します (連続 %u 回目)",
                               INMU_AGENT_SOCK, next, failures);
                        break;
                    case CONN_ERR_SOCKET:
                    case CONN_ERR_CONNECT:
                    case CONN_ERR_WRITE:
                    default:
                        log_ts("デーモンに接続できません (%s) — %u秒後に再試行します "
                               "(連続 %u 回目、起動順序は自由)",
                               strerror(err), next, failures);
                        break;
                    }
                }
                continue;
            }
            /* すぐ切られる接続が続いている間は、接続成功のログも間引く */
            if (should_log_failure(failures + 1))
                log_ts("デーモンに接続、MATCHの購読を開始しました");
            connected_at = mono_now();
            memset(&lr, 0, sizeof lr);
        }

        char buf[INMU_IPC_LINE_MAX];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            fd = -1;
            if (mono_now() - connected_at >= CONNECT_STABLE_SEC) failures = 0;
            failures++;
            if (should_log_failure(failures))
                log_ts("デーモンとの接続が切れました — %u秒後に再接続します (連続 %u 回目)",
                       reconnect_delay_sec(failures), failures);
            continue;
        }

        /* 1 回の read に複数の MATCH 行が入っていても 1 回だけ処理する */
        if (line_reader_feed(&lr, buf, (size_t)n) == 0) continue;

        log_ts("MATCH 受信");
        if (injected_once && mono_now() < cooldown_until) {
            log_ts("直前の注入から %d 秒以内のため無視します (二重注入防止)",
                   INJECT_COOLDOWN_SEC);
            continue;
        }
        if (handle_match()) {
            injected_once = true;
            cooldown_until = mono_now() + INJECT_COOLDOWN_SEC;
        }
    }

    if (fd >= 0) close(fd);
    log_ts("終了");
    return 0;
}
