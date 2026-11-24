#!/bin/bash
# inmu-lock をシステムへインストールする (make install の実体)。
#
#   1. バイナリ → /usr/local/bin/{inmu-cored,inmu-agent,inmu-enroll}
#      PAM モジュール → /usr/local/lib/pam/pam_inmu.so
#      vendor DLL → /usr/local/libexec/inmu-lock/
#      登録オーナー → /usr/local/etc/inmu-lock/owner (無い場合のみ作成)
#   2. /etc/pam.d/sudo_local へ組み込み (マーカー付き・バックアップ付き)
#   3. launchd 登録:
#      - LaunchDaemon (root): cored — センサー+matcher+IPC
#      - LaunchAgent (user):  agent — ロック解除注入
#   4. 稼働に必要な残りの手順を表示
#
# 実行方法: 一般ユーザーで `make install` (必要な箇所だけ sudo を使う)、
#           または `sudo make install`。root のまま (sudo 経由でない) の実行は不可。
# アンインストール: make uninstall
# 冪等性: 既に起動中/登録済みでも正しく動作する

set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/common.sh
. scripts/common.sh

BINDIR=/usr/local/bin
PAMDIR=/usr/local/lib/pam
LIBEXECDIR=/usr/local/libexec/inmu-lock
ETCDIR=/usr/local/etc/inmu-lock
LOGDIR=/var/log/inmu-lock
STATEDIR=/var/db/inmu-lock
OWNER_FILE="$ETCDIR/owner"
VENDOR_DLL=vendor/ftWbioEngineAdapter.dll
SUDO_LOCAL="/etc/pam.d/sudo_local"
BEGIN="# BEGIN inmu-lock (pam_inmu)"
END="# END inmu-lock (pam_inmu)"
PAM_LINE="auth       sufficient     $PAMDIR/pam_inmu.so timeout_ms=5000"
DAEMON_PLIST_SRC=launchd/dev.mohemohe.inmu-lock.cored.plist
DAEMON_PLIST_DST=/Library/LaunchDaemons/dev.mohemohe.inmu-lock.cored.plist
AGENT_PLIST_SRC=launchd/dev.mohemohe.inmu-lock.agent.plist
DAEMON_ID=dev.mohemohe.inmu-lock.cored
AGENT_ID=dev.mohemohe.inmu-lock.agent

# 実ユーザー (agent / Keychain / 登録オーナーの対象)。sudo 実行時は SUDO_USER。
REAL_USER=""
REAL_UID=""
REAL_HOME=""
inmu_resolve_real_user || exit 1
AGENT_PLIST_DIR="$REAL_HOME/Library/LaunchAgents"
AGENT_PLIST_DST="$AGENT_PLIST_DIR/dev.mohemohe.inmu-lock.agent.plist"
AGENT_LOG_DIR="$REAL_HOME/Library/Logs/inmu-lock"

# 中断時に残さない一時ファイル
DLL_STAGE=""     # root 所有 ($LIBEXECDIR 内。vendor DLL と inmu-agent の検査用)
OWNER_TMP=""     # root 所有 ($ETCDIR 内)
AGENT_TMP=""     # 実ユーザー所有 ($AGENT_PLIST_DIR 内)
cleanup() {
    if [ -n "$DLL_STAGE" ]; then sudo rm -rf "$DLL_STAGE" || true; fi
    if [ -n "$OWNER_TMP" ]; then sudo rm -f "$OWNER_TMP" || true; fi
    if [ -n "$AGENT_TMP" ]; then inmu_as_user rm -f "$AGENT_TMP" || true; fi
}
trap cleanup EXIT

fail() {
    echo "!! $*" >&2
    exit 1
}

# root 所有・group/other 書き込み不可の実ディレクトリ (シンボリックリンク不可) か。
# ここを満たさない場所へ置いたバイナリや PAM モジュールは、root 以外が差し替え
# られる (例: Homebrew が /usr/local 配下を所有する Intel Mac)。
is_root_only_dir() {
    local info uid mode type
    info="$(stat -f '%u %Lp %HT' "$1" 2>/dev/null)" || return 1
    uid="${info%% *}"
    info="${info#* }"
    mode="${info%% *}"
    type="${info#* }"
    [ "$type" = "Directory" ] || return 1
    [ "$uid" = "0" ] || return 1
    [[ "$mode" =~ ^[0-7]{3,4}$ ]] || return 1
    [ $(( 8#$mode & 8#022 )) -eq 0 ]
}

# inmu-agent の当該アーキテクチャのスライスが Hardened Runtime で署名されているか。
# 3 番目以降の引数は codesign の前に置く (root 専用の場所にあるファイルは sudo で読む)。
has_hardened_runtime() { # <file> <arch> [sudo]
    local out file="$1" arch="$2"
    shift 2
    out="$("$@" codesign -d -v -a "$arch" "$file" 2>&1)" || return 1
    [[ "$out" =~ flags=0x[0-9a-f]+\([a-z,-]*runtime[a-z,-]*\) ]]
}

echo "==> 1/5 バイナリ・DLL を配置"

# --- 事前検証 (システムに触れる前に行う) ---
for f in build/inmu-cored build/inmu-agent build/inmu-enroll build/pam_inmu.so; do
    [ -f "$f" ] || fail "$f がありません — make を先に実行してください"
done
# agent は Keychain のパスワードとアクセシビリティ権限を持つ。Hardened Runtime が
# 無いと DYLD_INSERT_LIBRARIES で任意コードを注入できるため、無ければ入れない。
for arch in arm64 x86_64; do
    has_hardened_runtime build/inmu-agent "$arch" || \
        fail "build/inmu-agent ($arch) が Hardened Runtime で署名されていません — make clean && make でビルドし直してください"
done
inmu_verify_sha256 "$VENDOR_DLL" "$INMU_DLL_SHA256" || \
    fail "$VENDOR_DLL の SHA-256 が既知の値と一致しません — make vendor を実行してください"

# --- インストール先ディレクトリ (親から順に: 無ければ root で作成 → 検証) ---
for d in /usr/local /usr/local/bin /usr/local/lib "$PAMDIR" \
         /usr/local/libexec "$LIBEXECDIR" /usr/local/etc "$ETCDIR"; do
    if [ ! -e "$d" ] && [ ! -L "$d" ]; then
        sudo install -d -o root -g wheel -m 0755 "$d"
    fi
    if ! is_root_only_dir "$d"; then
        echo "!! $d が root 所有かつ group/other 書き込み不可のディレクトリではありません:" >&2
        echo "     $(stat -f '%Su:%Sg %Sp' "$d" 2>/dev/null || echo '(stat 失敗)')" >&2
        echo "   このままでは root 以外のユーザーが PAM モジュールやデーモンを差し替えられるため中止します。" >&2
        echo "   (例: sudo chown root:wheel '$d' && sudo chmod 0755 '$d' — Homebrew 等が使用中の場合は影響を確認のこと)" >&2
        exit 1
    fi
done

# inmu-agent と vendor DLL: ユーザーが書き込める build/ や vendor/ から直接入れず、
# root 専用の場所へコピーしたものを検査してから配置する (検査とコピーの間の
# 差し替えを防ぐ。検査に落ちたものはインストール先に置かない)。
DLL_STAGE="$(sudo mktemp -d "$LIBEXECDIR/.stage.XXXXXX")"
sudo install -o root -g wheel -m 0755 build/inmu-agent "$DLL_STAGE/inmu-agent"
for arch in arm64 x86_64; do
    has_hardened_runtime "$DLL_STAGE/inmu-agent" "$arch" sudo || \
        fail "build/inmu-agent ($arch) が Hardened Runtime で署名されていません (インストールしません) — make clean && make でビルドし直してください"
done
sudo install -o root -g wheel -m 0644 "$VENDOR_DLL" "$DLL_STAGE/ftWbioEngineAdapter.dll"
STAGED_SHA="$(sudo /usr/bin/shasum -a 256 "$DLL_STAGE/ftWbioEngineAdapter.dll" | awk '{print $1}')"
if [ "$STAGED_SHA" != "$INMU_DLL_SHA256" ]; then
    fail "$VENDOR_DLL の SHA-256 が既知の値と一致しません (インストールしません) — make vendor を実行してください"
fi

# 検査を通ってから配置する (どれかに落ちた場合は何も入れ替えない)
sudo install -o root -g wheel -m 0755 build/inmu-cored build/inmu-enroll "$BINDIR/"
sudo install -o root -g wheel -m 0755 build/pam_inmu.so "$PAMDIR/"
sudo mv -f "$DLL_STAGE/inmu-agent" "$BINDIR/inmu-agent"
sudo mv -f "$DLL_STAGE/ftWbioEngineAdapter.dll" "$LIBEXECDIR/ftWbioEngineAdapter.dll"
sudo rmdir "$DLL_STAGE"
DLL_STAGE=""

# ログ: root 以外から読めないようにする (閲覧は make logs = sudo tail)
sudo install -d -o root -g wheel -m 0750 "$LOGDIR"
sudo chown root:wheel "$LOGDIR"
sudo chmod 0750 "$LOGDIR"
sudo find "$LOGDIR" -maxdepth 1 -type f -exec chown root:wheel {} + -exec chmod 0600 {} +

# 永続状態 (連続失敗カウンタ等): root のみ
sudo install -d -o root -g wheel -m 0700 "$STATEDIR"
sudo chown root:wheel "$STATEDIR"
sudo chmod 0700 "$STATEDIR"

# 登録オーナー (指紋の持ち主の uid)。既にあれば触らない。
if [ -e "$OWNER_FILE" ] || [ -L "$OWNER_FILE" ]; then
    CUR_OWNER="$(head -c 32 "$OWNER_FILE" 2>/dev/null | head -n 1 || true)"
    if [[ "$CUR_OWNER" =~ ^[0-9]+$ ]] && [ "$CUR_OWNER" = "$REAL_UID" ]; then
        echo "    登録オーナー: uid=$REAL_UID ($REAL_USER) — 既存 (変更なし)"
    elif ! [[ "$CUR_OWNER" =~ ^[0-9]+$ ]]; then
        echo "    !! 既存の登録オーナー ($OWNER_FILE) の内容が不正です (変更しません)"
        echo "       このままではデーモンは一切承認しません (指紋は誰にも効きません)。登録し直してください:"
        echo "         sudo make stop-daemon && sudo make enroll && sudo make start-daemon"
    else
        echo "    !! 既存の登録オーナー ($OWNER_FILE) は現在のユーザー (uid=$REAL_UID) と異なります (変更しません)"
        echo "       指紋認証はそのファイルに記録された uid のユーザーにのみ有効です"
    fi
else
    OWNER_TMP="$(sudo mktemp "$ETCDIR/.owner.XXXXXX")"
    printf '%s\n' "$REAL_UID" | sudo tee "$OWNER_TMP" >/dev/null
    sudo chown root:wheel "$OWNER_TMP"
    sudo chmod 0644 "$OWNER_TMP"
    sudo mv -f "$OWNER_TMP" "$OWNER_FILE"
    OWNER_TMP=""
    echo "    登録オーナー: uid=$REAL_UID ($REAL_USER) を $OWNER_FILE に記録"
fi

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

echo "==> 3/5 launchd に登録 (daemon: system / agent: gui/$REAL_UID)"

# cored (LaunchDaemon)
sudo install -o root -g wheel -m 0644 "$DAEMON_PLIST_SRC" "$DAEMON_PLIST_DST"
# 冪等: 既に起動中なら停止してから再登録
sudo launchctl bootout system/"$DAEMON_ID" 2>/dev/null || true
sudo launchctl bootstrap system "$DAEMON_PLIST_DST" || true

# agent (LaunchAgent) — ホーム配下のファイルはすべて実ユーザーとして作成する
# (sudo 実行時に root 所有のファイルをホームへ残さない)。
inmu_as_user mkdir -p "$AGENT_LOG_DIR" "$AGENT_PLIST_DIR"
# 一時ファイルは配置先と同じ (実ユーザー所有の) ディレクトリに mktemp で作り、
# ログのパスは sed ではなく plutil で設定する (パス中の特殊文字で壊れない)。
AGENT_TMP="$(inmu_as_user mktemp "$AGENT_PLIST_DIR/.inmu-agent.XXXXXX")"
inmu_as_user cp "$AGENT_PLIST_SRC" "$AGENT_TMP"
inmu_as_user plutil -replace StandardOutPath -string "$AGENT_LOG_DIR/agent.log" "$AGENT_TMP"
inmu_as_user plutil -replace StandardErrorPath -string "$AGENT_LOG_DIR/agent.err.log" "$AGENT_TMP"
inmu_as_user plutil -lint "$AGENT_TMP" >/dev/null
inmu_as_user chmod 0644 "$AGENT_TMP"
inmu_as_user mv -f "$AGENT_TMP" "$AGENT_PLIST_DST"
AGENT_TMP=""
# 冪等: 既に起動中なら停止してから再登録
inmu_as_user launchctl bootout gui/"$REAL_UID"/"$AGENT_ID" 2>/dev/null || true
inmu_as_user launchctl bootstrap gui/"$REAL_UID" "$AGENT_PLIST_DST" || true

echo "==> 4/5 状態確認"
sleep 1
sudo launchctl print system/"$DAEMON_ID" 2>/dev/null | grep -E "state|pid" | head -2 || \
    echo "    (cored: テンプレート未登録などで起動待ちの場合あり)"
inmu_as_user launchctl print gui/"$REAL_UID"/"$AGENT_ID" 2>/dev/null | grep -E "state|pid" | head -2 || true

echo ""
echo "================================ インストール完了 ================================"
echo ""
echo "稼働に必要な残りの手順:"
echo ""
if [ ! -f "$ETCDIR/template.bin" ]; then
    echo "  1. 指紋を登録 (対話的に指を置いてください):"
    echo "       sudo make stop-daemon && sudo make enroll && sudo make start-daemon"
else
    echo "  1. 指紋テンプレート: $ETCDIR/template.bin ✅"
    echo "     (登録し直す場合: sudo make stop-daemon && sudo make enroll && sudo make start-daemon)"
fi
echo ""
echo "  2. 画面ロック解除を使う場合のみ:"
echo "     a. ログインパスワードを保存:   $BINDIR/inmu-agent --set-password"
echo "     b. アクセシビリティ権限を与える:"
echo "        システム設定 → プライバシーとセキュリティ → アクセシビリティ"
echo "        → 「+」→ $BINDIR/inmu-agent を追加"
echo "     c. agent を再起動:  make restart-agent"
echo ""
echo "     !! 再インストール / 更新した場合もこの a〜c をやり直してください。"
echo "        inmu-agent は Hardened Runtime 付きで署名し直されており、署名が変わると"
echo "        以前のアクセシビリティ許可と Keychain 項目のアクセス許可は無効になります:"
echo "          - アクセシビリティ: 一覧の既存の inmu-agent を「−」で削除してから追加し直す"
echo "          - Keychain:        $BINDIR/inmu-agent --set-password を再実行する"
echo ""
echo "  sudo 指紋認証:   sudo -k && sudo whoami  → センサーに触れる"
echo "  画面ロック解除:  ⌃⌘Q → センサーに触れる"
echo "  状態/ログ:      make status / make logs"
echo ""
echo "デーモン操作:"
echo "  cored:  sudo make stop-daemon / start-daemon / restart-daemon"
echo "  agent:  make stop-agent / start-agent / restart-agent"
echo "解除: make uninstall"
