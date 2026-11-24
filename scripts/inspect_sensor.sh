#!/bin/bash
# FT9201 (VID 2808 / PID 93a9) 接続時のUSB構成を調査するスクリプト
#
# 使い方:
#   ./scripts/inspect_sensor.sh          (センサーをUSBに挿した状態で実行)
#   sudo ./scripts/inspect_sensor.sh     (より詳細な情報)
#
# 何がわかるか (docs/03-ft9201-notes.md の調査TODOを埋めるためのデータ):
#   - デバイスが認識されているか (VID/PID確認)
#   - インターフェース数・クラス (bulk? HID?) — IOKit実装方針の確定
#   - エンドポイント構成 (bulk in/out のアドレス) — トランスポート層の実装パラメータ
#   - 現在のドライバのアタッチ状況 (Apple純正に握られていないか)

set -uo pipefail

VID_HEX="2808"
PID_HEX="93a9"
GREEN=$'\033[32m'
YELLOW=$'\033[33m'
RED=$'\033[31m'
RESET=$'\033[0m'

echo "${GREEN}== 1. system_profiler: USBデバイス認識状況 ==${RESET}"
system_profiler SPUSBDataType 2>/dev/null | grep -i -B3 -A14 'focal\|ft9201\|fingerprint' || {
    echo "${RED}   センサーが見つかりません。USB挿入を確認してください。${RESET}"
}

echo ""
echo "${GREEN}== 2. ioreg: USBインターフェース・エンドポイント構成 ==${RESET}"
ioreg -p IOUSB -w 0 -l 2>/dev/null | grep -i -B4 -A40 'FT9201\|Focal' || \
    echo "${YELLOW}   (root権限なし/デバイス未接続では表示されないことがあります)${RESET}"

echo ""
echo "${GREEN}== 3. ioreg: IOUSBHostInterface (ユーザースペースから開けるか) ==${RESET}"
ioreg -r -c IOUSBHostInterface -w 0 -l 2>/dev/null | grep -i -B2 -A6 'FT9201\|Focal\|2808' || \
    echo "${YELLOW}   該当なし${RESET}"

echo ""
echo "${GREEN}== 4. ioreg: HIDデバイスとして登録されていないか (HIDならIOHIDDeviceに現れる) ==${RESET}"
ioreg -r -c IOHIDDevice -w 0 -l 2>/dev/null | grep -i -B2 -A6 'FT9201\|Focal\|2808' || \
    echo "   該当なし (HID未登録 — Linux同様にベンダーbulkインターフェースの可能性が高い)"

echo ""
echo "${GREEN}== 5. カーネルドライバのアタッチ状況 ==${RESET}"
ioreg -p IOUSB -w 0 2>/dev/null | grep -i -B2 -A2 'FT9201\|Focal' || true

echo ""
echo "次のステップ (docs/03-ft9201-notes.md 参照):"
echo "  * エンドポイント構成を docs/03 に記録する"
echo "  * Apple純正ドライバがアタッチしている場合は interface claim の要否を検討"
echo "  * Linux版ドライバ (banianitc/ft9201-fingerprint-driver, OMGrant/ft9201-libfprint) の"
echo "    プロトコル定数と照合する"
