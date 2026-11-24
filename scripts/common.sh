#!/bin/bash
# common.sh — scripts/ 配下で共有する定数と関数 (単体では実行しない。source して使う)。
#
#   - vendor 取得物の既知の SHA-256 と照合関数
#   - 実ユーザー (sudo 経由なら SUDO_USER) の特定と検証
#   - 実ユーザー権限 / root 権限でのコマンド実行

# --- vendor 取得物の既知の SHA-256 -----------------------------------------
# ここに無い内容のファイルは一切使わない (照合に失敗したら中止する)。
# ベンダーが配布物を更新した場合は、内容を確認したうえでここを更新すること。

# FocalTech Windows ドライバ (.cab — Microsoft Update Catalog)
INMU_CAB_SHA256="65dffda7ddc76d30b324ebf02c989cb74d0ec85ceeac3b1e10d37fcc3d321811"
# .cab から取り出した matcher DLL (root の inmu-cored / inmu-enroll が実行する)
INMU_DLL_SHA256="ca34ba1e8494fc84a9ac31dd5c003e825d847484e5e3509f099de123d7953c6a"
# ft9201-static の .deb (MCU ファームウェアの抽出元)
INMU_DEB_SHA256="a95acd783a6cf6d1309595d08758b5e2959eac809204d55399ccaef620cd3ebc"
# scripts/extract_firmware.py が上記 .deb から生成する src/lib/ft9201_fw.h
INMU_FW_HDR_SHA256="32cb2e5138c561fce68fcb02811f37e15d81b7f8674c5bc5a744cab9d8e0de1c"

# inmu_sha256 <file> — SHA-256 (16 進小文字) を標準出力へ。読めなければ空。
inmu_sha256() {
    /usr/bin/shasum -a 256 < "$1" 2>/dev/null | /usr/bin/awk '{print $1}'
}

# inmu_verify_sha256 <file> <expected> — 一致すれば 0。
# ファイルが無い・読めない・ハッシュを計算できない場合も不一致として扱う。
inmu_verify_sha256() {
    local actual
    [ -f "$1" ] || return 1
    actual="$(inmu_sha256 "$1")" || return 1
    [ ${#actual} -eq 64 ] && [ "$actual" = "$2" ]
}

# --- 実ユーザーの特定 ---------------------------------------------------------
# inmu_resolve_real_user — REAL_USER / REAL_UID / REAL_HOME を設定する。
#
#   一般ユーザーで実行:      そのユーザー
#   sudo 経由 (root) で実行: SUDO_USER (sudo が設定した呼び出し元)
#   root のまま (SUDO_USER 無し): 拒否 (agent / Keychain / 登録オーナーの対象が決まらない)
#
# ユーザー名はシェルやパスへ展開する前に厳密に検証し、ホームは eval ではなく
# ディレクトリサービス (dscl) から引く。失敗時は 1 を返す。
inmu_resolve_real_user() {
    local LC_ALL=C
    local cand uid home owner

    if [ "$(id -u)" = "0" ]; then
        cand="${SUDO_USER:-}"
        if [ -z "$cand" ]; then
            echo "  !! root のまま (sudo 経由でない) の実行はできません。" \
                 "一般ユーザーで実行するか、そのユーザーから sudo で実行してください" >&2
            return 1
        fi
    else
        cand="$(id -un)" || return 1
    fi

    if ! [[ "$cand" =~ ^[A-Za-z0-9_][A-Za-z0-9._-]{0,254}$ ]]; then
        echo "  !! ユーザー名が不正です (使用できない文字を含む)" >&2
        return 1
    fi

    uid="$(id -u -- "$cand" 2>/dev/null)" || uid=""
    if ! [[ "$uid" =~ ^[0-9]{1,10}$ ]]; then
        echo "  !! ユーザー '$cand' が存在しません" >&2
        return 1
    fi
    if [ "$uid" = "0" ]; then
        echo "  !! root を対象ユーザーにはできません" >&2
        return 1
    fi
    # sudo は SUDO_USER と同時に SUDO_UID も設定する。食い違う環境は信用しない。
    if [ "$(id -u)" = "0" ] && [ -n "${SUDO_UID:-}" ] && [ "$SUDO_UID" != "$uid" ]; then
        echo "  !! SUDO_USER と SUDO_UID が一致しません" >&2
        return 1
    fi

    # "NFSHomeDirectory: /Users/name" (空白を含む値は次の行に出力される)
    home="$(/usr/bin/dscl /Search -read "/Users/$cand" NFSHomeDirectory 2>/dev/null)" || home=""
    case "$home" in
        NFSHomeDirectory:*) home="${home#NFSHomeDirectory:}" ;;
        *) home="" ;;
    esac
    home="${home#"${home%%[![:space:]]*}"}"
    case "$home" in
        /*) ;;
        *)
            echo "  !! ユーザー '$cand' のホームディレクトリを取得できません" >&2
            return 1
            ;;
    esac
    case "$home" in
        *$'\n'*)
            echo "  !! ユーザー '$cand' のホームディレクトリが不正です" >&2
            return 1
            ;;
    esac
    owner="$(/usr/bin/stat -f '%u' "$home" 2>/dev/null)" || owner=""
    if [ ! -d "$home" ] || [ "$owner" != "$uid" ]; then
        echo "  !! ホームディレクトリ '$home' が存在しないか、'$cand' の所有ではありません" >&2
        return 1
    fi

    REAL_USER="$cand"
    REAL_UID="$uid"
    REAL_HOME="$home"
}

# inmu_as_user <cmd> [args...] — 実ユーザーの権限でコマンドを実行する。
# (root で動いているときだけ sudo -u で降格する。引数は単語分割されない)
inmu_as_user() {
    if [ "$(id -u)" = "0" ]; then
        sudo -u "$REAL_USER" -H -- "$@"
    else
        "$@"
    fi
}

# inmu_as_root <cmd> [args...] — root 権限でコマンドを実行する。
inmu_as_root() {
    if [ "$(id -u)" = "0" ]; then
        "$@"
    else
        sudo -- "$@"
    fi
}
