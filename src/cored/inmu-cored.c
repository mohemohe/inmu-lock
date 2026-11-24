/*
 * inmu-cored.c — inmu-lock: Core Service デーモン (root, x86_64/Rosetta)。
 *
 * 責務:
 *   1. FT9201 bring-up (fw転送) と 96×96 画像取得          (検証D の実績コード)
 *   2. vendor matcher DLL の in-process 実行 (Rosetta 2)   (検証C の実績コード)
 *   3. 指検知 → 照合 → MATCH の割り当て (プロトコルは ipc.h):
 *      - pam.sock   (0600 root):  peer euid==0 のみ。WAIT 要求に OK/NG/ERR で応答
 *      - agent.sock (0600 owner): peer euid==登録オーナー かつ 実行ファイルが
 *                                 inmu-agent のもののみ。MATCH を push
 *
 * 承認ポリシー (fail-closed。判断できないときは承認しない):
 *   - MATCH 1 回で承認する PAM 要求は「その撮影の開始より前に WAIT 済み」で
 *     期限内のもののうち最も古い 1 件だけ。
 *   - 承認条件: WAIT の uid == 登録オーナー uid == コンソールユーザー uid、
 *     画面がロックされていない、要求元の audit session == コンソール GUI
 *     セッション (SSH・別セッションの sudo は不可)。判定は MATCH の時点で行う。
 *     別セッションと分かっている要求は WAIT の時点で NG を返す。
 *   - 画面ロック中の MATCH は agent への通知 (ロック解除) にだけ使う。agent への
 *     通知は「登録オーナーのコンソールセッションが画面ロック中」の時だけ行う。
 *   - 使い道の無い接触 (待機中の PAM 要求が無く、agent へ通知できる状態でもない)
 *     は撮影も照合もしない。
 *   - 1 回撮影したら指が離れるまで次の撮影をしない (置きっぱなしで再承認しない)。
 *   - 連続不一致が INMU_LOCKOUT_THRESHOLD に達したらロックアウト。状態は
 *     INMU_LOCKOUT_FILE に永続化し、再起動 (USB 抜き差し) でも消えない。
 *     試行は照合の前に保存する (保存できなければ照合しない)。
 *
 * 使い方:
 *   sudo ./inmu-cored --template <tmpl.bin> --engine <ftWbioEngineAdapter.dll>
 */

#include "ipc.h"

#include "ft_engine.h"
#include "ft9201.h"
#include "usb_transport.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <bsm/libbsm.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_PAM_CLIENTS 16
#define MAX_AGENT_CLIENTS 8
/* 1 回の service で 1 つの listen ソケットから受け付ける接続数の上限 */
#define ACCEPT_BURST 16
/* 接続してから WAIT / SUB を送るまでの猶予 (ms)。過ぎたら切断 */
#define HELLO_TIMEOUT_MS 5000
/* 「指が離れた」と見なすのに必要な連続非検知回数 (チャタリング対策) */
#define LIFT_DEBOUNCE 3
/* 連続失敗カウンタの上限 (ロックアウト時間は既に上限に達している) */
#define LOCKOUT_FAILS_CAP (INMU_LOCKOUT_THRESHOLD + 16)

/* 接続ごとの状態。行バッファも接続ごとに持ち、閉じたら必ず初期化する。 */
typedef struct {
    int fd;
    char buf[INMU_IPC_LINE_MAX];
    size_t len;
    long long accept_ms; /* 接続時刻 (モノトニック ms) */
    int ready;           /* PAM: WAIT 受付済み / AGENT: SUB 受付済み */
    /* 以下 PAM のみ */
    uid_t uid;                   /* WAIT で指定された認証対象 uid */
    long long deadline_ms;       /* 期限 (モノトニック ms) */
    unsigned long long wait_seq; /* WAIT 受付の通し番号 (古い順の判定に使う) */
} Conn;

static Conn pam_clients[MAX_PAM_CLIENTS];
static int n_pam_clients = 0;
static Conn agent_clients[MAX_AGENT_CLIENTS];
static int n_agent_clients = 0;
static unsigned long long g_wait_seq = 0;

static int g_pam_listen = -1, g_agent_listen = -1;
static volatile sig_atomic_t g_stop = 0;

static uid_t g_euid = 0;         /* デーモン自身の euid */
static uid_t g_pam_peer_uid = 0; /* pam.sock の peer に要求する euid (本番 = 0) */
static int g_owner_known = 0;    /* 登録オーナー uid を読めたか */
static uid_t g_owner_uid = 0;
static const char *g_agent_path = INMU_AGENT_PATH;

/* 連続失敗によるロックアウト状態 */
static struct {
    int dirfd;               /* 状態ディレクトリ */
    const char *name;        /* 状態ファイル名 (ディレクトリ内) */
    int fails;               /* 連続不一致回数 */
    time_t until_wall;       /* ロックアウト解除時刻 (壁時計、保存用) */
    long long until_mono_ms; /* 同 (モノトニック、実行中の判定用)。0 = なし */
    char boot[40];           /* 起動セッション識別子 (kern.bootsessionuuid)。"" = 不明 */
} g_lock = {.dirfd = -1};

static long long mono_ms(void) {
    struct timespec ts;
    /* CLOCK_MONOTONIC はスリープ中も進む (man clock_gettime) */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

/* ファイル/ディレクトリの所有者として信頼できる uid か。
 * root 実行時は root のみ。非 root (テストモード) では自分自身も可。 */
static int trusted_uid(uid_t u) {
    return u == 0 || u == g_euid;
}

/* 厳密な 10 進数パース: [0-9]{1,maxdigits} のみ (符号・空白・接頭辞は不可)。
 * *end に数字列の直後を返す。成功で 0。 */
static int parse_dec(const char *s, int maxdigits, unsigned long long *out,
                     const char **end) {
    unsigned long long v = 0;
    int n = 0;
    while (s[n] >= '0' && s[n] <= '9') {
        if (n >= maxdigits) return -1;
        v = v * 10 + (unsigned long long)(s[n] - '0');
        n++;
    }
    if (n == 0) return -1;
    *out = v;
    *end = s + n;
    return 0;
}

static int write_all(int fd, const char *p, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ================================================================ */
/* 登録オーナー / 検証付きファイルオープン                            */
/* ================================================================ */

/* ディレクトリを symlink を辿らずに開き、所有者とモードを検証する。
 * 条件: ディレクトリ・信頼できる所有者・group/other 書き込み不可。
 * 検証後 mode に揃える (fd 経由なので symlink を辿らない)。成功で fd。 */
static int open_verified_dir(const char *path, mode_t mode, const char *what) {
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        log_ts("!! %s を作成できません: %s (%s)", what, path, strerror(errno));
        return -1;
    }
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        log_ts("!! %s を開けません (symlink は不可): %s (%s)", what, path,
               strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        log_ts("!! %s がディレクトリではありません: %s", what, path);
        close(fd);
        return -1;
    }
    if (!trusted_uid(st.st_uid) || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        log_ts("!! %s の所有者/モードが安全ではありません: %s (uid=%u mode=%o)", what,
               path, (unsigned)st.st_uid, (unsigned)(st.st_mode & 07777));
        close(fd);
        return -1;
    }
    if ((st.st_mode & 07777) != mode && fchmod(fd, mode) != 0) {
        log_ts("!! %s のモードを %o にできません: %s (%s)", what, (unsigned)mode, path,
               strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* 通常ファイルを symlink を辿らずに読み取り用に開き、所有者とモードを検証する。
 * deny_bits: 立っていてはならないモードビット。成功で fd。 */
static int open_verified_file(const char *path, mode_t deny_bits, struct stat *st,
                              const char *what) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        log_ts("!! %s を開けません (symlink は不可): %s (%s)", what, path,
               strerror(errno));
        return -1;
    }
    if (fstat(fd, st) != 0 || !S_ISREG(st->st_mode)) {
        log_ts("!! %s が通常ファイルではありません: %s", what, path);
        close(fd);
        return -1;
    }
    if (!trusted_uid(st->st_uid) || (st->st_mode & deny_bits)) {
        log_ts("!! %s の所有者/モードが安全ではありません: %s (uid=%u mode=%o)", what,
               path, (unsigned)st->st_uid, (unsigned)(st->st_mode & 07777));
        close(fd);
        return -1;
    }
    return fd;
}

/* 登録オーナー uid を読む (10 進テキスト 1 行)。成功で 0。 */
static int load_owner(const char *path, uid_t *out) {
    struct stat st;
    int fd = open_verified_file(path, S_IWGRP | S_IWOTH, &st, "オーナーファイル");
    if (fd < 0) return -1;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) {
        log_ts("!! オーナーファイルが空か読めません: %s", path);
        return -1;
    }
    buf[n] = 0;
    unsigned long long v = 0;
    const char *end = NULL;
    if (strlen(buf) != (size_t)n || parse_dec(buf, 10, &v, &end) != 0 ||
        !(end[0] == 0 || (end[0] == '\n' && end[1] == 0)) ||
        /* root (0) と nobody (-2) / -1 はオーナーとして認めない */
        v == 0 || v >= 0xfffffffeULL) {
        log_ts("!! オーナーファイルの内容が不正です: %s", path);
        return -1;
    }
    *out = (uid_t)v;
    return 0;
}

/* ================================================================ */
/* ロックアウト状態の永続化                                           */
/* ================================================================ */

/* 連続失敗回数に対応するロックアウト秒数 (閾値未満なら 0) */
static long lockout_duration_sec(int fails) {
    if (fails < INMU_LOCKOUT_THRESHOLD) return 0;
    long d = INMU_LOCKOUT_BASE_SEC;
    for (int i = INMU_LOCKOUT_THRESHOLD; i < fails; i++) {
        d *= 2;
        if (d >= INMU_LOCKOUT_MAX_SEC) break;
    }
    return d > INMU_LOCKOUT_MAX_SEC ? INMU_LOCKOUT_MAX_SEC : d;
}

/* 起動セッション識別子に使える文字か (UUID: 16 進数字と '-') */
static int boot_char_ok(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') ||
           c == '-';
}

/* 現在の起動セッション識別子 (OS の起動ごとに変わる UUID) を g_lock.boot に読む。
 * 取れなければ "" のまま (= 同じ起動セッションとは見なさず、壁時計で復元する)。 */
static void lockout_read_boot(void) {
    char buf[64];
    size_t len = sizeof buf - 1;
    g_lock.boot[0] = 0;
    if (sysctlbyname("kern.bootsessionuuid", buf, &len, NULL, 0) != 0 || len == 0) return;
    buf[len] = 0;
    size_t n = strlen(buf);
    if (n != 36) return;
    for (size_t i = 0; i < n; i++)
        if (!boot_char_ok(buf[i])) return;
    memcpy(g_lock.boot, buf, n + 1);
}

/* 現在の状態を 一時ファイル + rename で原子的に保存する。成功で 0。
 * 形式 2: 壁時計の解除時刻に加えて、起動セッション識別子とモノトニックの解除時刻を
 * 保存する (同じ起動セッション内の再起動では壁時計を使わずに残り時間を復元するため)。
 * 起動セッションが不明なら "boot -"。 */
static int lockout_save(void) {
    static const char tmpname[] = ".lockout.tmp";
    char text[256];
    int len = snprintf(text, sizeof text,
                       "inmu-lockout 2\nfails %d\nuntil %lld\nsaved %lld\nboot %s\nmono %lld\n",
                       g_lock.fails, (long long)g_lock.until_wall, (long long)time(NULL),
                       g_lock.boot[0] ? g_lock.boot : "-", g_lock.until_mono_ms);
    if (len <= 0 || (size_t)len >= sizeof text) return -1;

    (void)unlinkat(g_lock.dirfd, tmpname, 0);
    int fd = openat(g_lock.dirfd, tmpname,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (fchmod(fd, 0600) != 0 || write_all(fd, text, (size_t)len) != 0 || fsync(fd) != 0) {
        close(fd);
        (void)unlinkat(g_lock.dirfd, tmpname, 0);
        return -1;
    }
    if (close(fd) != 0 || renameat(g_lock.dirfd, tmpname, g_lock.dirfd, g_lock.name) != 0) {
        (void)unlinkat(g_lock.dirfd, tmpname, 0);
        return -1;
    }
    (void)fsync(g_lock.dirfd);
    return 0;
}

/* 状態ファイルの本文をパースする。成功で 0。
 * 形式 1 (旧版が書いたもの。壁時計のみ) と形式 2 を受け付ける。
 * boot: 保存時の起動セッション識別子 (形式 1 と不明 "-" は "")。
 * until_mono: モノトニックの解除時刻 ms (形式 1 は 0)。 */
static int lockout_parse(const char *text, int *fails, time_t *until, time_t *saved,
                         char boot[40], long long *until_mono) {
    static const char header1[] = "inmu-lockout 1\nfails ";
    static const char header2[] = "inmu-lockout 2\nfails ";
    unsigned long long f = 0, u = 0, s = 0, m = 0;
    const char *p = text;
    int ver;
    if (strncmp(p, header1, sizeof header1 - 1) == 0) ver = 1;
    else if (strncmp(p, header2, sizeof header2 - 1) == 0) ver = 2;
    else return -1;
    p += sizeof header1 - 1;
    if (parse_dec(p, 4, &f, &p) != 0 || strncmp(p, "\nuntil ", 7) != 0) return -1;
    p += 7;
    if (parse_dec(p, 18, &u, &p) != 0 || strncmp(p, "\nsaved ", 7) != 0) return -1;
    p += 7;
    if (parse_dec(p, 18, &s, &p) != 0) return -1;
    boot[0] = 0;
    if (ver == 2) {
        if (strncmp(p, "\nboot ", 6) != 0) return -1;
        p += 6;
        size_t n = 0;
        while (boot_char_ok(p[n])) {
            if (n >= 36) return -1;
            n++;
        }
        if (n == 36) {
            memcpy(boot, p, n);
            boot[n] = 0;
        } else if (n != 1 || p[0] != '-') { /* "-" = 保存時に不明だった */
            return -1;
        }
        p += n;
        if (strncmp(p, "\nmono ", 6) != 0) return -1;
        p += 6;
        if (parse_dec(p, 18, &m, &p) != 0) return -1;
    }
    if (strcmp(p, "\n") != 0) return -1;
    if (f > LOCKOUT_FAILS_CAP) return -1;
    *fails = (int)f;
    *until = (time_t)u;
    *saved = (time_t)s;
    *until_mono = (long long)m;
    return 0;
}

/* 起動時に状態を読み込む。戻り 0 = 続行可、-1 = 起動中止。
 *   - ファイルが無い        → 初期状態 (失敗 0)
 *   - 読めない/壊れている/所有者・モードが不正
 *                           → 閾値到達扱いで上限時間のロックアウト (fail-closed)
 *   - 保存時と同じ起動セッション (OS を再起動していない)
 *                           → モノトニック時計の解除時刻から残り時間を復元する。
 *                             壁時計は見ない (時計を進めても戻しても短縮できない)
 *   - 起動セッションが違う/不明 → 壁時計で復元する (モノトニック時計は OS の起動で
 *                             0 に戻るため使えない):
 *       - 時計が巻き戻されている → ロックアウトを保存時点の残り時間でやり直す
 *   - 解除時刻が上限より先  → 上限に丸める */
static int lockout_load(void) {
    time_t now = time(NULL);
    long remaining = 0;
    int resave = 0;

    lockout_read_boot();

    int fd = openat(g_lock.dirfd, g_lock.name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        g_lock.fails = 0;
        g_lock.until_wall = 0;
    } else {
        int ok = 0;
        int fails = 0;
        time_t until = 0, saved = 0;
        char boot[40] = "";
        long long until_mono = 0;
        if (fd >= 0) {
            struct stat st;
            char text[256];
            if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && trusted_uid(st.st_uid) &&
                (st.st_mode & 077) == 0 && st.st_size > 0 &&
                st.st_size < (off_t)sizeof text) {
                ssize_t n = read(fd, text, sizeof text - 1);
                if (n == st.st_size) {
                    text[n] = 0;
                    if (strlen(text) == (size_t)n &&
                        lockout_parse(text, &fails, &until, &saved, boot, &until_mono) == 0)
                        ok = 1;
                }
            }
            close(fd);
        }
        if (!ok) {
            log_ts("!! ロックアウト状態ファイルが読めないか不正です — 安全側に倒し "
                   "%d 秒ロックアウトします",
                   INMU_LOCKOUT_MAX_SEC);
            g_lock.fails = INMU_LOCKOUT_THRESHOLD;
            remaining = INMU_LOCKOUT_MAX_SEC;
            resave = 1;
        } else {
            g_lock.fails = fails;
            if (g_lock.boot[0] && strcmp(boot, g_lock.boot) == 0) {
                /* 同じ起動セッション: モノトニック時計で復元する (壁時計は使わない) */
                long long left_ms = until_mono - mono_ms();
                if (until_mono > 0 && left_ms > 0) {
                    if (left_ms > (long long)INMU_LOCKOUT_MAX_SEC * 1000)
                        remaining = INMU_LOCKOUT_MAX_SEC;
                    else
                        remaining = (long)((left_ms + 999) / 1000);
                }
                /* 壁時計の解除時刻 (OS 再起動後の復元用) を現在の時計に合わせ直す */
                if (remaining > 0) resave = 1;
            } else if (now < saved) {
                /* 保存時より時計が戻っている: 残り時間を短縮させない */
                log_ts("!! 時計が状態ファイルの保存時刻より前です — ロックアウトをやり直します");
                remaining = until > saved ? (long)(until - saved > INMU_LOCKOUT_MAX_SEC
                                                       ? INMU_LOCKOUT_MAX_SEC
                                                       : until - saved)
                                          : 0;
                resave = 1;
            } else if (until > now) {
                remaining = until - now > INMU_LOCKOUT_MAX_SEC ? INMU_LOCKOUT_MAX_SEC
                                                               : (long)(until - now);
                /* 旧形式/別の起動セッションの記録を、現在の起動セッションと
                 * モノトニックの解除時刻を持つ形式で保存し直す (上限への丸めも反映) */
                resave = 1;
            }
        }
        g_lock.until_wall = remaining > 0 ? now + remaining : 0;
    }
    g_lock.until_mono_ms = remaining > 0 ? mono_ms() + (long long)remaining * 1000 : 0;

    if (resave && lockout_save() != 0) {
        log_ts("!! ロックアウト状態を保存できません (%s)", strerror(errno));
        return -1;
    }
    if (remaining > 0)
        log_ts("ロックアウト継続中: 残り %ld 秒 (連続失敗 %d 回)", remaining, g_lock.fails);
    else if (g_lock.fails > 0)
        log_ts("連続失敗カウンタを引き継ぎました: %d 回", g_lock.fails);
    return 0;
}

/* ロックアウト中か */
static int lockout_active(void) {
    return g_lock.until_mono_ms != 0 && mono_ms() < g_lock.until_mono_ms;
}

/* 定期処理: ロックアウト解除のログ */
static void lockout_tick(void) {
    if (g_lock.until_mono_ms != 0 && mono_ms() >= g_lock.until_mono_ms) {
        g_lock.until_mono_ms = 0;
        log_ts("ロックアウト解除 (連続失敗 %d 回のまま。次の不一致で再ロックアウト)",
               g_lock.fails);
    }
}

/* 照合の前に呼ぶ: これから行う 1 回の試行を「不一致だった場合の状態」として先に
 * 保存する。成功で 0。保存できなければ状態を元に戻して -1 を返し、呼び出し側は
 * 照合しない。
 * 照合の後で数えると、保存の失敗や照合〜保存の間の異常終了 (USB 抜き差しによる
 * 再起動を含む) で、数えられないまま照合結果だけが得られてしまう。 */
static int lockout_begin_attempt(void) {
    int prev_fails = g_lock.fails;
    time_t prev_wall = g_lock.until_wall;
    long long prev_mono = g_lock.until_mono_ms;

    if (g_lock.fails < LOCKOUT_FAILS_CAP) g_lock.fails++;
    long dur = lockout_duration_sec(g_lock.fails);
    if (dur > 0) {
        g_lock.until_wall = time(NULL) + dur;
        g_lock.until_mono_ms = mono_ms() + (long long)dur * 1000;
    }
    if (lockout_save() != 0) {
        int e = errno;
        g_lock.fails = prev_fails;
        g_lock.until_wall = prev_wall;
        g_lock.until_mono_ms = prev_mono;
        log_ts("!! ロックアウト状態を保存できません (%s) — 照合しません", strerror(e));
        return -1;
    }
    return 0;
}

/* 不一致 (記録は lockout_begin_attempt で保存済み)。ロックアウトに入ったら 1 を返す。 */
static int lockout_record_mismatch(void) {
    long dur = lockout_duration_sec(g_lock.fails);
    if (dur > 0)
        log_ts("指紋 不一致 (連続 %d 回) — %ld 秒ロックアウトします", g_lock.fails, dur);
    else
        log_ts("指紋 不一致 (連続 %d/%d 回)", g_lock.fails, INMU_LOCKOUT_THRESHOLD);
    return lockout_active();
}

/* 一致: lockout_begin_attempt で先に数えた分も含めてカウンタをリセットする。 */
static void lockout_record_match(void) {
    if (g_lock.fails == 0 && g_lock.until_wall == 0) return;
    g_lock.fails = 0;
    g_lock.until_wall = 0;
    g_lock.until_mono_ms = 0;
    /* 保存に失敗してもディスク上は厳しい側の値が残るだけなので続行する */
    if (lockout_save() != 0)
        log_ts("!! 連続失敗カウンタのリセットを保存できません (%s)", strerror(errno));
}

/* ================================================================ */
/* コンソールセッション (IORegistry ルートの IOConsoleUsers)          */
/* ================================================================ */

typedef struct {
    uid_t uid;      /* on-console セッションのユーザー */
    au_asid_t asid; /* そのセッションの audit session id */
    int locked;     /* 画面ロック中か */
} ConsoleState;

static int cf_to_ll(CFTypeRef v, long long *out) {
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return -1;
    if (CFNumberIsFloatType((CFNumberRef)v)) return -1;
    return CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, out) ? 0 : -1;
}

/* on-console セッションの状態を取得する。成功で 0。
 * on-console セッションがちょうど 1 つで、uid と audit session id が
 * 取れた場合だけ成功とする (それ以外は呼び出し側が拒否する)。 */
static int console_query(ConsoleState *out) {
    io_registry_entry_t root = IORegistryGetRootEntry(kIOMainPortDefault);
    if (root == MACH_PORT_NULL) return -1;
    CFTypeRef users =
        IORegistryEntryCreateCFProperty(root, CFSTR("IOConsoleUsers"), kCFAllocatorDefault, 0);
    CFTypeRef clocked =
        IORegistryEntryCreateCFProperty(root, CFSTR("IOConsoleLocked"), kCFAllocatorDefault, 0);
    IOObjectRelease(root);

    int found = 0;
    int locked = 0;
    long long uid = -1, asid = -1;

    /* IOConsoleLocked: 存在して false 以外ならロック中と見なす */
    if (clocked) {
        if (clocked != kCFBooleanFalse) locked = 1;
        CFRelease(clocked);
    }

    if (users && CFGetTypeID(users) == CFArrayGetTypeID()) {
        CFArrayRef arr = (CFArrayRef)users;
        CFIndex n = CFArrayGetCount(arr);
        for (CFIndex i = 0; i < n; i++) {
            CFTypeRef e = CFArrayGetValueAtIndex(arr, i);
            if (!e || CFGetTypeID(e) != CFDictionaryGetTypeID()) continue;
            CFDictionaryRef d = (CFDictionaryRef)e;
            if (CFDictionaryGetValue(d, CFSTR("kCGSSessionOnConsoleKey")) != kCFBooleanTrue)
                continue;
            found++;
            if (cf_to_ll(CFDictionaryGetValue(d, CFSTR("kCGSSessionUserIDKey")), &uid) != 0)
                uid = -1;
            if (cf_to_ll(CFDictionaryGetValue(d, CFSTR("kCGSSessionAuditIDKey")), &asid) != 0)
                asid = -1;
            /* CGSSessionScreenIsLocked: 存在して false 以外ならロック中 */
            CFTypeRef l = CFDictionaryGetValue(d, CFSTR("CGSSessionScreenIsLocked"));
            if (l && l != kCFBooleanFalse) locked = 1;
        }
    }
    if (users) CFRelease(users);

    if (found != 1) return -1;
    if (uid < 0 || uid > 0xffffffffLL) return -1;
    if (asid <= 0 || asid > INT_MAX) return -1;
    out->uid = (uid_t)uid;
    out->asid = (au_asid_t)asid;
    out->locked = locked;
    return 0;
}

/* ================================================================ */
/* peer 検証                                                         */
/* ================================================================ */

static int peer_token(int fd, audit_token_t *tok) {
    socklen_t len = sizeof *tok;
    memset(tok, 0, sizeof *tok);
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERTOKEN, tok, &len) != 0) return -1;
    return len == sizeof *tok ? 0 : -1;
}

/* pam.sock の peer: euid == 0 (テストモードではデーモン自身の uid) */
static int pam_peer_ok(int fd) {
    uid_t euid = (uid_t)-1;
    gid_t egid = (gid_t)-1;
    if (getpeereid(fd, &euid, &egid) != 0) return 0;
    return euid == g_pam_peer_uid;
}

/* agent.sock の peer: euid == 登録オーナー uid かつ 実行ファイル == inmu-agent */
static int agent_peer_ok(int fd) {
    if (!g_owner_known) return 0;
    uid_t euid = (uid_t)-1;
    gid_t egid = (gid_t)-1;
    if (getpeereid(fd, &euid, &egid) != 0 || euid != g_owner_uid) return 0;
    audit_token_t tok;
    if (peer_token(fd, &tok) != 0) return 0;
    if (audit_token_to_euid(tok) != g_owner_uid) return 0;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    memset(path, 0, sizeof path);
    if (proc_pidpath_audittoken(&tok, path, sizeof path) <= 0) return 0;
    path[sizeof path - 1] = 0;
    return strcmp(path, g_agent_path) == 0;
}

/* ================================================================ */
/* ソケット                                                       */
/* ================================================================ */

/* 検証済みディレクトリ (dirfd、かつカレントディレクトリ) の中に listen ソケットを
 * 作る。パスを辿り直さないよう相対名で bind し、モードは umask で最初から 0600
 * にする (bind〜chmod の隙を作らない)。owner != 0 なら所有者をその uid にする。 */
static int make_listener(int dirfd, const char *name, uid_t owner) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", name);

    if (unlinkat(dirfd, name, 0) != 0 && errno != ENOENT) {
        log_ts("!! 古いソケットを削除できません: %s (%s)", name, strerror(errno));
        close(fd);
        return -1;
    }
    mode_t old = umask(0177);
    int br = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    int berr = errno;
    umask(old);
    if (br != 0) {
        log_ts("!! bind 失敗: %s (%s)", name, strerror(berr));
        close(fd);
        return -1;
    }
    uid_t want = g_euid;
    if (owner != 0 && owner != g_euid) {
        if (fchownat(dirfd, name, owner, (gid_t)-1, AT_SYMLINK_NOFOLLOW) != 0) {
            log_ts("!! ソケットの所有者を変更できません: %s (%s)", name, strerror(errno));
            goto fail;
        }
        want = owner;
    }
    struct stat st;
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISSOCK(st.st_mode) ||
        (st.st_mode & 0777) != 0600 || st.st_uid != want) {
        log_ts("!! ソケットの所有者/モードを確認できません: %s", name);
        goto fail;
    }
    if (listen(fd, 8) != 0) {
        log_ts("!! listen 失敗: %s (%s)", name, strerror(errno));
        goto fail;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        log_ts("!! listen ソケットを非ブロッキングにできません: %s", name);
        goto fail;
    }
    return fd;

fail:
    (void)unlinkat(dirfd, name, 0);
    close(fd);
    return -1;
}

/* 受け付けた接続の初期設定 (非ブロッキング、SIGPIPE 抑止)。成功で 0。 */
static int setup_client_fd(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0) return -1;
    return 0;
}

/* 短い応答を全部送る。部分書き込みは続きを送り、送れなければ失敗 (-1)。 */
static int send_all(int fd, const char *msg) {
    size_t len = strlen(msg);
    while (len > 0) {
        ssize_t n = send(fd, msg, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1; /* EAGAIN (相手が読まない) も失敗扱い */
        }
        if (n == 0) return -1;
        msg += n;
        len -= (size_t)n;
    }
    return 0;
}

static void conn_reset(Conn *c) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static void conn_close(Conn *c) {
    if (c->fd >= 0) close(c->fd);
    conn_reset(c);
}

/* 応答を送って閉じる。送れたら 0。 */
static int conn_reply_close(Conn *c, const char *msg) {
    int r = send_all(c->fd, msg);
    conn_close(c);
    return r;
}

/* 接続から 1 行取り出す ('\n' 終端、非ブロッキング)。
 * 1 回の呼び出しで recv は高々 1 回・返す行も高々 1 行 (他の接続を飢えさせない)。
 * 戻り: 1 = 行あり (line は NUL 終端、INMU_IPC_LINE_MAX バイト以上の領域)、
 *       0 = まだ、-1 = 切断/エラー、-2 = 行が長すぎる/不正なバイト。 */
static int conn_read_line(Conn *c, char *line) {
    char *nl = memchr(c->buf, '\n', c->len);
    if (!nl) {
        size_t room = sizeof c->buf - 1 - c->len;
        if (room == 0) return -2;
        ssize_t n = recv(c->fd, c->buf + c->len, room, 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            return -1;
        }
        c->len += (size_t)n;
        nl = memchr(c->buf, '\n', c->len);
        if (!nl) return c->len >= sizeof c->buf - 1 ? -2 : 0;
    }
    size_t linelen = (size_t)(nl - c->buf);
    if (memchr(c->buf, 0, linelen)) return -2;
    memcpy(line, c->buf, linelen);
    line[linelen] = 0;
    c->len -= linelen + 1;
    memmove(c->buf, nl + 1, c->len);
    return 1;
}

static void compact(Conn *arr, int *n) {
    int w = 0;
    for (int i = 0; i < *n; i++) {
        if (arr[i].fd < 0) continue;
        if (w != i) arr[w] = arr[i];
        w++;
    }
    for (int i = w; i < *n; i++) conn_reset(&arr[i]);
    *n = w;
}

/* ================================================================ */
/* PAM / AGENT クライアント管理                                      */
/* ================================================================ */

/* "WAIT <timeout_ms> <uid>" を厳密にパースする。成功で 0。
 * timeout_ms は 1..INMU_PAM_MAX_TIMEOUT_MS に丸める (ipc.h)。 */
static int parse_wait(const char *line, long *timeout_ms, uid_t *uid) {
    unsigned long long t = 0, u = 0;
    const char *p = line;
    if (strncmp(p, "WAIT ", 5) != 0) return -1;
    p += 5;
    if (parse_dec(p, 10, &t, &p) != 0 || *p != ' ') return -1;
    p++;
    if (parse_dec(p, 10, &u, &p) != 0 || *p != 0) return -1;
    if (u > 0xffffffffULL) return -1;
    if (t < 1) t = 1;
    if (t > INMU_PAM_MAX_TIMEOUT_MS) t = INMU_PAM_MAX_TIMEOUT_MS;
    *timeout_ms = (long)t;
    *uid = (uid_t)u;
    return 0;
}

/* WAIT 済みの PAM 要求すべてに NG を返す (ロックアウト開始時) */
static void pam_reject_all_waiting(const char *why) {
    for (int i = 0; i < n_pam_clients; i++) {
        Conn *c = &pam_clients[i];
        if (c->fd < 0 || !c->ready) continue;
        log_ts("PAM 要求を拒否 → NG (fd=%d): %s", c->fd, why);
        (void)conn_reply_close(c, "NG\n");
    }
    compact(pam_clients, &n_pam_clients);
}

/* PAM 要求を承認できない理由を返す (承認可なら NULL)。MATCH の時点で呼ぶ。 */
static const char *pam_deny_reason(const Conn *c, int cs_ok, const ConsoleState *cs) {
    if (!g_owner_known) return "登録オーナーが不明";
    if (c->uid != g_owner_uid) return "要求 uid が登録オーナーではない";
    if (!cs_ok) return "コンソールセッションの情報を取得できない";
    if (cs->uid != g_owner_uid) return "コンソールユーザーが登録オーナーではない";
    if (cs->locked) return "画面ロック中";
    audit_token_t tok;
    if (peer_token(c->fd, &tok) != 0) return "要求元の audit token を取得できない";
    if (audit_token_to_asid(tok) != cs->asid)
        return "要求元がコンソール GUI セッションではない (SSH 等)";
    return NULL;
}

/* agent へ MATCH を通知してよい状態か: 登録オーナーのコンソールセッションが
 * 画面ロック中の時だけ。agent は受け取った MATCH の鮮度を判断できないので、
 * 非ロック時の一致 (sudo の承認など) を通知すると、読まれずに残った MATCH が
 * 後のロック解除に使われうる。判断できないときは通知しない。 */
static int agent_notify_allowed(int cs_ok, const ConsoleState *cs) {
    return g_owner_known && cs_ok && cs->locked && cs->uid == g_owner_uid;
}

/* この接触を撮影・照合する意味があるか。
 * 期限内の WAIT 済み PAM 要求があるか、agent へ通知できる状態 (SUB 済みの購読者が
 * いて agent_notify_allowed) のどちらかを満たす時だけ 1。どちらでもない接触の
 * 照合結果は誰にも使われず、不一致だけがロックアウトに数えられてしまう。 */
static int capture_wanted(void) {
    long long now = mono_ms();
    for (int i = 0; i < n_pam_clients; i++) {
        const Conn *c = &pam_clients[i];
        if (c->fd >= 0 && c->ready && now <= c->deadline_ms) return 1;
    }
    int subscribed = 0;
    for (int i = 0; i < n_agent_clients; i++)
        if (agent_clients[i].fd >= 0 && agent_clients[i].ready) subscribed = 1;
    if (!subscribed) return 0;
    ConsoleState cs;
    memset(&cs, 0, sizeof cs);
    int cs_ok = console_query(&cs) == 0;
    return agent_notify_allowed(cs_ok, &cs);
}

/* MATCH 1 回分の割り当て。
 * cap_seq: 撮影開始時点の WAIT 通し番号。これ以下の番号の要求だけが対象。 */
static void dispatch_match(unsigned long long cap_seq) {
    ConsoleState cs;
    memset(&cs, 0, sizeof cs);
    int cs_ok = console_query(&cs) == 0;
    int approved = 0;

    /* PAM: 撮影開始より前に WAIT 済みのものを古い順に見て、最初に条件を満たした
     * 1 件だけを承認する。条件を満たさないものは NG で閉じる。
     * 撮影開始後に WAIT したものには触れない (待機のまま)。 */
    while (!approved) {
        Conn *c = NULL;
        for (int i = 0; i < n_pam_clients; i++) {
            Conn *x = &pam_clients[i];
            if (x->fd < 0 || !x->ready || x->wait_seq > cap_seq) continue;
            if (!c || x->wait_seq < c->wait_seq) c = x;
        }
        if (!c) break;
        int fd = c->fd;
        if (mono_ms() > c->deadline_ms) {
            log_ts("PAM WAIT 期限切れ → NG (fd=%d)", fd);
            (void)conn_reply_close(c, "NG\n");
            continue;
        }
        const char *why = pam_deny_reason(c, cs_ok, &cs);
        if (why) {
            log_ts("PAM 要求を拒否 → NG (fd=%d): %s", fd, why);
            (void)conn_reply_close(c, "NG\n");
            continue;
        }
        if (conn_reply_close(c, "OK\n") == 0) {
            log_ts("PAM 要求を承認 → OK (fd=%d)", fd);
            approved = 1;
        } else {
            log_ts("PAM への OK 送信失敗 (fd=%d) — 次の要求を確認します", fd);
        }
    }
    compact(pam_clients, &n_pam_clients);

    /* AGENT: 登録オーナーのセッションが画面ロック中の時だけ、SUB 済みの購読者へ
     * 通知する (送信直前に peer を再検証する) */
    int notified = 0;
    int notify = agent_notify_allowed(cs_ok, &cs);
    for (int i = 0; notify && i < n_agent_clients; i++) {
        Conn *c = &agent_clients[i];
        if (c->fd < 0 || !c->ready) continue;
        if (!agent_peer_ok(c->fd) || send_all(c->fd, "MATCH\n") != 0) {
            log_ts("agent 購読者を切断 (fd=%d): 再検証または送信に失敗", c->fd);
            conn_close(c);
            continue;
        }
        notified++;
    }
    compact(agent_clients, &n_agent_clients);

    log_ts("MATCH 割り当て: PAM 承認 %d 件 / agent 通知 %d 件%s", approved, notified,
           cs_ok ? (cs.locked ? " (画面ロック中)" : "") : " (コンソール情報なし)");
}

static void accept_clients(int listen_fd, int is_pam) {
    Conn *arr = is_pam ? pam_clients : agent_clients;
    int *n = is_pam ? &n_pam_clients : &n_agent_clients;
    int max = is_pam ? MAX_PAM_CLIENTS : MAX_AGENT_CLIENTS;
    const char *kind = is_pam ? "PAM" : "agent";

    for (int k = 0; k < ACCEPT_BURST; k++) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) break;
        if (setup_client_fd(fd) != 0) {
            close(fd);
            continue;
        }
        /* peer 検証 (満たさなければ何も返さず即切断) */
        if (!(is_pam ? pam_peer_ok(fd) : agent_peer_ok(fd))) {
            log_ts("%s 接続を拒否: peer 検証に失敗", kind);
            close(fd);
            continue;
        }
        if (*n >= max && !is_pam) {
            /* agent: 最も古い接続 (配列は受付順) を切断して枠を空ける。検証を通った
             * 接続で枠を埋められても、正規の agent の再接続を締め出させない。 */
            log_ts("agent 接続数の上限: 最も古い接続を切断します (fd=%d)", arr[0].fd);
            conn_close(&arr[0]);
            compact(arr, n);
        }
        if (*n >= max) {
            log_ts("%s 接続を拒否: 接続数の上限", kind);
            if (is_pam) (void)send_all(fd, "ERR\n");
            close(fd);
            continue;
        }
        Conn *c = &arr[*n];
        conn_reset(c);
        c->fd = fd;
        c->accept_ms = mono_ms();
        (*n)++;
        log_ts("%s 接続 (fd=%d)", kind, fd);
    }
}

/* ソケットを最大 wait_ms 待ってから 1 巡処理する。 */
static void service_sockets(int wait_ms) {
    char line[INMU_IPC_LINE_MAX];
    struct pollfd pfds[2 + MAX_PAM_CLIENTS + MAX_AGENT_CLIENTS];
    nfds_t np = 0;

    pfds[np++] = (struct pollfd){.fd = g_pam_listen, .events = POLLIN};
    pfds[np++] = (struct pollfd){.fd = g_agent_listen, .events = POLLIN};
    for (int i = 0; i < n_pam_clients; i++)
        pfds[np++] = (struct pollfd){.fd = pam_clients[i].fd, .events = POLLIN};
    for (int i = 0; i < n_agent_clients; i++)
        pfds[np++] = (struct pollfd){.fd = agent_clients[i].fd, .events = POLLIN};
    /* 待つだけが目的 (結果は見ず、下で全接続を非ブロッキングで 1 回ずつ処理する) */
    (void)poll(pfds, np, wait_ms);

    accept_clients(g_pam_listen, 1);
    accept_clients(g_agent_listen, 0);

    long long now = mono_ms();

    /* PAM 要求の読み取り (1 接続につき 1 回の recv・1 行まで) */
    for (int i = 0; i < n_pam_clients; i++) {
        Conn *c = &pam_clients[i];
        if (c->fd < 0) continue;
        int fd = c->fd;
        int r = conn_read_line(c, line);
        if (r == -1) {
            conn_close(c);
            continue;
        }
        if (r == -2 || (c->ready && (r == 1 || c->len > 0))) {
            /* 行が長すぎる / WAIT は 1 接続につき 1 回 */
            log_ts("PAM プロトコル違反 → ERR (fd=%d)", fd);
            (void)conn_reply_close(c, "ERR\n");
            continue;
        }
        if (r == 1) {
            long timeout_ms = 0;
            uid_t uid = 0;
            if (parse_wait(line, &timeout_ms, &uid) != 0) {
                log_ts("PAM 不正な要求 → ERR (fd=%d)", fd);
                (void)conn_reply_close(c, "ERR\n");
                continue;
            }
            /* 待っても承認され得ない要求は即答してパスワードへ進ませる。
             * 要求元の audit session がコンソール GUI セッションと違うと分かって
             * いる場合 (SSH 等) もここで拒否する。コンソール情報や audit token を
             * 取得できない場合は待機させる (承認条件は MATCH の時点で改めて
             * すべて判定するので、ここを通っても承認されるとは限らない)。 */
            const char *why = NULL;
            ConsoleState wcs;
            audit_token_t wtok;
            if (!g_owner_known) why = "登録オーナーが不明";
            else if (uid != g_owner_uid) why = "要求 uid が登録オーナーではない";
            else if (lockout_active()) why = "ロックアウト中";
            else if (console_query(&wcs) == 0 && peer_token(fd, &wtok) == 0 &&
                     audit_token_to_asid(wtok) != wcs.asid)
                why = "要求元がコンソール GUI セッションではない (SSH 等)";
            if (why) {
                log_ts("PAM 要求を拒否 → NG (fd=%d uid=%u): %s", fd, (unsigned)uid, why);
                (void)conn_reply_close(c, "NG\n");
                continue;
            }
            c->ready = 1;
            c->uid = uid;
            c->deadline_ms = now + timeout_ms;
            c->wait_seq = ++g_wait_seq;
            log_ts("PAM WAIT %ldms 受付 (fd=%d uid=%u)", timeout_ms, fd, (unsigned)uid);
            if (c->len > 0) {
                log_ts("PAM プロトコル違反 → ERR (fd=%d)", fd);
                (void)conn_reply_close(c, "ERR\n");
            }
            continue;
        }
    }

    /* AGENT SUB の読み取り */
    for (int i = 0; i < n_agent_clients; i++) {
        Conn *c = &agent_clients[i];
        if (c->fd < 0) continue;
        int fd = c->fd;
        int r = conn_read_line(c, line);
        if (r < 0 || (r == 1 && strcmp(line, "SUB") != 0)) {
            if (r != -1) log_ts("agent プロトコル違反 → 切断 (fd=%d)", fd);
            conn_close(c);
            continue;
        }
        if (r == 1 && !c->ready) {
            c->ready = 1;
            log_ts("agent 購読開始 (fd=%d)", fd);
        }
    }

    /* 期限切れ: PAM WAIT → NG (パスワードフォールバック)、WAIT/SUB 未送信 → 切断 */
    now = mono_ms();
    for (int i = 0; i < n_pam_clients; i++) {
        Conn *c = &pam_clients[i];
        if (c->fd < 0) continue;
        if (c->ready && now > c->deadline_ms) {
            log_ts("PAM WAIT 期限切れ → NG (fd=%d)", c->fd);
            (void)conn_reply_close(c, "NG\n");
        } else if (!c->ready && now - c->accept_ms > HELLO_TIMEOUT_MS) {
            log_ts("PAM WAIT が届かない → ERR (fd=%d)", c->fd);
            (void)conn_reply_close(c, "ERR\n");
        }
    }
    for (int i = 0; i < n_agent_clients; i++) {
        Conn *c = &agent_clients[i];
        if (c->fd >= 0 && !c->ready && now - c->accept_ms > HELLO_TIMEOUT_MS) {
            log_ts("agent SUB が届かない → 切断 (fd=%d)", c->fd);
            conn_close(c);
        }
    }

    compact(pam_clients, &n_pam_clients);
    compact(agent_clients, &n_agent_clients);
}

/* ================================================================ */
/* センサー照合ループ                                               */
/* ================================================================ */

static int finger_loop(Ft9201 *ft, const uint8_t *tmpl, size_t tmpllen) {
    uint8_t reg[4];
    size_t imglen = (size_t)ft->width * ft->height;
    if (imglen == 0) return -1;
    uint8_t *img = malloc(imglen);
    if (!img) return -1;
    uint8_t quality[2] = {0, 0};

    /* 1 回撮影したら (一致/不一致/品質不足/失敗を問わず) 指が離れるまで次の撮影を
     * しない。起動直後も、指が置かれたままなら一度離すまで撮影しない。 */
    int need_lift = 1;
    int absent = 0;
    int idle_logged = 0;

    log_ts("照合待機開始 (指を検知したら撮影→照合します)");

    while (!g_stop) {
        /* 指が離れるのを待つ間もソケットは処理する (期限切れ NG など) */
        service_sockets(need_lift ? 20 : 100);
        lockout_tick();

        if (ft9201_reg_read(ft, REG_FINGER, reg, 4) != 0) {
            log_ts("!! 指状態読み取り失敗 — USB エラー。終了します");
            free(img);
            return -1;
        }
        if (reg[0] != 0xa0) {
            if (absent < LIFT_DEBOUNCE) absent++;
            if (absent >= LIFT_DEBOUNCE) {
                need_lift = 0;
                idle_logged = 0;
            }
            continue;
        }
        absent = 0;
        if (need_lift) continue;
        need_lift = 1;

        /* 承認も通知もできない状態では撮影・照合自体を行わない */
        if (!g_owner_known || lockout_active() || !capture_wanted()) {
            if (!idle_logged) {
                log_ts("指を検知しましたが照合しません (%s)",
                       !g_owner_known ? "登録オーナーが不明"
                       : lockout_active() ? "ロックアウト中"
                                          : "待機中の要求が無く、画面ロック中の通知先も無い");
                idle_logged = 1;
            }
            continue;
        }

        /* 撮影開始。これより後に WAIT した PAM 要求はこの撮影では承認しない */
        unsigned long long cap_seq = g_wait_seq;

        /* セットリング → 撮影 → 照合 */
        usleep(50 * 1000);
        if (ft9201_capture_image(ft, img, quality) != 0) {
            log_ts("!! 画像取得失敗");
            continue;
        }
        log_ts("画像取得 (quality=%02x %02x)", quality[0], quality[1]);

        uint32_t hr = ft_engine_accept(img, ft->width, ft->height, 1 /* verify */);
        if (hr != 0) {
            log_ts("accept reject (hr=%#x) — 品質不足。指を置き直してください", hr);
            continue;
        }

        /* 試行を先に保存してから照合する (保存できなければ照合しない) */
        if (lockout_begin_attempt() != 0) continue;

        if (ft_engine_verify(tmpl, tmpllen)) {
            log_ts("★ 指紋 MATCH");
            lockout_record_match();
            dispatch_match(cap_seq);
        } else if (lockout_record_mismatch()) {
            /* ロックアウト開始: 待機中の要求はすぐパスワードへ進ませる */
            pam_reject_all_waiting("ロックアウト中");
        }
    }

    free(img);
    return 0;
}

/* ================================================================ */
/* main                                                             */
/* ================================================================ */

/* テンプレートを読み込む。symlink 不可、通常ファイル、信頼できる所有者、
 * group/other アクセス不可でなければ拒否する。 */
static uint8_t *load_template(const char *path, size_t *outlen) {
    struct stat st;
    int fd = open_verified_file(path, S_IRWXG | S_IRWXO, &st, "テンプレート");
    if (fd < 0) return NULL;
    /* 上限はエンジンが照合に使える大きさ (超えると照合されず常に不一致になる) */
    if (st.st_size <= 0 || (unsigned long long)st.st_size > FT_ENGINE_TEMPLATE_MAX) {
        log_ts("!! テンプレートのサイズが不正です: %lld bytes (上限 %u)", (long long)st.st_size,
               (unsigned)FT_ENGINE_TEMPLATE_MAX);
        close(fd);
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *p = malloc(len);
    if (!p) {
        close(fd);
        return NULL;
    }
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != len) {
        free(p);
        return NULL;
    }
    *outlen = len;
    return p;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: sudo %s --template tmpl.bin --engine DLL\n"
            "  テスト用 (非 root でユーザー領域に置く場合は --sockdir と --state-dir が必要):\n"
            "    [--sockdir %s] [--state-dir %s]\n"
            "    [--owner-file %s] [--agent-path %s]\n",
            argv0, INMU_SOCKET_DIR, INMU_STATE_DIR, INMU_OWNER_FILE, INMU_AGENT_PATH);
}

int main(int argc, char **argv) {
    const char *template_path = NULL;
    const char *engine_path = NULL;
    const char *sockdir = INMU_SOCKET_DIR;
    const char *statedir = INMU_STATE_DIR;
    const char *owner_file = INMU_OWNER_FILE;

    /* 自分が作るファイル (ログ・状態・ソケット) を他ユーザーから見えなくする */
    umask(077);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--template") && i + 1 < argc) template_path = argv[++i];
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine_path = argv[++i];
        else if (!strcmp(argv[i], "--sockdir") && i + 1 < argc) sockdir = argv[++i];
        else if (!strcmp(argv[i], "--state-dir") && i + 1 < argc) statedir = argv[++i];
        else if (!strcmp(argv[i], "--owner-file") && i + 1 < argc) owner_file = argv[++i];
        else if (!strcmp(argv[i], "--agent-path") && i + 1 < argc) g_agent_path = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "不明な引数です: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (!template_path || !engine_path) {
        fprintf(stderr, "--template と --engine が必要です (-h 参照)\n");
        return 2;
    }
    g_euid = geteuid();
    /* root 必須は既定のパスを使う場合のみ (テスト用に両方差し替えればユーザー実行可) */
    if (g_euid != 0 && (!strcmp(sockdir, INMU_SOCKET_DIR) || !strcmp(statedir, INMU_STATE_DIR))) {
        fprintf(stderr, "既定のソケット/状態ディレクトリには root が必要です (sudo で実行、"
                        "または --sockdir と --state-dir でユーザー領域を指定)\n");
        return 2;
    }
    /* pam.sock の peer は root のみ。非 root 実行 (テストモード) では自分自身のみ */
    g_pam_peer_uid = g_euid;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    log_ts("inmu-cored 起動 (template=%s, engine=%s)", template_path, engine_path);
    if (g_euid != 0)
        log_ts("!! 非 root のテストモードです (pam.sock の peer は uid=%u のみ許可)",
               (unsigned)g_euid);

    int rc = 1;
    int sockdir_fd = -1;
    int usb_open = 0;
    uint8_t *tmpl = NULL;
    size_t tmpllen = 0;
    Ft9201Usb u;
    Ft9201 ft;
    struct stat est;
    int efd;
    const char *lock_name = strrchr(INMU_LOCKOUT_FILE, '/');

    /* 登録オーナー */
    if (load_owner(owner_file, &g_owner_uid) == 0) {
        g_owner_known = 1;
        log_ts("登録オーナー uid=%u", (unsigned)g_owner_uid);
    } else {
        log_ts("!! 登録オーナーが不明です (%s) — PAM 承認も agent 通知も行いません。"
               "inmu-enroll で登録し直してください",
               owner_file);
    }

    /* ロックアウト状態 (センサーに触る前に読み、読めなければ起動しない) */
    g_lock.name = lock_name ? lock_name + 1 : INMU_LOCKOUT_FILE;
    g_lock.dirfd = open_verified_dir(statedir, 0700, "状態ディレクトリ");
    if (g_lock.dirfd < 0 || lockout_load() != 0) goto out;

    /* ソケットディレクトリ (symlink 不可・信頼できる所有者・他者書き込み不可) */
    sockdir_fd = open_verified_dir(sockdir, 0755, "ソケットディレクトリ");
    if (sockdir_fd < 0) goto out;

    /* テンプレート読み込み */
    tmpl = load_template(template_path, &tmpllen);
    if (!tmpl) {
        fprintf(stderr, "テンプレートが読めません: %s\n", template_path);
        goto out;
    }
    log_ts("テンプレート読み込み: %zu bytes", tmpllen);

    /* matcher DLL は root 権限で実行されるコード: 他者が書き換えられるものは拒否 */
    efd = open_verified_file(engine_path, S_IWGRP | S_IWOTH, &est, "matcher DLL");
    if (efd < 0) goto out;
    close(efd);

    /* センサー bring-up */
    if (ft_usb_find(&u) != 0) {
        fprintf(stderr, "FT9201 が見つかりません (接続を確認)\n");
        goto out;
    }
    if (ft_usb_open(&u) != 0) {
        fprintf(stderr, "USB オープン失敗\n");
        goto out;
    }
    usb_open = 1;
    if (ft9201_init(&ft, &u) != 0) {
        fprintf(stderr, "bring-up 失敗 — センサーを抜き差しして再試行\n");
        goto out;
    }
    if (ft9201_capture_background(&ft) != 0) {
        fprintf(stderr, "背景 (FPN) 取得失敗\n");
        goto out;
    }

    /* matcher (vendor DLL, Rosetta 2) */
    if (ft_engine_open(engine_path) != 0) {
        fprintf(stderr, "matcher DLL ロード失敗: %s\n", engine_path);
        goto out;
    }
    /* テンプレートを engine のストレージに読み込ませる (verify 用) */
    (void)ft_engine_verify(tmpl, tmpllen);
    log_ts("matcher 準備完了");

    /* ソケット: 検証済みディレクトリをカレントにして相対名で bind する
     * (検証後にパスを差し替えられても別の場所に作らない) */
    if (fchdir(sockdir_fd) != 0) {
        log_ts("!! ソケットディレクトリへ移動できません (%s)", strerror(errno));
        goto out;
    }
    g_pam_listen = make_listener(sockdir_fd, "pam.sock", 0);
    if (g_pam_listen >= 0)
        g_agent_listen = make_listener(sockdir_fd, "agent.sock", g_owner_known ? g_owner_uid : 0);
    if (chdir("/") != 0) log_ts("!! / へ移動できません (%s)", strerror(errno));
    if (g_pam_listen < 0 || g_agent_listen < 0) {
        fprintf(stderr, "ソケット作成失敗 (%s)\n", sockdir);
        goto out;
    }
    if (g_owner_known)
        log_ts("IPC 準備完了: %s/pam.sock (0600) / %s/agent.sock (0600, uid=%u)", sockdir,
               sockdir, (unsigned)g_owner_uid);
    else
        log_ts("IPC 準備完了: %s/pam.sock (0600) / %s/agent.sock (0600, オーナー不明)", sockdir,
               sockdir);

    rc = finger_loop(&ft, tmpl, tmpllen) == 0 ? 0 : 1;

out:
    log_ts("終了中...");
    for (int i = 0; i < n_pam_clients; i++) conn_close(&pam_clients[i]);
    for (int i = 0; i < n_agent_clients; i++) conn_close(&agent_clients[i]);
    n_pam_clients = n_agent_clients = 0;
    if (g_pam_listen >= 0) {
        (void)unlinkat(sockdir_fd, "pam.sock", 0);
        close(g_pam_listen);
    }
    if (g_agent_listen >= 0) {
        (void)unlinkat(sockdir_fd, "agent.sock", 0);
        close(g_agent_listen);
    }
    if (sockdir_fd >= 0) close(sockdir_fd);
    if (g_lock.dirfd >= 0) close(g_lock.dirfd);
    if (usb_open) ft_usb_close(&u);
    free(tmpl);
    return rc;
}
