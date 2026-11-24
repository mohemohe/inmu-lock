#!/bin/bash
# inmu-lock をシステムから除去する (make uninstall の実体)。
#
#   - launchd からの解除と plist 削除 (agent は実行したユーザーの分のみ)
#   - /etc/pam.d/sudo_local から当該行のみ除去 (空ならファイル削除)
#   - Keychain に保存したログインパスワードの削除 (inmu-agent --unset-password。
#     実行したユーザーの分のみ。できなければ手動削除を案内する)
#   - バイナリ・DLL・テンプレート・登録オーナー・ロックアウト状態・ログの削除
#     (PAM モジュールは /etc/pam.d に pam_inmu.so を参照する行が残っている間は消さない。
#      参照だけ残してモジュールを消すと sudo が認証前に失敗しうるため)
#
# 残るもの (末尾で案内する): sudo_local のバックアップ、アクセシビリティの許可、
# 実行したユーザー以外の LaunchAgent / Keychain 項目 / agent のログ

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
RUNDIR=/var/run/inmu
SUDO_LOCAL="/etc/pam.d/sudo_local"
BEGIN="# BEGIN inmu-lock (pam_inmu)"
END="# END inmu-lock (pam_inmu)"
DAEMON_ID=dev.mohemohe.inmu-lock.cored
AGENT_ID=dev.mohemohe.inmu-lock.agent
DAEMON_PLIST=/Library/LaunchDaemons/dev.mohemohe.inmu-lock.cored.plist

# 実ユーザー (agent の持ち主)。sudo 実行時は SUDO_USER。
# sudo が必要な操作のみ sudo を使い、agent とホーム配下は実ユーザーとして操作する。
REAL_USER=""
REAL_UID=""
REAL_HOME=""
inmu_resolve_real_user || exit 1
AGENT_PLIST="$REAL_HOME/Library/LaunchAgents/dev.mohemohe.inmu-lock.agent.plist"
AGENT_LOG_DIR="$REAL_HOME/Library/Logs/inmu-lock"

# sudo_local の BEGIN〜END ブロックが「install.sh が書いたままの形」か。
# BEGIN / END が行全体の完全一致で 1 組以上あり、正しく対になっている場合だけ真。
# (END の無い BEGIN を消しにいくと、以降の他モジュールの行まで落としてしまう)
sudo_local_block_ok() {
    sudo awk -v b="$BEGIN" -v e="$END" '
        $0 == b { if (inblk) bad = 1; inblk = 1; n++ }
        $0 == e { if (!inblk) bad = 1; inblk = 0 }
        END     { exit (n >= 1 && !inblk && !bad) ? 0 : 1 }
    ' "$SUDO_LOCAL"
}

# PAM 設定ファイルに、コメント以外で pam_inmu.so を参照する行があるか。
# 0 = ある / 1 = 無い / それ以外 = 読めなかった (呼び出し側は「ある」として扱う)。
pam_file_refs_module() {
    sudo awk '
        !/^[[:space:]]*#/ && /pam_inmu\.so/ { found = 1 }
        END { exit found ? 0 : 1 }
    ' "$1"
}

# 中断時に残さない一時ファイル (root 所有・/etc/pam.d 内)
SUDO_LOCAL_TMP=""
cleanup() {
    if [ -n "$SUDO_LOCAL_TMP" ]; then sudo rm -f "$SUDO_LOCAL_TMP" || true; fi
}
trap cleanup EXIT

echo "==> 1/4 launchd から解除"
sudo launchctl bootout system/"$DAEMON_ID" 2>/dev/null || echo "    (cored は未登録)"
sudo rm -f "$DAEMON_PLIST"
inmu_as_user launchctl bootout gui/"$REAL_UID"/"$AGENT_ID" 2>/dev/null || echo "    (agent は未登録)"
inmu_as_user rm -f "$AGENT_PLIST"

echo "==> 2/4 sudo_local から除去"
if [ ! -f "$SUDO_LOCAL" ]; then
    echo "    $SUDO_LOCAL は存在しません"
elif ! sudo grep -qF "$BEGIN" "$SUDO_LOCAL"; then
    # 組み込まれていなければ何も書き換えない (バックアップも作らない)。
    # 判定は install.sh と同じ (マーカーの部分一致)。
    echo "    $SUDO_LOCAL に inmu-lock のマーカーはありません (変更なし)"
elif ! sudo_local_block_ok; then
    # マーカーが手で編集されている (行末の空白・CRLF・END が無い等): 自動では書き換えない。
    # pam_inmu.so の行が残っていれば、4/4 でモジュールを残して手動削除を案内する。
    echo "    !! $SUDO_LOCAL のマーカーが install.sh の書いた形と異なります (変更しません)"
else
    BACKUP="$SUDO_LOCAL.bak.$(date +%Y%m%d%H%M%S)"
    echo "    既存ファイルをバックアップ: $BACKUP"
    sudo cp -p "$SUDO_LOCAL" "$BACKUP"
    # 書き戻す内容はユーザー所有の一時ファイルを経由させない: root 所有の
    # 一時ファイルを同じディレクトリに作り、書き終えてから mv で置き換える。
    SUDO_LOCAL_TMP="$(sudo mktemp "$SUDO_LOCAL.inmu.XXXXXX")"
    sudo awk -v b="$BEGIN" -v e="$END" '
        $0 == b { skip = 1 }
        !skip   { print }
        $0 == e { skip = 0 }
    ' "$SUDO_LOCAL" | sudo tee "$SUDO_LOCAL_TMP" >/dev/null
    sudo chown root:wheel "$SUDO_LOCAL_TMP"
    sudo chmod 0644 "$SUDO_LOCAL_TMP"
    sudo mv -f "$SUDO_LOCAL_TMP" "$SUDO_LOCAL"
    SUDO_LOCAL_TMP=""
    if [ -z "$(sudo grep -E '^[[:space:]]*[^[:space:]#]' "$SUDO_LOCAL" || true)" ]; then
        sudo rm -f "$SUDO_LOCAL"
        echo "    実質空のため $SUDO_LOCAL を削除しました"
    else
        echo "    当該行を除去しました (他の行は保持)"
    fi
fi

echo "==> 3/4 Keychain のログインパスワードを削除"
# 削除手段 (inmu-agent --unset-password) はこの後バイナリごと消すので、先に実行する。
# Keychain は実ユーザーのものなので、実ユーザー権限で実行する。
KEYCHAIN_CLEARED=0
if [ -x "$BINDIR/inmu-agent" ]; then
    if inmu_as_user "$BINDIR/inmu-agent" --unset-password; then
        KEYCHAIN_CLEARED=1
    else
        echo "    (保存していなかった場合は問題ありません。末尾の案内で確認してください)"
    fi
else
    echo "    $BINDIR/inmu-agent がありません (末尾の案内で確認してください)"
fi

echo "==> 4/4 ファイルを削除"
sudo rm -f "$BINDIR/inmu-cored" "$BINDIR/inmu-agent" "$BINDIR/inmu-enroll"
# PAM モジュールは、/etc/pam.d に参照が残っていないことを確かめてから消す。
# 参照が残ったままモジュールだけ消すと、sudo が認証の前に失敗しうる (パスワードへも
# 落ちない)。モジュールが残っていれば、デーモン不在でも PAM_IGNORE でパスワードへ落ちる。
# 確認できなかったファイルは「参照あり」として扱う。自分のバックアップは対象外。
PAM_REFS=()
for f in /etc/pam.d/*; do
    [ -f "$f" ] || continue
    case "$f" in "$SUDO_LOCAL".bak.*) continue ;; esac
    rc=0
    pam_file_refs_module "$f" || rc=$?
    if [ "$rc" -ne 1 ]; then PAM_REFS+=("$f"); fi
done
if [ "${#PAM_REFS[@]}" -eq 0 ]; then
    sudo rm -f "$PAMDIR/pam_inmu.so"
else
    echo "    !! pam_inmu.so を参照する PAM 設定が残っているため $PAMDIR/pam_inmu.so は削除しません"
fi
# $ETCDIR にはテンプレートと登録オーナー (owner)、$STATEDIR にはロックアウト状態がある
sudo rm -rf "$LIBEXECDIR" "$ETCDIR" "$LOGDIR" "$STATEDIR" "$RUNDIR"
# agent のログ (install.sh が実ユーザーとして作ったもの)
inmu_as_user rm -rf "$AGENT_LOG_DIR"

echo ""
if [ "${#PAM_REFS[@]}" -eq 0 ]; then
    echo "=============================== アンインストール完了 ==============================="
    echo "動作確認: sudo -k && sudo whoami  (パスワードを聞かれれば sudo は元に戻っています)"
else
    echo "========================= アンインストール未完了 (要手動対応) ========================="
    echo "!! 次のファイルに pam_inmu.so を参照する行が残っています (または内容を確認できませんでした):"
    for f in "${PAM_REFS[@]}"; do
        echo "     $f"
        sudo grep -nF 'pam_inmu.so' "$f" 2>/dev/null | sed 's/^/       /' || true
    done
    echo "   sudo を壊さないよう $PAMDIR/pam_inmu.so は残してあります (デーモンが無いので"
    echo "   指紋は使われず、sudo は従来どおりパスワード入力になります)。"
    echo "   上の行 (と inmu-lock のマーカー行) を root 権限のエディタで削除してから、"
    echo "   もう一度 make uninstall を実行してください。"
fi
echo ""
if [ "$KEYCHAIN_CLEARED" = "1" ]; then
    echo "Keychain: $REAL_USER のログインパスワード (dev.mohemohe.inmu-lock) を削除しました"
else
    echo "!! Keychain: $REAL_USER のログインパスワードを削除できたか確認できていません。"
    echo "   保存していた場合は手動で削除してください (inmu-agent は削除済み):"
    echo "     キーチェーンアクセス →「ログイン」→ dev.mohemohe.inmu-lock を削除"
    echo "     または: security delete-generic-password -s dev.mohemohe.inmu-lock -a login-password"
fi
echo ""
echo "このスクリプトが消さないもの (必要なら手動で削除):"
echo "  - アクセシビリティの許可: システム設定 → プライバシーとセキュリティ →"
echo "    アクセシビリティ → inmu-agent を「−」で削除"
echo "  - $SUDO_LOCAL.bak.<日時> (install / uninstall が作ったバックアップ)"
echo "  - $REAL_USER 以外のユーザーで使っていた場合、そのユーザーの"
echo "    ~/Library/LaunchAgents/$AGENT_ID.plist、~/Library/Logs/inmu-lock、"
echo "    Keychain の dev.mohemohe.inmu-lock (そのユーザーで make uninstall を先に実行するか手動で削除)"

# PAM の参照が残っている場合は失敗として終了する (モジュールは残してある)
if [ "${#PAM_REFS[@]}" -ne 0 ]; then exit 1; fi
