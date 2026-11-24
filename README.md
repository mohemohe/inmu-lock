# inmu-lock

macOS において、汎用USB指紋センサー **Focal-systems FT9201 (VID 2808:93A9)** で
次の2つを実現するシステム:

1. **`sudo` 実行時の指紋認証** — PAM モジュール (`sudo_local` 経由)
2. **画面ロック解除** — 常駐デーモン + Keychain + キー入力注入

アーキテクチャは immurok (IK-1) の macOS 版と同じ分離方式
（sudo は PAM、画面ロック解除は Keychain＋キーストローク注入）。

Focal-systems FT9201 が使用されている指紋センサーとして、以下のデバイスで動作を確認しました。

- [ARCANITE AKFSD-07](https://www.amazon.co.jp/dp/B07VK71TST/): 3000円前後

---

## クイックスタート (CLI)

```sh
make vendor        # 依存バイナリ取得 (FocalTech DLL + MCU fw)。SHA-256 を既知の値と照合
make               # ビルド (inmu-agent は Hardened Runtime 付きで ad-hoc 署名される)
make install       # /usr/local + launchd + sudo_local へ組み込み。登録オーナーも記録

# 指紋を登録 (対話: 指を置く)。センサーはデーモンが占有しているので止めてから:
sudo make stop-daemon && sudo make enroll && sudo make start-daemon

# 画面ロック解除を使う場合のみ (パスワードを Keychain に保存する。下の残存リスク (c) 参照):
/usr/local/bin/inmu-agent --set-password    # 端末から入力 (パイプ不可)
# アクセシビリティ権限: システム設定 → プライバシーとセキュリティ →
#   アクセシビリティ → /usr/local/bin/inmu-agent を追加
make restart-agent

# 動作確認:
sudo -k && sudo whoami     # → 「指紋センサーに触れてください」→ 触る → root
⌃⌘Q → 指紋センサーに触れる   # → ロック解除
make status && make logs
```

- `make enroll` は**インストール済みの** `/usr/local/bin/inmu-enroll` を実行する
  (ユーザーが書き換えられる `build/` の生成物を root で実行しないため)。テンプレートと一緒に
  「登録オーナー」(指紋の持ち主の uid) を `/usr/local/etc/inmu-lock/owner` に記録する
- 指紋で承認されるのは、**登録オーナー本人が、コンソールの GUI セッション内で、画面が
  ロックされていない時に実行した sudo** だけ。SSH セッション内で実行した sudo・他ユーザー・
  画面ロック中は常にパスワード入力になる (登録オーナー本人の権限で SSH ログインできる相手は
  GUI セッション側で sudo を起動させられるので、残存リスク (b) と同じ扱いになる)
- 1 回のタッチで承認される sudo は 1 件だけ。連続 5 回の不一致で 60 秒〜15 分のロックアウト
  (その間もパスワードは使える)
- 指紋を待っている sudo が無く、画面もロックされていない時にセンサーへ触れても、撮影も照合も
  しない (不一致として数えられることもない)

> 開発モード（`/usr/local` に入れずにセンサー・照合・IPC を試す。センサーは排他利用なので
> インストール済みのデーモンは止めておく）:
> ```sh
> build/inmu-enroll --engine vendor/ftWbioEngineAdapter.dll \
>   --out /tmp/inmu/tmpl.bin --owner-file /tmp/inmu/owner
> build/inmu-cored --template /tmp/inmu/tmpl.bin --engine vendor/ftWbioEngineAdapter.dll \
>   --sockdir /tmp/inmu/sock --state-dir /tmp/inmu/state --owner-file /tmp/inmu/owner
> ```
> 非 root で `--sockdir` と `--state-dir` の両方を指定するとテストモードになる。このモードの
> デーモンは **本物の sudo からは使えない** (PAM モジュールは root のデーモンにしか接続せず、
> root のデーモンは root 所有のテンプレート・DLL・ディレクトリしか受け付けない)。
> IPC は `src/common/ipc.h` の行プロトコルを話す自作クライアントで確認する。

### 既存インストールからの更新

セキュリティ修正より前のバージョンを入れている場合。プロトコルが変わったため、
新旧のコンポーネントは混在できない。

1. `make clean && make vendor && make && make install` — 全コンポーネントを入れ替える。
   `/usr/local` 配下のインストール先が root 所有・他者書き込み不可でない場合は中止する
2. **登録オーナー**: `install.sh` は `/usr/local/etc/inmu-lock/owner` が無ければ、実行した
   ユーザーの uid で作る (既存のテンプレートはそのユーザーの指紋とみなす)。指紋の持ち主が
   別のユーザーなら、`sudo make stop-daemon && sudo make enroll && sudo make start-daemon` で
   本人が登録し直すこと。このファイルが無い・不正な間、デーモンは一切承認しない
3. **アクセシビリティの再許可**: `inmu-agent` の署名が変わるため、システム設定の一覧から
   既存の `inmu-agent` を「−」で削除し、追加し直す
4. **`/usr/local/bin/inmu-agent --set-password` のやり直し**: Keychain 項目のアクセス許可も
   署名に結び付いている。その後 `make restart-agent`
5. 上の「動作確認」を行う。SSH 越しの `sudo` が指紋で通らないことも確認するとよい

3 と 4 は、以後も `inmu-agent` をビルドし直してインストールするたびに必要になる
(ad-hoc 署名はビルドごとに変わる)。

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
  fetch-vendor.sh           依存バイナリ取得 (MS Update Catalog + ft9201-static、SHA-256 照合)
  common.sh                 共有定数 (vendor の SHA-256)・実ユーザーの特定
  install.sh / uninstall.sh
  daemon.sh / agent.sh      launchd の起動・停止 (make start-daemon 等の実体)
launchd/
  dev.mohemohe.inmu-lock.cored.plist   (LaunchDaemon, root)
  dev.mohemohe.inmu-lock.agent.plist    (LaunchAgent, user)
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
| `/usr/local/etc/inmu-lock/template.bin` | 登録テンプレート (root 0600。`make enroll` が作る) |
| `/usr/local/etc/inmu-lock/owner` | 登録オーナーの uid (root 0644、10 進 1 行) |
| `/var/db/inmu-lock/lockout` | 連続失敗カウンタ・ロックアウト状態 (root 0700/0600) |
| `/var/run/inmu/{pam,agent}.sock` | IPC ソケット (0600。デーモンが作る) |
| `/var/log/inmu-lock/` | デーモンのログ (root 0750/0600。`make logs`) |
| `/etc/pam.d/sudo_local` | `auth sufficient pam_inmu.so timeout_ms=5000` (1行・マーカー付き) |
| `/Library/LaunchDaemons/dev.mohemohe.inmu-lock.cored.plist` | デーモン (KeepAlive) |
| `~/Library/LaunchAgents/dev.mohemohe.inmu-lock.agent.plist` | エージェント |

**安全装置** (詳細は docs/01 §4):
- デーモン不在・応答が `OK` 以外・少しでも想定外の場合、PAM は `PAM_IGNORE`
  (sudo は従来どおりパスワード入力)
- 承認は登録オーナー本人・コンソール GUI セッション・画面非ロックの sudo に限る。
  1 回のタッチで 1 件だけ。指を置きっぱなしにしても再承認しない
- 連続 5 回の不一致でロックアウト (再起動・USB 抜き差しでも消えない。試行は照合の前に
  記録し、記録できなければ照合しない)
- agent への通知は画面ロック中だけ。agent も画面ロック中のみ注入し、1 文字ごとにロック状態を
  再確認 (誤爆防止)
- 各ソケットは 0600 で、双方が接続相手を検証する

## アンインストール (`make uninstall`)

launchd の登録・`/etc/pam.d/sudo_local` のマーカー行・インストールしたファイル・デーモンと
agent のログを削除し、Keychain に保存したログインパスワードも
`inmu-agent --unset-password` で削除する (削除できたかは最後に表示される)。

`sudo_local` を手で編集していてマーカー行が `install.sh` の書いた形と違う場合は書き換えない。
`/etc/pam.d` に `pam_inmu.so` を参照する行が残っている間は、sudo を壊さないよう
`/usr/local/lib/pam/pam_inmu.so` を削除せず、該当行を表示して終了コード 1 で終わる
(デーモンは削除済みなので sudo はパスワード入力になる)。表示された行を手で消してから
もう一度 `make uninstall` を実行する。

**次のものは残る**ので、必要なら手動で削除する:

- **Keychain のログインパスワード** (自動削除に失敗した場合、または `make uninstall` を
  実行したユーザー以外が保存していた場合): アンインストール前なら
  `/usr/local/bin/inmu-agent --unset-password`、後なら
  `security delete-generic-password -s dev.mohemohe.inmu-lock -a login-password`、
  またはキーチェーンアクセス →「ログイン」→ `dev.mohemohe.inmu-lock` を削除
- アクセシビリティの許可 (システム設定 → プライバシーとセキュリティ → アクセシビリティ)
- `/etc/pam.d/sudo_local.bak.<日時>` (install / uninstall が書き換え前に作るバックアップ)
- `make uninstall` を実行したユーザー以外の `~/Library/LaunchAgents/dev.mohemohe.inmu-lock.agent.plist`
  と `~/Library/Logs/inmu-lock/` (複数ユーザーでインストールした場合は、各ユーザーで
  `make uninstall` を実行するか手動で削除する)

## セキュリティ上の残存リスク

ソフトウェアでは解消できない、または解消していないもの。全文は
[docs/01-architecture.md §4.6](docs/01-architecture.md) にある。

- **(a) センサーを認証できない**: USB の VID/PID でしか識別できず、画像は平文。生体検知も
  無い。偽造指や、USB に挿した偽デバイスからの画像再生を区別できない。ロックアウトは
  当てずっぽうの総当たりを遅くするだけ。物理的に触れられる相手にはパスワードより弱い
- **(b) タッチの便乗**: 自分の権限で GUI セッション内に常駐するマルウェアは、`sudo` を
  待機させて、別の目的のタッチを横取りできる。「いま何を承認するか」を示す信頼できる
  UI は無い。見覚えのない指紋プロンプトが出ているときは触れないこと。登録オーナー本人の
  権限で SSH ログインできる相手も、GUI セッション側で `sudo` を起動させれば同じ立場になる
  (遮断しているのは SSH セッション内で直接実行した `sudo` だけ)
- **(c) パスワードの保存**: 画面ロック解除を使う限り、ログインパスワードが login keychain に
  残る。保護は keychain の ACL 頼みで、より強い data protection keychain は使えない。
  避けたい場合は `--set-password` を実行しない (sudo の指紋認証だけが有効になる)
- **(d) 個別の未解決事項**: ロック画面がユーザー名欄を表示する構成ではパスワードが平文で
  打たれうる / ロック解除と同時に最大 1 文字が解除後の画面に届きうる / root のデーモンが
  クローズドソースの Windows 用 DLL を自作ローダーで実行しており、テンプレートファイルは
  その DLL にとって信頼された入力である (root 0600 で保護) / 1 回のロックアウトは最大 15 分
  だが、失敗カウンタは一致するまで減らないため、センサーに触れられる人はロックアウト明けに
  触り続けることで指紋認証を止め続けられる (パスワードは使える) / ロックアウトの残り時間は
  OS の再起動をまたぐと壁時計で復元するため、時計を進めてから OS を再起動できる相手は
  残り時間を短縮できる (失敗カウンタは残る) / agent への MATCH 通知は
  登録オーナーの権限で動くプロセスなら受信しうる (承認ではなく、画面ロック中の一致の通知)

## 前提・制約

- FileVault 解除（電源投入直後）は対象外。初回は手動パスワード入力
- FocalTech の DLL・fw は `make vendor` が公開配布元から自動取得し、既知の SHA-256 と照合する（コミットしない）
- matcher は x86_64 → `inmu-cored` は Rosetta 2 で実行 (Apple Silicon / Intel 両対応)
- USB 断線時の自動再接続は未実装 (KeepAlive で再起動はする)
- `daemon/` (SwiftPM) は将来の Swift 化への参照実装（設計ドキュメント docs/02 参照）

## 技術の核 (docs 詳細)

- **PAM**: `/etc/pam.d/sudo_local` → `pam_inmu.so` → `pam.sock` (root 0600) →
  `WAIT <ms> <uid>` → cored が承認ポリシーを判定 → `OK` のときだけ `PAM_SUCCESS`
- **ロック解除**: cored → `agent.sock` (登録オーナー 0600) → agent →
  コンソール上かつ `CGSSessionScreenIsLocked` 判定 →
  Keychain (`dev.mohemohe.inmu-lock`) → `CGEventPost` 注入
- **matcher**: x86_64 PE (FocalTech 製) を Apple Silicon の Rosetta 2 上で in-process 実行。
  `%gs` (TEB) は mach trap `0x3000003`、libc 呼び出しは gs 切替え規律で保護
  (docs/03 §3-a)
