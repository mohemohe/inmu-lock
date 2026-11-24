/*
 * ipc.h — inmu-lock MVP (T4): コンポーネント間 Unix domain socket プロトコル。
 *
 * トランスポート: Unix domain socket (SOCK_STREAM)、行ベーステキストプロトコル。
 *   (docs/01 は JSON lines を想定していたが、MVP は依存なしの最小行プロトコル。
 *    本番化の際に JSON 化可能)
 *
 *   PAM → core:    "WAIT <timeout_ms>\n"   core → PAM: "OK\n" | "NG\n" | "ERR\n"
 *   AGENT → core:  "SUB\n"                 core → AGENT: "MATCH\n"
 *
 * ソケットの ACL:
 *   pam.sock   0600 root:wheel — 接続は root のみ (PAM は sudo 内で euid 0)
 *   agent.sock 0666 root:wheel — 購読は誰でも可 (MATCH 通知は無害な情報のみ。
 *              注入の制御は agent 側のロック判定と Keychain が担う)
 */

#ifndef INMU_IPC_H
#define INMU_IPC_H

#define INMU_SOCKET_DIR "/var/run/inmu"
#define INMU_PAM_SOCK INMU_SOCKET_DIR "/pam.sock"
#define INMU_AGENT_SOCK INMU_SOCKET_DIR "/agent.sock"

/* PAM モジュールのデフォルト待ち時間 (ms)。
 * 指を触らなければこの時間後にパスワード入力へフォールバックする。 */
#define INMU_PAM_DEFAULT_TIMEOUT_MS 3000

#endif /* INMU_IPC_H */
