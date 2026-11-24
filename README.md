# inmu-lock

macOS において、汎用USB指紋センサー **Focal-systems FT9201 (VID 2808:93A9)** で
次の2つを実現するシステム:

1. **`sudo` 実行時の指紋認証** — PAM モジュール (`sudo_local` 経由)
2. **画面ロック解除** — 常駐デーモン + Keychain + キー入力注入

アーキテクチャは immurok (IK-1) の macOS 版と同じ分離方式
（sudo は PAM、画面ロック解除は Keychain＋キーストローク注入）。

> **現在地: 本番構成 (CLI) 実装済み — 全経路が実機で動作実証済み**
>
> - **検証A (PAM / sudo): ✅** パスワードレスsudo・フォールバック・ロールバック安全性
> - **検証B (画面ロック): ✅** ロック判定・Keychain・ロック画面へのキー注入
> - **検証C (matcher/Rosetta 2): ✅** vendor DLL が Apple Silicon で動作、
>   実指紋で genuine 3/3 MATCH / imposter 3/3 reject
> - **検証D (USB): ✅** fw転送による bring-up (MCU起動 `a5 5a`)・96×96 実画像取得
> - **本番構成 `src/` + `Makefile`:** cored / pam / agent / enroll + launchd。
>   **実ロック画面での sudo 認証・ロック解除の統合テストも通過済み**

---

## クイックスタート (CLI)

```sh
make vendor        # 依存バイナリ取得 (FocalTech DLL + MCU fw) — 初回のみ
make               # ビルド
make install       # /usr/local + launchd + sudo_local へ組み込み
make enroll        # 指紋を登録 (対話: 指を置く)

# 画面ロック解除を使う場合のみ:
/usr/local/bin/inmu-agent --set-password    # パスワードをKeychainに保存
# アクセシビリティ権限: システム設定 → プライバシーとセキュリティ →
#   アクセシビリティ → /usr/local/bin/inmu-agent を追加
launchctl kickstart -k gui/$(id -u)/com.mohemohe.inmu-lock.agent

# テスト:
sudo -k && sudo whoami     # → 「指紋センサーに触れてください」→ 触る → root
⌃⌘Q → 指紋センサーに触れる   # → ロック解除
make status && make logs
```

> 開発モード（`/usr/local` に入れずに試す）:
> ```sh
> make enroll  # または build/inmu-enroll --out /tmp/tmpl.bin
> sudo build/inmu-cored --template /tmp/tmpl.bin --engine vendor/ftWbioEngineAdapter.dll \
>   --sockdir /tmp/inmu-ipc
> # /etc/pam.d/sudo_local に sock=/tmp/inmu-ipc/pam.sock を手動で一時設定することで
> # PAM接続先を変更可能 (sock= モジュール引数)
> ```

## ディレクトリ構成

```
Makefile                    make / make vendor / make enroll / make install / make uninstall
src/
  common/ipc.h              コンポーネント間プロトコル (Unix socket, 行ベース)
  lib/                      検証済み共通ライブラリ (検証C/D の実績コード)
    usb_transport.c         IOKit ユーザースペース USB (bulk, vendor control)
    ft9201.c                FT9201 bring-up (fw転送) + 96×96 画像取得
    ft_engine_mac.c         vendor matcher DLL の PE ローダー (Rosetta 2, LGPL-2.1)
    ft9201_fw.h             fw 配列 (make vendor が生成 — コミットしない)
  cored/inmu-cored.c        Core Service デーモン (root, x86_64/Rosetta):
                            センサー制御 + 照合 + IPC (pam.sock/agent.sock)
  pam/pam_inmu.c            PAM モジュール (universal): socketクライアント
  agent/inmu-agent.c        Unlock Agent (universal): MATCH→ロック判定→Keychain→注入
  enroll/inmu-enroll.c      登録ツール (x86_64): 対話的に指紋を登録
scripts/
  fetch-vendor.sh           依存バイナリ取得 (MS Update Catalog + ft9201-static)
  install.sh / uninstall.sh
launchd/
  com.mohemohe.inmu-lock.cored.plist   (LaunchDaemon, root)
  com.mohemohe.inmu-lock.agent.plist    (LaunchAgent, user)
tools/                      診断ツール (src/lib を直接使う):
  capture_test             センサー画像をPGM保存 (照合不良時の原因調査)
  engine_test              画像ファイルで matcher 登録/照合 (デーモン不要)
  usb_probe                USB 構成確認
vendor/                     取得物 (gitignore)
docs/
  01-architecture.md        全体設計・言語選定・コンポーネント詳細・セキュリティ
  02-sensor-interface.md    センサー抽象化インターフェース設計
  03-ft9201-notes.md        センサー技術調査（Linux実装・matcher問題・T3実績）
  04-testing-rollback.md    検証A〜B 手順とロールバック記録
daemon/                     SwiftPM スケルトン (センサー抽象化設計の参照実装)
```

> 検証A〜D の実験コード (pam-dummy / lock-probe / pe-loader-test / usb-transport) は
> すべて本番構成へ昇格したため作業ツリーから削除された。
> 検証時のコードと手順は git 初期コミット (`1025ca1`) に永久保存されている。
> docs/04 は当時の手順・結果の記録としてそのまま残している。

## インストールされるもの (`make install`)

| パス | 内容 |
|---|---|
| `/usr/local/bin/inmu-{cored,agent,enroll}` | 本体3種 |
| `/usr/local/lib/pam/pam_inmu.so` | PAM モジュール |
| `/usr/local/libexec/inmu-lock/ftWbioEngineAdapter.dll` | vendor matcher |
| `/usr/local/etc/inmu-lock/template.bin` | 登録テンプレート (0600) |
| `/etc/pam.d/sudo_local` | `auth sufficient pam_inmu.so timeout_ms=3000` (1行・マーカー付き) |
| `/Library/LaunchDaemons/com.mohemohe.inmu-lock.cored.plist` | デーモン (KeepAlive) |
| `~/Library/LaunchAgents/com.mohemohe.inmu-lock.agent.plist` | エージェント |

**安全装置**:
- デーモン不在時、PAM は即 `PAM_IGNORE` (sudo は従来どおり)
- 画面ロック中のみ注入 (誤爆防止 — `CGSSessionScreenIsLocked` 判定)
- `make uninstall` で完全解除 (`/etc/pam.d` はマーカー行のみ除去)

## 前提・制約

- FileVault 解除（電源投入直後）は対象外。初回は手動パスワード入力
- FocalTech の DLL・fw は `make vendor` が公開配布元から自動取得（コミットしない）
- matcher は x86_64 → `inmu-cored` は Rosetta 2 で実行 (Apple Silicon / Intel 両対応)
- USB 断線時の自動再接続は未実装 (KeepAlive で再起動はする)
- `daemon/` (SwiftPM) は将来の Swift 化への参照実装（設計ドキュメント docs/02 参照）

## 技術の核 (docs 詳細)

- **PAM**: `/etc/pam.d/sudo_local` → `pam_inmu.so` → `pam.sock` (root) → MATCH で `PAM_SUCCESS`
- **ロック解除**: cored → `agent.sock` → agent → `CGSSessionScreenIsLocked` 判定 →
  Keychain (`com.mohemohe.inmu-lock`) → `CGEventPost` 注入
- **matcher**: x86_64 PE (FocalTech 製) を Apple Silicon の Rosetta 2 上で in-process 実行。
  `%gs` (TEB) は mach trap `0x3000003`、libc 呼び出しは gs 切替え規律で保護
  (docs/03 §3-a)
