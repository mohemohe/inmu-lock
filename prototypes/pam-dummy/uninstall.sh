#!/bin/bash
# 検証A: ダミーPAMモジュールのアンインストール (ロールバック)
#
#   - /etc/pam.d/sudo_local から当プロトタイプの行だけを除去
#   - sudo_local が実質空になったらファイルごと削除
#   - /etc/pam.d/inmutest と /usr/local/lib/pam/pam_inmu_dummy.so を削除
#
# 実行後、sudo -k && sudo whoami でパスワードを聞かれれば正常に戻っています。

set -euo pipefail
cd "$(dirname "$0")"

MODULE="pam_inmu_dummy.so"
MODULE_DST="/usr/local/lib/pam/$MODULE"
SUDO_LOCAL="/etc/pam.d/sudo_local"
TEST_SERVICE="/etc/pam.d/inmutest"
BEGIN="# BEGIN inmu-lock pam-dummy (prototype)"
END="# END inmu-lock pam-dummy (prototype)"

if [ "$EUID" -ne 0 ]; then
    # root以外で実行されたらsudoで自分を呼び直す
    exec sudo "$0" "$@"
fi

echo "==> 1/3 $SUDO_LOCAL から解除"
if [ -f "$SUDO_LOCAL" ]; then
    cp -p "$SUDO_LOCAL" "$SUDO_LOCAL.bak.$(date +%Y%m%d%H%M%S)"
    TMP="$(mktemp)"
    awk -v b="$BEGIN" -v e="$END" '
        $0 == b { skip = 1 }
        !skip   { print }
        $0 == e { skip = 0 }
    ' "$SUDO_LOCAL" > "$TMP"
    install -m 0644 "$TMP" "$SUDO_LOCAL"
    rm -f "$TMP"
    # 実質空 (有効な行がない) なら削除
    if [ -z "$(grep -E '^[[:space:]]*[^[:space:]#]' "$SUDO_LOCAL" || true)" ]; then
        rm -f "$SUDO_LOCAL"
        echo "    実質空のため $SUDO_LOCAL を削除しました"
    else
        echo "    当プロトタイプの行を除去しました (他の行は保持)"
    fi
else
    echo "    $SUDO_LOCAL は存在しません"
fi

echo "==> 2/3 テスト用サービスを削除: $TEST_SERVICE"
rm -f "$TEST_SERVICE"

echo "==> 3/3 モジュールを削除: $MODULE_DST"
rm -f "$MODULE_DST"

echo ""
echo "=============================== アンインストール完了 ==============================="
echo "動作確認 (パスワードを聞かれればOK):"
echo "  sudo -k && sudo whoami"
