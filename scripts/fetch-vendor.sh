#!/bin/bash
# 依存バイナリ (vendor blobs) を取得する:
#   1. ftWbioEngineAdapter.dll — FocalTech の Windows 用マッチングエンジン
#      (Microsoft Update Catalog 公開の署名済みドライバ .cab から)
#   2. FT9348W MCU ファームウェア — mrrbrilliant/ft9201-static の FocalTech
#      Linux blob (.deb) から抽出 → src/lib/ft9201_fw.h を生成
#
# いずれも FocalTech の所有物のためリポジトリにはコミットしない (.gitignore)。
# 必要なツール: cabextract (無ければ Homebrew で自動インストール), tar (xz), python3, nm

set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p vendor

CAB_URL="https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2023/05/fd237921-f610-43de-b77c-f1685416480b_710b4d2c8dd2e80043e371b91bebb1721b5163a0.cab"
DEB_URL="https://github.com/mrrbrilliant/ft9201-static/raw/main/libfprint_2_2_1_90_1%2Btod1_0ubuntu120_04_2_amd64_16c6e64404f8411.deb"

# --- 1. matcher DLL ---
if [ ! -f vendor/ftWbioEngineAdapter.dll ]; then
    echo "==> [vendor 1/2] FocalTech Windowsドライバ (.cab) をダウンロード"
    curl -fsSL -o vendor/ftfocal.cab "$CAB_URL"

    CABEXTRACT="$(command -v cabextract 2>/dev/null || true)"
    if [ -z "$CABEXTRACT" ] && [ -x /opt/homebrew/bin/cabextract ]; then
        CABEXTRACT=/opt/homebrew/bin/cabextract
    fi
    if [ -z "$CABEXTRACT" ]; then
        echo "==> cabextract をインストール (Homebrew)"
        /opt/homebrew/bin/brew install cabextract
        CABEXTRACT="$(command -v cabextract 2>/dev/null || echo /opt/homebrew/bin/cabextract)"
    fi
    "$CABEXTRACT" -q -d vendor vendor/ftfocal.cab
    rm -f vendor/ftfocal.cab
fi
echo "==> [vendor 1/2] ftWbioEngineAdapter.dll: $(ls -la vendor/ftWbioEngineAdapter.dll | awk '{print $5}') bytes ✅"

# --- 2. MCU ファームウェア → ft9201_fw.h ---
if [ ! -f src/lib/ft9201_fw.h ]; then
    echo "==> [vendor 2/2] ft9201-static blob (.deb) をダウンロード"
    curl -fsSL -o vendor/ft9201-static.deb "$DEB_URL"
    rm -rf vendor/deb && mkdir -p vendor/deb
    ( cd vendor/deb && ar x ../ft9201-static.deb && tar -xJf data.tar.xz )
    SO="$(find vendor/deb -name 'libfprint-2.so.2.0.0' | head -1)"
    if [ -z "$SO" ]; then
        echo "!! libfprint-2.so.2.0.0 が見つからない" >&2
        exit 1
    fi
    python3 scripts/extract_firmware.py "$SO" src/lib/ft9201_fw.h
fi
echo "==> [vendor 2/2] ft9201_fw.h: $(grep -o 'ft9201_fw_size = [0-9]*' src/lib/ft9201_fw.h | head -1) ✅"
echo "==> vendor 取得完了"
