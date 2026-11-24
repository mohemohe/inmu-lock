#!/bin/bash
# FT9348W MCU firmware (ft9201_fw.h) を取得・生成する。
#
#   1. mrrbrilliant/ft9201-static の FocalTech Linux libfprint blob (.deb) をダウンロード
#   2. ar + tar で libfprint-2.so.2.0.0 を取り出す
#   3. python3 で FOCALFP_9348_FW_APP シンボルを抽出 → src/ft9201_fw.h
#
# fw は FocalTech の所有物 → リポジトリにはコミットしない (.gitignore 済み)。

set -euo pipefail
cd "$(dirname "$0")/.."

DEB_URL="https://github.com/mrrbrilliant/ft9201-static/raw/main/libfprint_2_2_1_90_1%2Btod1_0ubuntu120_04_2_amd64_16c6e64404f8411.deb"

if [ -f src/ft9201_fw.h ]; then
    echo "==> src/ft9201_fw.h は生成済み ($(grep -o 'ft9201_fw_size = [0-9]*' src/ft9201_fw.h | head -1))"
    exit 0
fi

mkdir -p blobs/deb
if [ ! -f blobs/ft9201-static.deb ]; then
    echo "==> ft9201-static blob (.deb) をダウンロード"
    curl -fsSL -o blobs/ft9201-static.deb "$DEB_URL"
fi

echo "==> .deb を展開 (ar + tar)"
( cd blobs/deb && ar x ../ft9201-static.deb && tar -xJf data.tar.xz )

SO="$(find blobs/deb -name 'libfprint-2.so.2.0.0' | head -1)"
if [ -z "$SO" ]; then
    echo "!! libfprint-2.so.2.0.0 が見つからない" >&2
    exit 1
fi

echo "==> fw を抽出: $SO"
python3 scripts/extract_firmware.py "$SO" src/ft9201_fw.h
