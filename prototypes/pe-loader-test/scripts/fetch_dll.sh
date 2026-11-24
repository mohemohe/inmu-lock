#!/bin/bash
# ftWbioEngineAdapter.dll (FocalTech の Windows 用マッチングエンジン) を取得する。
#
# 出典: Microsoft Update Catalog で公開されている FocalTech 署名済みドライバ
# (OMGrant/ft9201-libfprint の scripts/fetch-blobs.sh と同一の入手先・手法)。
# DLL は FocalTech の所有物のためリポジトリにはコミットしない (.gitignore 済み)。
#
# 事前に必要: Homebrew (cabextract がなければインストールする)

set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p blobs

DLL_CAB="https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2023/05/fd237921-f610-43de-b77c-f1685416480b_710b4d2c8dd2e80043e371b91bebb1721b5163a0.cab"

if [ -f blobs/ftWbioEngineAdapter.dll ]; then
    echo "==> blobs/ftWbioEngineAdapter.dll は取得済み"
else
    echo "==> FocalTech Windowsドライバ (.cab) をダウンロード"
    curl -fsSL -o blobs/ftfocal.cab "$DLL_CAB"

    CABEXTRACT="$(command -v cabextract 2>/dev/null || true)"
    if [ -z "$CABEXTRACT" ] && [ -x /opt/homebrew/bin/cabextract ]; then
        CABEXTRACT=/opt/homebrew/bin/cabextract
    fi
    if [ -z "$CABEXTRACT" ]; then
        echo "==> cabextract をインストール (Homebrew)"
        /opt/homebrew/bin/brew install cabextract
        CABEXTRACT="$(command -v cabextract 2>/dev/null || echo /opt/homebrew/bin/cabextract)"
    fi

    echo "==> cabextract で展開"
    "$CABEXTRACT" -q -d blobs blobs/ftfocal.cab
fi

ls -la blobs/ftWbioEngineAdapter.dll
echo "==> 完了。次: make run"
