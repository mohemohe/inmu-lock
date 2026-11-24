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

AGENT_ID="com.mohemohe.inmu-lock.agent"
AGENT_PLIST="$HOME/Library/LaunchAgents/com.mohemohe.inmu-lock.agent.plist"

# 実際のユーザーを特定 (sudo 経由の場合は SUDO_USER、そうでなければ自分)
if [ -n "${SUDO_USER:-}" ] && [ "$(id -u)" = "0" ]; then
    REAL_USER="$SUDO_USER"
    REAL_UID=$(id -u "$SUDO_USER")
    REAL_HOME=$(eval echo "~$SUDO_USER")
    AGENT_PLIST="$REAL_HOME/Library/LaunchAgents/com.mohemohe.inmu-lock.agent.plist"
    AS_USER="sudo -u $SUDO_USER"
elif [ "$(id -u)" = "0" ]; then
    echo "  !! agent の操作にはユーザー権限が必要です (sudo make start-agent でなく" \
         "make start-agent を実行、または SUDO_USER が設定された環境で実行してください)" >&2
    exit 1
else
    REAL_USER="$(whoami)"
    REAL_UID="$(id -u)"
    AS_USER=""
fi

is_loaded() {
    if [ -n "$AS_USER" ]; then
        $AS_USER launchctl print "gui/$REAL_UID/$AGENT_ID" >/dev/null 2>&1
    else
        launchctl print "gui/$REAL_UID/$AGENT_ID" >/dev/null 2>&1
    fi
}

do_stop() {
    if is_loaded; then
        if [ -n "$AS_USER" ]; then
            $AS_USER launchctl bootout "gui/$REAL_UID/$AGENT_ID"
        else
            launchctl bootout "gui/$REAL_UID/$AGENT_ID"
        fi
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
    if [ -n "$AS_USER" ]; then
        $AS_USER launchctl bootstrap "gui/$REAL_UID" "$AGENT_PLIST"
    else
        launchctl bootstrap "gui/$REAL_UID" "$AGENT_PLIST"
    fi
    echo "  agent 起動しました"
}

do_restart() {
    if is_loaded; then
        if [ -n "$AS_USER" ]; then
            $AS_USER launchctl kickstart -k "gui/$REAL_UID/$AGENT_ID"
        else
            launchctl kickstart -k "gui/$REAL_UID/$AGENT_ID"
        fi
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
