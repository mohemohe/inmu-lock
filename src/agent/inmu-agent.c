/*
 * inmu-agent.c — inmu-lock MVP (T6): 画面ロック解除エージェント (arm64 ネイティブ)。
 *
 * 責務 (docs/01 §4.3 の設計どおり):
 *   1. /var/run/inmu/agent.sock を購読し、デーモンから "MATCH" を受ける
 *   2. MATCH 受信時、CGSessionCopyCurrentDictionary の
 *      CGSSessionScreenIsLocked で画面ロック中であることを確認
 *      (ロック中でなければ何もしない = 誤爆防止 — 検証Bで実証済みの原則)
 *   3. ロック中のみ Keychain からログインパスワードを取得し、
 *      CGEvent でパスワード + Return を注入してロック解除
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

#include "ipc.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <Security/Security.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/termios.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#define SERVICE_NAME "com.mohemohe.inmu-lock"
#define ACCOUNT_NAME "login-password"

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

/* ================================================================ */
/* Keychain                                                         */
/* ================================================================ */

static OSStatus keychain_put(const char *secret) {
    CFStringRef service = CFSTR(SERVICE_NAME);
    CFStringRef account = CFSTR(ACCOUNT_NAME);
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)secret, (CFIndex)strlen(secret));

    /* 冪等にするため先に削除 */
    const void *delKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void *delVals[] = {kSecClassGenericPassword, service, account};
    CFDictionaryRef del = CFDictionaryCreate(NULL, delKeys, delVals, 3,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    SecItemDelete(del);
    CFRelease(del);

    const void *addKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData,
                             kSecAttrAccessible};
    const void *addVals[] = {kSecClassGenericPassword, service, account, data,
                             kSecAttrAccessibleAfterFirstUnlock};
    CFDictionaryRef add = CFDictionaryCreate(NULL, addKeys, addVals, 5,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    OSStatus st = SecItemAdd(add, NULL);
    CFRelease(add);
    CFRelease(data);
    return st;
}

static char *keychain_get(void) {
    const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData,
                          kSecMatchLimit};
    const void *vals[] = {kSecClassGenericPassword, CFSTR(SERVICE_NAME), CFSTR(ACCOUNT_NAME),
                          kCFBooleanTrue, kSecMatchLimitOne};
    CFDictionaryRef query =
        CFDictionaryCreate(NULL, keys, vals, 5, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CFTypeRef result = NULL;
    OSStatus st = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (st != errSecSuccess || !result) return NULL;
    CFDataRef d = (CFDataRef)result;
    CFIndex len = CFDataGetLength(d);
    char *s = malloc((size_t)len + 1);
    memcpy(s, CFDataGetBytePtr(d), (size_t)len);
    s[len] = 0;
    CFRelease(d);
    return s;
}

/* tty からエコーなしで1行読む (パスワード入力用) */
static char *read_password_tty(const char *prompt) {
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    struct termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);
    struct termios newt = oldt;
    newt.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    char buf[1024];
    if (!fgets(buf, sizeof buf, stdin)) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        return NULL;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fprintf(stderr, "\n");
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    char *p = strdup(buf);
    memset(buf, 0, sizeof buf);
    return p;
}

/* ================================================================ */
/* 画面ロック判定 (検証B と同一の方法)                              */
/* ================================================================ */

static bool is_screen_locked(void) {
    CFDictionaryRef dict = CGSessionCopyCurrentDictionary();
    if (!dict) return false; /* GUIセッション外 (SSH等) はロック中ではない扱い */
    CFBooleanRef locked = CFDictionaryGetValue(dict, CFSTR("CGSSessionScreenIsLocked"));
    bool v = (locked != NULL && CFBooleanGetValue(locked));
    CFRelease(dict);
    return v;
}

/* ================================================================ */
/* キー注入 (検証B の Keyboard.swift を C に移植)                   */
/* ================================================================ */

#define kVK_Return 0x24

static void post_unicode_char(UniChar ch) {
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, 0, true);
    if (down) {
        CGEventKeyboardSetUnicodeString(down, 1, &ch);
        CGEventPost(kCGHIDEventTap, down);
        CFRelease(down);
    }
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, 0, false);
    if (up) {
        CGEventKeyboardSetUnicodeString(up, 1, &ch);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(up);
    }
}

static void post_key(UInt32 keycode) {
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, keycode, true);
    if (down) {
        CGEventPost(kCGHIDEventTap, down);
        CFRelease(down);
    }
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, keycode, false);
    if (up) {
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(up);
    }
}

static void type_string(const char *s) {
    for (const char *p = s; *p; p++) {
        post_unicode_char((UniChar)(unsigned char)*p);
        usleep(20000);
    }
    usleep(20000);
    post_key(kVK_Return);
}

/* ================================================================ */
/* main                                                             */
/* ================================================================ */

/* デーモンへ購読接続 (起動順序に依存しないよう再試行ループ付き) */
static int connect_subscribe(void) {
    for (;;) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            return -1;
        }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof addr.sun_path, "%s", INMU_AGENT_SOCK);
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0 &&
            write(fd, "SUB\n", 4) == 4) {
            log_ts("デーモンに接続、MATCHの購読を開始しました");
            return fd;
        }
        close(fd);
        log_ts("デーモンに接続できません — 2秒後に再試行します (起動順序は自由)");
        sleep(2);
        if (g_stop) return -1;
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--set-password") == 0) {
        char *pw = read_password_tty(
            "⚠️  ログインパスワードを macOS Keychain に保存します\n"
            "   (保存は画面ロック解除機能にのみ使われます。docs/01 §4.4 参照)\n"
            "パスワード: ");
        if (!pw || !*pw) {
            fprintf(stderr, "空のパスワードは保存できません\n");
            return 1;
        }
        OSStatus st = keychain_put(pw);
        free(pw);
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

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

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
        char *pw = keychain_get();
        if (!pw) {
            fprintf(stderr,
                    "⚠️  パスワードが未保存です。先に:\n"
                    "  %s --set-password\n"
                    "  を実行してください\n",
                    argv[0]);
            return 1;
        }
        free(pw);
    }

    /* デーモンへ購読接続 (再試行付き) */
    int fd = connect_subscribe();
    if (fd < 0) return 1;
    log_ts("現在のロック状態: %s", is_screen_locked() ? "LOCKED" : "unlocked");

    while (!g_stop) {
        char buf[64];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            log_ts("デーモンとの接続が切れました — 再接続します");
            close(fd);
            fd = connect_subscribe();
            if (fd < 0) break;
            continue;
        }
        buf[n] = 0;
        if (strncmp(buf, "MATCH", 5) != 0) continue;

        log_ts("MATCH 受信");
        if (!is_screen_locked()) {
            log_ts("画面はロックされていないため何もしません (誤爆防止)");
            continue;
        }
        log_ts("画面ロック中 → Keychain からパスワードを取得して注入します");
        char *pw = keychain_get();
        if (!pw) {
            log_ts("!! パスワード取得失敗 (Keychain)");
            continue;
        }
        type_string(pw);
        memset(pw, 0, strlen(pw));
        free(pw);
        log_ts("注入完了");
        /* 結果はユーザーが目視 (ロック解除されるはず) */
    }

    close(fd);
    log_ts("終了");
    return 0;
}
