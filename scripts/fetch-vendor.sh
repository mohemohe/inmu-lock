#!/bin/bash
# 依存バイナリ (vendor blobs) を取得する:
#   1. ftWbioEngineAdapter.dll — FocalTech の Windows 用マッチングエンジン
#      (Microsoft Update Catalog 公開の署名済みドライバ .cab から)
#   2. FT9348W MCU ファームウェア — mrrbrilliant/ft9201-static の FocalTech
#      Linux blob (.deb) から抽出 → src/lib/ft9201_fw.h を生成
#
# いずれも FocalTech の所有物のためリポジトリにはコミットしない (.gitignore)。
# 必要なツール: cabextract (無ければ Homebrew で自動インストール), tar (xz), python3, nm
#
# 取得物は root で動くデーモンが実行する (DLL) / センサーへ書き込む (fw) ため、
# すべて既知の SHA-256 (scripts/common.sh) と照合する:
#   - ダウンロードしたアーカイブは、展開する前に照合する
#   - 取り出した DLL / 生成したヘッダは、配置する前に照合する
#   - 既に存在するファイルも実行のたびに照合し、不一致なら削除して失敗する

set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/common.sh
. scripts/common.sh

DLL=vendor/ftWbioEngineAdapter.dll
DEB=vendor/ft9201-static.deb
FW_HDR=src/lib/ft9201_fw.h

CAB_URL="https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2023/05/fd237921-f610-43de-b77c-f1685416480b_710b4d2c8dd2e80043e371b91bebb1721b5163a0.cab"
# 可変の raw/main ではなく、当該ファイルを含むコミットに固定する
DEB_URL="https://github.com/mrrbrilliant/ft9201-static/raw/e9aa44c94f6a97a4112d3f5de79b4d5d8d0d285a/libfprint_2_2_1_90_1%2Btod1_0ubuntu120_04_2_amd64_16c6e64404f8411.deb"
# .deb 内で必要な唯一のファイル (これだけを展開する)
DEB_SO_MEMBER="./usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0"

mkdir -p vendor

# 作業用ディレクトリ (毎回新規・自分のみアクセス可)。ダウンロードと展開はすべて
# この中で行い、照合を通ったファイルだけを vendor/ や src/lib/ へ移す。
WORK="$(mktemp -d vendor/.fetch.XXXXXX)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

fail() {
    echo "!! $*" >&2
    exit 1
}

download() { # <url> <出力先>
    curl -fsSL --proto '=https' --proto-redir '=https' -o "$2" "$1"
}

# --- 1. matcher DLL ---
if [ -e "$DLL" ]; then
    if ! inmu_verify_sha256 "$DLL" "$INMU_DLL_SHA256"; then
        rm -f "$DLL"
        fail "$DLL の SHA-256 が既知の値と一致しません (改ざんまたは破損)。削除しました — もう一度実行して取得し直してください"
    fi
else
    echo "==> [vendor 1/2] FocalTech Windowsドライバ (.cab) をダウンロード"
    download "$CAB_URL" "$WORK/ftfocal.cab"
    if ! inmu_verify_sha256 "$WORK/ftfocal.cab" "$INMU_CAB_SHA256"; then
        fail "ダウンロードした .cab の SHA-256 が既知の値と一致しません (展開せずに破棄しました)"
    fi

    CABEXTRACT="$(command -v cabextract 2>/dev/null || true)"
    if [ -z "$CABEXTRACT" ] && [ -x /opt/homebrew/bin/cabextract ]; then
        CABEXTRACT=/opt/homebrew/bin/cabextract
    fi
    if [ -z "$CABEXTRACT" ]; then
        echo "==> cabextract をインストール (Homebrew)"
        /opt/homebrew/bin/brew install cabextract
        CABEXTRACT="$(command -v cabextract 2>/dev/null || echo /opt/homebrew/bin/cabextract)"
    fi
    # 必要な DLL だけを、空の作業用ディレクトリへ展開する (vendor/ 直下の既存
    # ファイルを上書きさせない)
    mkdir "$WORK/cab"
    "$CABEXTRACT" -q -F ftWbioEngineAdapter.dll -d "$WORK/cab" "$WORK/ftfocal.cab"
    if ! inmu_verify_sha256 "$WORK/cab/ftWbioEngineAdapter.dll" "$INMU_DLL_SHA256"; then
        fail "取り出した ftWbioEngineAdapter.dll の SHA-256 が既知の値と一致しません (破棄しました)"
    fi
    mv -f "$WORK/cab/ftWbioEngineAdapter.dll" "$DLL"
fi
echo "==> [vendor 1/2] ftWbioEngineAdapter.dll: $(stat -f '%z' "$DLL") bytes, SHA-256 照合 ✅"

# --- 2. MCU ファームウェア → ft9201_fw.h ---
if [ -e "$FW_HDR" ]; then
    if ! inmu_verify_sha256 "$FW_HDR" "$INMU_FW_HDR_SHA256"; then
        rm -f "$FW_HDR"
        fail "$FW_HDR の SHA-256 が既知の値と一致しません (改ざんまたは破損)。削除しました — もう一度実行して生成し直してください"
    fi
else
    # 取得済みの .deb は照合を通った場合のみ再利用する
    if [ -e "$DEB" ] && ! inmu_verify_sha256 "$DEB" "$INMU_DEB_SHA256"; then
        echo "    $DEB の SHA-256 が既知の値と一致しないため削除して取得し直します" >&2
        rm -f "$DEB"
    fi
    if [ ! -e "$DEB" ]; then
        echo "==> [vendor 2/2] ft9201-static blob (.deb) をダウンロード"
        download "$DEB_URL" "$WORK/ft9201-static.deb"
        if ! inmu_verify_sha256 "$WORK/ft9201-static.deb" "$INMU_DEB_SHA256"; then
            fail "ダウンロードした .deb の SHA-256 が既知の値と一致しません (展開せずに破棄しました)"
        fi
        mv -f "$WORK/ft9201-static.deb" "$DEB"
    fi

    # 照合済みの .deb を作業用ディレクトリへコピーし、コピーを照合し直してから
    # 必要なメンバーだけを展開する (照合と展開の間に差し替えられないように)
    mkdir "$WORK/deb"
    cp "$DEB" "$WORK/deb/ft9201-static.deb"
    if ! inmu_verify_sha256 "$WORK/deb/ft9201-static.deb" "$INMU_DEB_SHA256"; then
        fail "$DEB の SHA-256 が既知の値と一致しません"
    fi
    ( cd "$WORK/deb" && ar x ft9201-static.deb data.tar.xz && tar -xJf data.tar.xz "$DEB_SO_MEMBER" )
    SO="$WORK/deb/$DEB_SO_MEMBER"
    if [ ! -f "$SO" ] || [ -L "$SO" ]; then
        fail "libfprint-2.so.2.0.0 が見つからない"
    fi
    python3 -I scripts/extract_firmware.py "$SO" "$WORK/ft9201_fw.h"
    if ! inmu_verify_sha256 "$WORK/ft9201_fw.h" "$INMU_FW_HDR_SHA256"; then
        fail "生成した ft9201_fw.h の SHA-256 が既知の値と一致しません (破棄しました)"
    fi
    mv -f "$WORK/ft9201_fw.h" "$FW_HDR"
fi
echo "==> [vendor 2/2] ft9201_fw.h: $(grep -o 'ft9201_fw_size = [0-9]*' "$FW_HDR" | head -1), SHA-256 照合 ✅"
echo "==> vendor 取得完了"
