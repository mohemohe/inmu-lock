#!/bin/bash
# daemon.sh — cored (LaunchDaemon) の冪等なライフサイクル操作。
#
#   ./scripts/daemon.sh stop      停止 (既に停止中なら何もしない)
#   ./scripts/daemon.sh start     起動 (既に起動中なら何もしない)
#   ./scripts/daemon.sh restart   再起動 (起動中なら kickstart、停止中なら bootstrap)
#   ./scripts/daemon.sh status    状態表示
#
# Makefile ターゲット (make stop-daemon 等) から呼ばれる。
# root 権限が必要なため、sudo make 経由で実行するか、このスクリプトが
# root 以外から呼ばれた場合は sudo を使う (TTY がある前提)。

set -u

DAEMON_ID="com.mohemohe.inmu-lock.cored"
DAEMON_PLIST="/Library/LaunchDaemons/com.mohemohe.inmu-lock.cored.plist"

# root でない場合は sudo を使う (TTY がない Makefile 経由では失敗するため、
# Makefile からは sudo make 経由で呼ぶことを想定)。
# inmu_as_root は引数をそのまま渡す (変数の単語分割に依存しない)。
# shellcheck source=scripts/common.sh
. "$(dirname "$0")/common.sh" || exit 1

is_loaded() {
    inmu_as_root launchctl print "system/$DAEMON_ID" >/dev/null 2>&1
}

do_stop() {
    if is_loaded; then
        if inmu_as_root launchctl bootout "system/$DAEMON_ID"; then
            echo "  cored 停止しました"
        else
            echo "  !! cored 停止に失敗" >&2
            return 1
        fi
    else
        echo "  cored は既に停止中です"
    fi
}

do_start() {
    if is_loaded; then
        echo "  cored は既に起動中です"
        return 0
    fi
    if [ ! -f "$DAEMON_PLIST" ]; then
        echo "  !! $DAEMON_PLIST が見つかりません — make install を先に実行してください" >&2
        return 1
    fi
    if inmu_as_root launchctl bootstrap system "$DAEMON_PLIST"; then
        echo "  cored 起動しました"
    else
        echo "  !! cored 起動に失敗 — 'sudo make start-daemon' で実行してください" >&2
        return 1
    fi
}

do_restart() {
    if is_loaded; then
        if inmu_as_root launchctl kickstart -k "system/$DAEMON_ID"; then
            echo "  cored 再起動しました (kickstart)"
        else
            echo "  !! cored 再起動に失敗" >&2
            return 1
        fi
    else
        do_start
    fi
}

do_status() {
    if is_loaded; then
        echo "  cored: 起動中"
    else
        echo "  cored: 停止中"
    fi
}

case "${1:-}" in
    stop)    do_stop ;;
    start)   do_start ;;
    restart) do_restart ;;
    status)  do_status ;;
    *)       echo "usage: $0 {stop|start|restart|status}" >&2; exit 2 ;;
esac
