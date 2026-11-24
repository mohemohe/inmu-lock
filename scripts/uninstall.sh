#!/bin/bash
# inmu-lock をシステムから完全に除去する (make uninstall の実体)。
#
#   - launchd からの解除と plist 削除
#   - バイナリ・DLL・テンプレート・ログの削除
#   - /etc/pam.d/sudo_local から当該行のみ除去 (空ならファイル削除)
#   - Keychain のパスワードは手動削除の案内のみ (inmu-agent --unset-password)

set -euo pipefail
cd "$(dirname "$0")/.."

BINDIR=/usr/local/bin
PAMDIR=/usr/local/lib/pam
LIBEXECDIR=/usr/local/libexec/inmu-lock
ETCDIR=/usr/local/etc/inmu-lock
LOGDIR=/var/log/inmu-lock
RUNDIR=/var/run/inmu
SUDO_LOCAL="/etc/pam.d/sudo_local"
BEGIN="# BEGIN inmu-lock (pam_inmu)"
END="# END inmu-lock (pam_inmu)"
DAEMON_ID=com.mohemohe.inmu-lock.cored
AGENT_ID=com.mohemohe.inmu-lock.agent
DAEMON_PLIST=/Library/LaunchDaemons/com.mohemohe.inmu-lock.cored.plist
AGENT_PLIST="$HOME/Library/LaunchAgents/com.mohemohe.inmu-lock.agent.plist"

if [ "$EUID" -ne 0 ]; then
    # sudo が必要な操作のみ sudo を使う (agent はユーザー操作)
    :
fi

echo "==> 1/3 launchd から解除"
sudo launchctl bootout system/"$DAEMON_ID" 2>/dev/null || echo "    (cored は未登録)"
sudo rm -f "$DAEMON_PLIST"
launchctl bootout gui/$(id -u)/"$AGENT_ID" 2>/dev/null || echo "    (agent は未登録)"
rm -f "$AGENT_PLIST"

echo "==> 2/3 sudo_local から除去"
if [ -f "$SUDO_LOCAL" ]; then
    sudo cp -p "$SUDO_LOCAL" "$SUDO_LOCAL.bak.$(date +%Y%m%d%H%M%S)"
    TMP="$(mktemp)"
    sudo awk -v b="$BEGIN" -v e="$END" '
        $0 == b { skip = 1 }
        !skip   { print }
        $0 == e { skip = 0 }
    ' "$SUDO_LOCAL" > "$TMP"
    sudo install -m 0644 "$TMP" "$SUDO_LOCAL"
    rm -f "$TMP"
    if [ -z "$(sudo grep -E '^[[:space:]]*[^[:space:]#]' "$SUDO_LOCAL" || true)" ]; then
        sudo rm -f "$SUDO_LOCAL"
        echo "    実質空のため $SUDO_LOCAL を削除しました"
    else
        echo "    当該行を除去しました (他の行は保持)"
    fi
else
    echo "    $SUDO_LOCAL は存在しません"
fi

echo "==> 3/3 ファイルを削除"
sudo rm -f "$BINDIR/inmu-cored" "$BINDIR/inmu-agent" "$BINDIR/inmu-enroll"
sudo rm -f "$PAMDIR/pam_inmu.so"
sudo rm -rf "$LIBEXECDIR" "$ETCDIR" "$LOGDIR" "$RUNDIR"

echo ""
echo "=============================== アンインストール完了 ==============================="
echo "動作確認: sudo -k && sudo whoami  (パスワードを聞かれれば完全に戻っています)"
echo ""
echo "Keychain に保存したパスワードを削除する場合:"
echo "  $BINDIR/inmu-agent --unset-password    # または キーチェーンアクセスで"
echo "  「login」→ com.mohemohe.inmu-lock を削除"
