/*
 * ipc.h — inmu-lock: コンポーネント間 Unix domain socket プロトコルと共有定数。
 *
 * トランスポート: Unix domain socket (SOCK_STREAM)、行ベーステキストプロトコル
 * (1 行は '\n' 終端、INMU_IPC_LINE_MAX 未満)。
 *
 *   PAM → core:    "WAIT <timeout_ms> <uid>\n"
 *                    timeout_ms: 1..INMU_PAM_MAX_TIMEOUT_MS (範囲外は core 側で丸める)
 *                    uid:        認証対象ユーザー (PAM_USER) の数値 uid
 *   core → PAM:    "OK\n"  (その要求に対して指紋 MATCH を 1 件割り当てた)
 *                  "NG\n"  (期限切れ / ポリシーにより拒否)
 *                  "ERR\n" (プロトコルエラー / 受付不可)
 *   AGENT → core:  "SUB\n"
 *   core → AGENT:  "MATCH\n"
 *
 * 接続の検証 (すべて必須。満たさない接続は即切断する):
 *   pam.sock   0600 root — core は peer の euid == 0 を検証する。
 *              PAM モジュール側も peer (core) の euid == 0 を検証する。
 *   agent.sock 0600 owner — core は peer の euid == 登録オーナー uid、かつ
 *              peer の実行ファイルパス == INMU_AGENT_PATH を検証する。
 *              agent 側も peer (core) の euid == 0 を検証する。
 *
 * core の承認ポリシー (MATCH 1 回につき):
 *   - PAM 要求は「撮影開始より前に WAIT 済み」のもののうち最も古い 1 件だけを承認する。
 *   - PAM 要求の承認条件: 要求 uid == 登録オーナー uid == コンソールユーザー uid、
 *     画面がロックされていない、要求元プロセスの audit session がコンソール GUI
 *     セッションと一致する (SSH 等のリモート/別セッションは不可)。
 *     uid 不一致・別セッションなど、待っても満たせない WAIT は受付時点で NG を返す。
 *   - agent への MATCH 通知は、登録オーナーのコンソールセッションが画面ロック中の
 *     時だけ行う (非ロック時の一致を通知すると、後のロック解除に使われうる)。
 *   - 期限内の PAM 要求も通知できる agent も無い時の接触は、撮影も照合もしない。
 *   - 連続失敗によるロックアウト中は承認も MATCH 通知も行わない。
 */

#ifndef INMU_IPC_H
#define INMU_IPC_H

#define INMU_SOCKET_DIR "/var/run/inmu"
#define INMU_PAM_SOCK INMU_SOCKET_DIR "/pam.sock"
#define INMU_AGENT_SOCK INMU_SOCKET_DIR "/agent.sock"

/* 1 行の最大長 (終端 NUL を含むバッファサイズ) */
#define INMU_IPC_LINE_MAX 128

/* PAM モジュールのデフォルト待ち時間 (ms)。
 * 指を触らなければこの時間後にパスワード入力へフォールバックする。 */
#define INMU_PAM_DEFAULT_TIMEOUT_MS 5000
/* WAIT で指定できる最大待ち時間 (ms) */
#define INMU_PAM_MAX_TIMEOUT_MS 60000

/* インストール先 (scripts/install.sh と一致させること) */
#define INMU_AGENT_PATH "/usr/local/bin/inmu-agent"
#define INMU_ETC_DIR "/usr/local/etc/inmu-lock"
#define INMU_TEMPLATE_FILE INMU_ETC_DIR "/template.bin"
#define INMU_ENGINE_DLL "/usr/local/libexec/inmu-lock/ftWbioEngineAdapter.dll"

/* 登録オーナー (指紋の持ち主) の uid を 10 進テキスト 1 行で保持するファイル。
 * root:wheel 0644。inmu-enroll / install.sh が書き、inmu-cored が起動時に読む。
 * 存在しない・不正な場合、core は一切の承認を行わない (fail-closed)。 */
#define INMU_OWNER_FILE INMU_ETC_DIR "/owner"

/* 連続失敗カウンタ等の永続状態 (root 0700 ディレクトリ / 0600 ファイル)。
 * デーモン再起動 (USB 抜き差し含む) でリセットされないよう永続化する。 */
#define INMU_STATE_DIR "/var/db/inmu-lock"
#define INMU_LOCKOUT_FILE INMU_STATE_DIR "/lockout"

/* 連続不一致がこの回数に達したらロックアウトを開始する */
#define INMU_LOCKOUT_THRESHOLD 5
/* ロックアウト時間 (秒): 初回 BASE、以後失敗のたびに倍、上限 MAX */
#define INMU_LOCKOUT_BASE_SEC 60
#define INMU_LOCKOUT_MAX_SEC 900

#endif /* INMU_IPC_H */
