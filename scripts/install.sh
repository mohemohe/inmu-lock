#!/bin/bash
# inmu-lock をシステムへインストールする (make install の実体)。
#
#   1. バイナリ → /usr/local/bin/{inmu-cored,inmu-agent,inmu-enroll}
#      PAM モジュール → /usr/local/lib/pam/pam_inmu.so
#      vendor DLL → /usr/local/libexec/inmu-lock/
#   2. /etc/pam.d/sudo_local へ組み込み (マーカー付き・バックアップ付き)
#   3. launchd 登録:
#      - LaunchDaemon (root): cored — センサー+matcher+IPC
#      - LaunchAgent (user):  agent — ロック解除注入
#   4. 稼働に必要な残りの手順を表示 (enroll / パスワード / アクセシビリティ)
#
# アンインストール: make uninstall

set -euo pipefail
cd "$(dirname "$0")/.."

BINDIR=/usr/local/bin
PAMDIR=/usr/local/lib/pam
LIBEXECDIR=/usr/local/libexec/inmu-lock
ETCDIR=/usr/local/etc/inmu-lock
LOGDIR=/var/log/inmu-lock
SUDO_LOCAL="/etc/pam.d/sudo_local"
BEGIN="# BEGIN inmu-lock (pam_inmu)"
END="# END inmu-lock (pam_inmu)"
PAM_LINE="auth       sufficient     $PAMDIR/pam_inmu.so timeout_ms=3000"
DAEMON_PLIST_SRC=launchd/com.mohemohe.inmu-lock.cored.plist
DAEMON_PLIST_DST=/Library/LaunchDaemons/com.mohemohe.inmu-lock.cored.plist
AGENT_PLIST_SRC=launchd/com.mohemohe.inmu-lock.agent.plist
AGENT_PLIST_DST="$HOME/Library/LaunchAgents/com.mohemohe.inmu-lock.agent.plist"
DAEMON_ID=com.mohemohe.inmu-lock.cored
AGENT_ID=com.mohemohe.inmu-lock.agent

echo "==> 1/5 バイナリ・DLL を配置"
sudo mkdir -p "$BINDIR" "$PAMDIR" "$LIBEXECDIR" "$ETCDIR" "$LOGDIR"
sudo install -m 0755 build/inmu-cored build/inmu-agent build/inmu-enroll "$BINDIR/"
sudo install -m 0755 build/pam_inmu.so "$PAMDIR/"
sudo install -m 0644 vendor/ftWbioEngineAdapter.dll "$LIBEXECDIR/"

echo "==> 2/5 $SUDO_LOCAL へ PAM を組み込み"
if [ -f "$SUDO_LOCAL" ]; then
    if grep -qF "$BEGIN" "$SUDO_LOCAL"; then
        echo "    すでに組み込み済みです (スキップ)"
    else
        BACKUP="$SUDO_LOCAL.bak.$(date +%Y%m%d%H%M%S)"
        echo "    既存ファイルをバックアップ: $BACKUP"
        sudo cp -p "$SUDO_LOCAL" "$BACKUP"
        printf '\n%s\n%s\n%s\n' "$BEGIN" "$PAM_LINE" "$END" | sudo tee -a "$SUDO_LOCAL" >/dev/null
    fi
else
    printf '# sudo_local: local config file which survives system update and is included for sudo\n' | sudo tee "$SUDO_LOCAL" >/dev/null
    printf '%s\n%s\n%s\n' "$BEGIN" "$PAM_LINE" "$END" | sudo tee -a "$SUDO_LOCAL" >/dev/null
fi
sudo chmod 0644 "$SUDO_LOCAL"
echo "    追加行: $PAM_LINE"

echo "==> 3/5 launchd に登録 (daemon: root / agent: user)"
# cored (LaunchDaemon)
sudo install -m 0644 "$DAEMON_PLIST_SRC" "$DAEMON_PLIST_DST"
sudo launchctl bootout system/"$DAEMON_ID" 2>/dev/null || true
sudo launchctl bootstrap system "$DAEMON_PLIST_DST"

# agent (LaunchAgent) — ログパスのPLACEHOLDERをユーザーHOMEに置換
mkdir -p "$HOME/Library/Logs/inmu-lock"
sed "s|PLACEHOLDER|$HOME|g" "$AGENT_PLIST_SRC" > /tmp/inmu-agent.plist.$$
launchctl bootout gui/$(id -u)/"$AGENT_ID" 2>/dev/null || true
install -m 0644 /tmp/inmu-agent.plist.$$ "$AGENT_PLIST_DST"
rm -f /tmp/inmu-agent.plist.$$
launchctl bootstrap gui/$(id -u) "$AGENT_PLIST_DST"

echo "==> 4/5 状態確認"
sleep 1
sudo launchctl print system/"$DAEMON_ID" 2>/dev/null | grep -E "state|pid" | head -2 || \
    echo "    (cored: テンプレート未登録などで起動待ちの場合あり)"
launchctl print gui/$(id -u)/"$AGENT_ID" 2>/dev/null | grep -E "state|pid" | head -2 || true

echo ""
echo "================================ インストール完了 ================================"
echo ""
echo "稼働に必要な残りの手順:"
echo ""
if [ ! -f "$ETCDIR/template.bin" ]; then
    echo "  1. 指紋を登録 (対話的に指を置いてください):"
    echo "       make enroll"
else
    echo "  1. 指紋テンプレート: $ETCDIR/template.bin ✅"
    echo "     (登録し直す場合: make enroll → sudo launchctl kickstart -k system/$DAEMON_ID)"
fi
echo ""
echo "  2. 画面ロック解除を使う場合のみ:"
echo "     a. ログインパスワードを保存:   $BINDIR/inmu-agent --set-password"
echo "     b. アクセシビリティ権限を与える:"
echo "        システム設定 → プライバシーとセキュリティ → アクセシビリティ"
echo "        → 「+」→ $BINDIR/inmu-agent を追加"
echo "        (launchd から起動されるため、実行バイナリ自体に権限が必要)"
echo "     c. agent を再起動:  launchctl kickstart -k gui/$(id -u)/$AGENT_ID"
echo ""
echo "テスト:"
echo "  sudo 指紋認証:   sudo -k && sudo whoami  → センサーに触れる"
echo "  画面ロック解除:  ⌃⌘Q → センサーに触れる"
echo "  状態/ログ:      make status / make logs"
echo ""
echo "解除: make uninstall"
