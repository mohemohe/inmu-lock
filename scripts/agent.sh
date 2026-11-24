#!/bin/bash
# agent.sh — inmu-agent (LaunchAgent) の冪等なライフサイクル操作。
#
#   ./scripts/agent.sh stop      停止
#   ./scripts/agent.sh start     起動
#   ./scripts/agent.sh restart   再起動
#   ./scripts/agent.sh status    状態表示
#
# agent は GUI セッション (gui/UID) で動作するため、root では実行できない。
# sudo make 経由で呼ばれた場合は実ユーザーに降格して操作する。

set -u

AGENT_ID="dev.mohemohe.inmu-lock.agent"

# shellcheck source=scripts/common.sh
. "$(dirname "$0")/common.sh" || exit 1

# 実際のユーザーを特定 (sudo 経由の場合は SUDO_USER、そうでなければ自分)。
# ユーザー名の検証とホームの取得は inmu_resolve_real_user が行う。
# root のまま (SUDO_USER 無し) の場合は対象の GUI セッションが決まらないため失敗する
# (sudo make start-agent でなく make start-agent を実行すること)。
REAL_USER=""
REAL_UID=""
REAL_HOME=""
inmu_resolve_real_user || exit 1
AGENT_PLIST="$REAL_HOME/Library/LaunchAgents/dev.mohemohe.inmu-lock.agent.plist"

is_loaded() {
    inmu_as_user launchctl print "gui/$REAL_UID/$AGENT_ID" >/dev/null 2>&1
}

do_stop() {
    if is_loaded; then
        inmu_as_user launchctl bootout "gui/$REAL_UID/$AGENT_ID"
        echo "  agent 停止しました"
    else
        echo "  agent は既に停止中です"
    fi
}

do_start() {
    if is_loaded; then
        echo "  agent は既に起動中です"
        return 0
    fi
    if [ ! -f "$AGENT_PLIST" ]; then
        echo "  !! $AGENT_PLIST が見つかりません — make install を先に実行してください" >&2
        return 1
    fi
    inmu_as_user launchctl bootstrap "gui/$REAL_UID" "$AGENT_PLIST"
    echo "  agent 起動しました"
}

do_restart() {
    if is_loaded; then
        inmu_as_user launchctl kickstart -k "gui/$REAL_UID/$AGENT_ID"
        echo "  agent 再起動しました (kickstart)"
    else
        do_start
    fi
}

do_status() {
    if is_loaded; then
        echo "  agent: 起動中 (uid=$REAL_UID)"
    else
        echo "  agent: 停止中 (uid=$REAL_UID)"
    fi
}

case "${1:-}" in
    stop)    do_stop ;;
    start)   do_start ;;
    restart) do_restart ;;
    status)  do_status ;;
    *)       echo "usage: $0 {stop|start|restart|status}" >&2; exit 2 ;;
esac
