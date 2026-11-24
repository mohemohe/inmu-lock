#!/bin/bash
# 検証A: ダミーPAMモジュールのインストール
#
#   1. ビルド
#   2. モジュールを /usr/local/lib/pam/ へコピー
#   3. 単体テスト用PAMサービス /etc/pam.d/inmutest を作成 (sudo設定とは無関係)
#   4. /etc/pam.d/sudo_local へ「sufficient」で組み込み (既存ファイルはバックアップ)
#
# 使い方:
#   ./install.sh                        デフォルト: モジュールは常に PAM_IGNORE を返す
#                                      (= sudo の挙動は無変更。ログだけ増える)
#   ./install.sh --passwordless-demo    sudo_local のモジュール行に demo_success を追加。
#                                      root所有の /tmp/inmu_pam_demo_success がある間だけ
#                                      パスワード無しで sudo が通る (A-3 の成功テスト用)
#                                      再実行すると既存ブロックを新しい行で更新する。
#
# ※sudo を壊さないための設計:
#   - PAM_SUCCESS を返すのは root 所有の /tmp/inmu_pam_demo_success が
#     あるときのみ (sudo 権を持つ人だけが arm できる)

set -euo pipefail
cd "$(dirname "$0")"

PASSWORDLESS_DEMO=0
if [ "${1:-}" = "--passwordless-demo" ]; then
    PASSWORDLESS_DEMO=1
elif [ $# -gt 0 ]; then
    echo "不明なオプション: $1" >&2
    exit 2
fi

MODULE="pam_inmu_dummy.so"
MODULE_DST_DIR="/usr/local/lib/pam"
MODULE_DST="$MODULE_DST_DIR/$MODULE"
SUDO_LOCAL="/etc/pam.d/sudo_local"
TEST_SERVICE="/etc/pam.d/inmutest"
BEGIN="# BEGIN inmu-lock pam-dummy (prototype)"
END="# END inmu-lock pam-dummy (prototype)"
MODULE_ARGS="debug"
if [ "$PASSWORDLESS_DEMO" = 1 ]; then
    MODULE_ARGS="$MODULE_ARGS demo_success"
fi
LINE="auth       sufficient     $MODULE_DST $MODULE_ARGS"

echo "==> 1/4 ビルド"
make

echo "==> 2/4 モジュールを $MODULE_DST へインストール"
sudo mkdir -p "$MODULE_DST_DIR"
sudo install -m 0755 "$MODULE" "$MODULE_DST"

echo "==> 3/4 単体テスト用サービス $TEST_SERVICE を作成"
sudo tee "$TEST_SERVICE" >/dev/null <<EOF
# inmu-lock 検証A: ダミーモジュールの単体テスト用サービス
# 実運用の sudo_local + sudo スタックと同じ形:
#   - モジュールが成功 → sufficient が短絡して pam_authenticate = 0
#   - モジュールが無視(PAM_IGNORE)/失敗 → 次へ → pam_deny.so が確定的に失敗させる
# 注意: auth行1行だけのスタックでモジュールが PAM_IGNORE を返すと、
#       OpenPAM の集計は PAM_SUCCESS になる (成否の区別がつかない) ため pam_deny を置く
# uninstall.sh で削除されます
auth      sufficient  $MODULE_DST debug demo_success
auth      required    pam_deny.so
account   required    pam_permit.so
EOF
sudo chmod 0644 "$TEST_SERVICE"

echo "==> 4/4 $SUDO_LOCAL へ組み込み (モジュール行: $LINE)"
# BEGIN〜END の間 (マーカー行を含む) を除去する
remove_block() {
    awk -v b="$BEGIN" -v e="$END" '
        $0 == b { skip = 1 }
        !skip   { print }
        $0 == e { skip = 0 }
    ' "$1" > "$2"
}

if [ -f "$SUDO_LOCAL" ]; then
    BACKUP="$SUDO_LOCAL.bak.$(date +%Y%m%d%H%M%S)"
    echo "    既存ファイルをバックアップ: $BACKUP"
    sudo cp -p "$SUDO_LOCAL" "$BACKUP"
    TMP="$(mktemp)"
    if grep -qF "$BEGIN" "$SUDO_LOCAL"; then
        echo "    既存の組み込み行を新しい設定で更新します"
        remove_block "$SUDO_LOCAL" "$TMP"
    else
        cp "$SUDO_LOCAL" "$TMP"
    fi
    printf '%s\n%s\n%s\n' "$BEGIN" "$LINE" "$END" >> "$TMP"
    sudo install -m 0644 "$TMP" "$SUDO_LOCAL"
    rm -f "$TMP"
else
    printf '# sudo_local: local config file which survives system update and is included for sudo\n%s\n%s\n%s\n' \
        "$BEGIN" "$LINE" "$END" | sudo tee "$SUDO_LOCAL" >/dev/null
fi
sudo chmod 0644 "$SUDO_LOCAL"

echo ""
echo "================================ インストール完了 ================================"
echo "⚠️  テスト中は別のターミナルで 'sudo -s' (root シェル) を開いたままにしてください。"
echo ""
echo "単体テスト (docs/04 検証A A-2):"
echo "  1. ./pam_harness inmutest                  # exit 1 + PAM_IGNORE (期待値)"
echo "  2. sudo touch /tmp/inmu_pam_demo_success"
echo "  3. ./pam_harness inmutest                  # exit 0 + PAM_SUCCESS (期待値)"
echo "  4. sudo rm /tmp/inmu_pam_demo_success"
echo "  5. ./pam_harness inmutest                  # exit 1 に戻る"
echo ""
if [ "$PASSWORDLESS_DEMO" = 1 ]; then
    echo "⚠️  --passwordless-demo モード (/etc/pam.d/sudo_local の行に demo_success 付き):"
    echo "   root所有の /tmp/inmu_pam_demo_success が存在する間は、"
    echo "   sudo がパスワード無しで通ります。テスト後は必ず:"
    echo "     sudo rm /tmp/inmu_pam_demo_success    # または ./uninstall.sh"
    echo ""
    echo "パスワードレス sudo のテスト (A-3):"
    echo "  sudo touch /tmp/inmu_pam_demo_success"
    echo "  sudo -k && sudo whoami                   # 期待値: パスワード無しで root"
    echo "  sudo rm /tmp/inmu_pam_demo_success"
else
    echo "このモード (デフォルト) ではモジュールは常に PAM_IGNORE を返すため、"
    echo "sudo の挙動は無変更です (ログだけ増える)。"
    echo "パスワードレス sudo のテスト (A-3) には './install.sh --passwordless-demo' を実行。"
fi
echo ""
echo "戻し方: ./uninstall.sh"
