# 04 — 検証A/B テスト手順とロールバック

Phase 0 の2つのプロトタイプで「OSのどこまでが許されるか」を安全に確かめる。

- **検証A** (ダミーPAMモジュール): `sudo_local` 経路とPAM返り値の効き方を確認
- **検証B** (ロック判定プローブ): 画面ロック判定・Keychain・キー注入の連鎖を確認

> **注記 (2026-11-25)**: 以下の手順は 2026-11-24 に実施した歴史的記録である。
> 検証に使ったコード (pam-dummy / lock-probe) は本番昇格後に作業ツリーから
> 削除されたが、**git 初期コミット (`1025ca1`) に保存されている**
> (`git show 1025ca1:prototypes/pam-dummy/README.md` で参照可)。
> 手順中のパスは当時のもの。

---

## 検証A — ダミーPAMモジュール

### A-0. 前提と安全装置

- モジュールの**デフォルトの返り値は `PAM_IGNORE`** = 「自分は関与しない」。この状態では
  sudo の挙動は一切変わらない（ログだけ増える）
- `PAM_SUCCESS` を返すのは `demo_success` オプションが設定され、かつ
  **root 所有の** `/tmp/inmu_pam_demo_success` が存在する場合のみ
  （一般ユーザーが勝手にパスワードレス sudo を arm できない設計）
- **テスト中は別ターミナルで `sudo -s` (rootシェル) を開いたままにすること**
  （万一 sudo が壊れても root シェルから `uninstall.sh` を実行すれば戻せる。
  `uninstall.sh` は `SUDO_USER` の無い root 実行を拒否するので、root シェルは
  必ず `sudo -s` で開くこと。`sudo -i` や `su` で得た root シェルでは動かない）

### A-1. インストール

```sh
cd prototypes/pam-dummy
./install.sh          # ビルド → /usr/local/lib/pam/ へ配置 → /etc/pam.d/inmutest 作成
                      # → /etc/pam.d/sudo_local へ組み込み (既存があれば自動バックアップ)
```

`install.sh` は `/etc/pam.d/sudo_local` が既に存在する場合は
`sudo_local.bak.<日時>` にバックアップしてからマーカー付きで1行追加する
（uninstall はマーカー区間のみ除去するので他の行は保持される）。

### A-2. 単体テスト（root不要・sudo設定に触らない）

`install.sh` が作る `/etc/pam.d/inmutest` は、実運用の sudo スタックと同じ形:

```
auth      sufficient  /usr/local/lib/pam/pam_inmu_dummy.so debug demo_success
auth      required    pam_deny.so
account   required    pam_permit.so
```

- モジュールが `PAM_IGNORE` / 失敗 → `sufficient` は効果なし → `pam_deny.so` が確定的に失敗させる
- モジュールが `PAM_SUCCESS` → `sufficient` がスタックを短絡して成功
- `pam_deny.so` の壁を置く理由: **auth行1個だけのスタックでモジュールが `PAM_IGNORE` を
  返すと、OpenPAM の集計は `PAM_SUCCESS` になってしまい「無視」と「成功」の区別がつかない**
  （2026-11-24 実測）。本スタックなら `pam_authenticate` の戻り値だけで判定できる

```sh
./pam_harness inmutest
# 期待値 (exit 1):
#   pam_authenticate = 9 (authentication error)
#   モジュールの判定ログ: ... -> PAM_IGNORE (パスワード認証へフォールバック)
#   => 結果: 認証失敗

sudo touch /tmp/inmu_pam_demo_success   # root所有ファイルで成功をarm
./pam_harness inmutest
# 期待値 (exit 0):
#   pam_authenticate = 0 (success)
#   pam_acct_mgmt    = 0 (success)  [参考]
#   モジュールの判定ログ: ... DEMO: ... -> PAM_SUCCESS
#   => 結果: 認証成功

sudo rm /tmp/inmu_pam_demo_success     # arm解除
./pam_harness inmutest
# 期待値: 再び exit 1 + PAM_IGNORE
```

ハーネスの「モジュールの判定ログ」は `/tmp/inmu_pam_dummy.log` から今回の実行
（pid一致）の判定行を表示する。pid が一致しない場合は「古いログ」と明示される。

### A-3. sudo 経由のテスト（本番経路）

**重要: パスワードレス sudo のテストには `--passwordless-demo` 付きのインストールが必要。**

デフォルトの `./install.sh` は sudo_local のモジュール行を `debug` のみで書く。
この状態でモジュールは常に `PAM_IGNORE` を返す (= sudo の挙動は無変更、ログだけ増える)。
`demo_success` を行に追加するには再インストールする (既存ブロックは自動更新される):

```sh
./install.sh --passwordless-demo
```

> **2026-11-24 のテストで判明した注意点（修正済み）:**
> デフォルトのまま #2 を実行するとモジュールは `PAM_IGNORE` を返すため、通常どおり
> `Password:` プロンプトが出る。ここで**何も入力せず Enter を押す**と OpenDirectory が
> `NULL passwords are not allowed` で拒否し、3回で `sudo: 3 incorrect password attempts`
> になる (unified log で確認済み)。システムには影響せず、正しいパスワードを入力すれば
> 従来どおり通る。

```sh
# 1. 通常時: モジュールは PAM_IGNORE を返す → パスワードを聞かれる (挙動無変更)
sudo -k && sudo whoami
# 期待値: Password プロンプト → 入力で実行
# 確認: tail /tmp/inmu_pam_dummy.log に user=<自分> service=sudo の記録
#       (ログに記録があれば「sudo_local経由でモジュールが呼ばれた」証拠)

# 2. 成功シミュレーション (--passwordless-demo でインストール後):
sudo touch /tmp/inmu_pam_demo_success
sudo -k && sudo whoami
# 期待値: パスワード無しで "root" が返る (= sufficient + PAM_SUCCESS の効果)
#        確認: tail /tmp/inmu_pam_dummy.log に service=sudo の
#              "DEMO: ... -> PAM_SUCCESS" の記録
sudo rm /tmp/inmu_pam_demo_success     # ← 必ず解除
sudo -k && sudo whoami                  # 再びパスワードプロンプトに戻ることを確認

# 3. 失敗フォールバック: (必要なら) PAM_AUTH_ERR 時の挙動
#    sudo_local の行に fail オプションを一時追加して確認:
#    auth sufficient /usr/local/lib/pam/pam_inmu_dummy.so debug demo_success fail
#    → パスワードプロンプトにフォールバックすること

# 4. 待ち時間シミュレーション: wait=5 を付けて
#    タイムアウトUX(5秒待ってからプロンプト)を体感する
```

ログのリアルタイム確認（syslog側）:
※zshでは `log` がシェル組み込みコマンドと衝突するため `/usr/bin/log` を使う

```sh
/usr/bin/log stream --style compact --predicate 'eventMessage CONTAINS "pam_inmu_dummy"'
```

### A-4. 成功基準

| # | 確認項目 | 判定 |
|---|---|---|
| A-1 | `sudo_local` に1行足すだけでモジュールが呼ばれる | ログに service=sudo |
| A-2 | デフォルト (PAM_IGNORE) で sudo が従来どおり動く | パスワードプロンプト |
| A-3 | `sufficient` + `PAM_SUCCESS` でパスワードレス sudo | `--passwordless-demo` + arm 時に whoami が通る |
| A-4 | 失敗・未接続でも sudo が壊れない | フォールバックすること |

→ すべて成立すれば Phase 1 の pam_inmu.so は「socket 結果を返り値に変換するだけ」になる。

### A-5. ロールバック

```sh
cd prototypes/pam-dummy
./uninstall.sh        # sudo_local から当該行のみ除去 (空ならファイル削除)
                      # /etc/pam.d/inmutest, モジュールを削除
sudo -k && sudo whoami   # パスワードを聞かれれば完全に戻っている
```

**2026-11-24 のテストで判明した注意点（修正済み）:**

- 旧版の `/etc/pam.d/inmutest` は auth 行のみで `account` 行がなく、`pam_acct_mgmt` が
  `9 (authentication error)` を返してハーネスが常に exit 1 になっていた
  → `account required pam_permit.so` を追加して修正
- 旧版のように auth 行1個だけのスタックでモジュールが `PAM_IGNORE` を返すと、
  OpenPAM の集計は `PAM_SUCCESS`（pam_authenticate = 0）になる
  → `pam_deny.so` の壁を追加して「無視」と「成功」を区別できるように修正
- このときモジュール本体・sudo 経由の呼び出し（`service=sudo` のログで確認可能）・
  パスワードへのフォールバックはすべて正しく動作していた

**緊急時（sudo が壊れた場合）:**

1. 開いてある root シェル（A-0 で確保済み、`sudo -s` で開いたもの）から `uninstall.sh` を実行。
   `/etc/pam.d` に `pam_inmu.so` を参照する行が残っている場合、`uninstall.sh` は
   PAM モジュールを残したまま該当行を表示して終了コード 1 で終わるので、
   表示された行を手で削除してから再実行する
2. root シェルがない場合は、別の管理者ユーザーでログインして `/etc/pam.d/sudo_local` を削除
3. それも不可能なら: リカバリーモード (⌘R) → ターミナル →
   `/Volumes/<起動ディスク>/etc/pam.d/sudo_local` を削除 → 再起動

バックアップファイル `sudo_local.bak.<日時>` が `uninstall.sh` 実行前に必要な場合は
手動で戻すこともできる。

---

## 検証B — lock-probe（ロック判定 + Keychain + キー注入）

### B-0. 前提

- このプローブは**テスト文字列だけ**を扱う。**本物のログインパスワードを保存しないこと**
  （テストのため、ロック画面にはテスト文字列が注入される = 解除は失敗して当たり前。
  確認するのは「パスワード欄に ●●● が入ったか」「Returnが押されたか」）
- アクセシビリティ権限が必要なのは `type` と `demo` のみ

### B-1. ビルド

```sh
cd prototypes/lock-probe
swift build
alias lock-probe="$(pwd)/.build/debug/lock-probe"   # (好みで)
```

### B-2. セッション・ロック判定の確認（権限不要）

```sh
lock-probe status
# 期待値: CGSessionCopyCurrentDictionary() の中身 + isScreenLocked = false

lock-probe watch          # 別のままにしておく
# この状態で ⌃⌘Q (ロック) → 「🔒 LOCKED」、ログイン → 「🔓 unlocked」と
# 遷移ごとに表示されれば CGSSessionScreenIsLocked 判定は本機で機能している
```

### B-3. キー注入の単体確認（アクセシビリティ権限が必要）

```sh
lock-probe type hello
```

- 初回はアクセシビリティ権限が無いため案内が表示される:
  1. システム設定 → プライバシーとセキュリティ → アクセシビリティ
  2. 「ターミナル」(実行元アプリ) を追加してオン
  3. （バイナリ直で実行した場合は lock-probe 自身が並ぶ）
  4. ターミナルを完全に再起動 (⌘Q → 再起動) — TCC の反映に再起動が必要なことが多い
- 再実行すると 3 秒後、最前面のアプリに `hello` + Return が打たれる
  → **打たれれば合成キーイベントの注入は機能している**

### B-4. Keychain の確認

```sh
lock-probe keychain set test123
# 初回は「lock-probe/ターミナルがキーチェーンにアクセスしようとしています」
# ダイアログが出る → 「常に許可」
lock-probe keychain get
# 期待値: 取得: 「test123」
```

### B-5. 本番シーケンスのテスト（ロック→注入）

テスト文字列をKeychainに入れたまま:

```sh
lock-probe demo --delay 15
# 「15秒待機します」→ この間に ⌃⌘Q で画面をロックする
# ロック検出 → 2秒待ち → Keychainから文字列取得 → 注入 → 結果判定
```

**判定:**

| 結果 | 意味 |
|---|---|
| パスワード欄に ● が増えた / 文字が入力されたのが目視できた | **B-4 成立**。キー注入はロック画面でも機能している（テスト文字列なら解除失敗・シェイクは期待どおり） |
| パスワード欄が空のまま | 合成イベントがロック画面に届いていない（OSにブロックされている）→ 代替手段の検討が必要 |
| `✅ 画面ロックが解除されました` | 注入後10秒以内にアンロック状態になった。テスト文字列と不一致のはずなので、判定 window 内の手動アンロック（または失敗試行後の状態遷移）の可能性が高い。**B-4 の成否はこのメッセージではなく目視で判定する** |
| `❌ 10秒経っても解除されません` + ● 増加あり | 同じく B-4 成立（解除はテスト文字列不一致で失敗 = 正常） |

ロック画面の見た目で判定できない場合は、`--text` で実際に画面に現れない文字列を指定して
ロック画面に「表示される」失敗（パスワード不一致のシェイク）が起きるか見る。

### B-6. 成功基準

| # | 確認項目 | コマンド |
|---|---|---|
| B-1 | `CGSessionCopyCurrentDictionary` でロック状態が取れる | `watch` |
| B-2 | アクセシビリティ権限で合成キーが打てる | `type` |
| B-3 | Keychain から文字列を取得できる | `keychain get` |
| B-4 | **ロック画面に対してのみ**注入が行われる | `demo`（ロック中のみ注入） |
| B-5 | 誤爆防止: ロック中でなければ絶対に打たない | `demo` 中にロックせず待機 → 何も起きない |

### B-7. トラブルシュート

| 症状 | 対処 |
|---|---|
| `type` しても何も打たれない | ターミナルを再起動（TCC反映）。`tccutil reset Accessibility com.apple.Terminal` でリセットしてから付け直し |
| Keychainダイアログが毎回出る | 「常に許可」を選ぶ（「許可」は1回のみ）。プローブバイナリをビルドし直すたびに聞かれる場合は署名でバイナリが変化するため（プロトタイプとしては正常） |
| `demo` がロックを検出しない | SSHやGUIセッション外で実行していないか → `status` で辞書が取れるか確認 |
| 注入した文字が文字化けする | 報告されたい（CGEventKeyboardSetUnicodeString の上限20 UTF-16単位/イベントを超える分割処理は本番で入れる） |

---

## 実施結果の記録（実行したら埋める）

### 検証A — 実施日: 2026-11-24 / 結果: ✅ 全項目成立

- [x] A-1 sudo_local 経由でモジュールが呼ばれる → `/tmp/inmu_pam_dummy.log` に
      `service=sudo` の記録を確認（unified log でもモジュールがスタック先頭で
      dispatch されることを確認）
- [x] A-2 デフォルト (PAM_IGNORE) で sudo の挙動が無変更 → パスワードプロンプト・
      認証とも通常どおり動作
- [x] A-3 `--passwordless-demo` + arm 時にパスワードレス sudo が動作 →
      `sudo -k && sudo whoami` が**プロンプト無し**で `root` を返した。
      解除 (`sudo rm`) 後は即座にパスワードプロンプトへ復帰
- [x] A-4 アンインストールで完全に戻る → `sudo_local` 削除後、
      パスワード認証が復帰することを確認

過程で判明した OpenPAM / sudo の挙動（設計・手順に反映済み）:

- auth行1個だけのスタックでモジュールが `PAM_IGNORE` のみを返すと、OpenPAM の
  集計は `PAM_SUCCESS` になる（成否の区別が消える）→ inmutest には `pam_deny.so`
  の壁を配置して解決
- sudo はパスワード試行のたびに `pam_authenticate` がスタック全体を再実行する
  （モジュールは試行回数分呼ばれる → 本番 pam_inmu.so では waitMatch 要求の
  冪等性・キャンセル扱いに注意）
- `sufficient` + `PAM_SUCCESS` はスタックを短絡し、以降のモジュール
  （pam_smartcard / pam_opendirectory）は実行されない
- sudo は `PAM_SILENT` (0x80000000) を渡してくる
- zsh では `log` がシェル組み込みと衝突するため `/usr/bin/log` を使う

### 検証B — 実施日: 2026-11-24 / 結果: ✅ 全項目成立

- [x] B-1 ロック状態が取得できる → `status` でセッション辞書を取得、
      `watch` で ⌃⌘Q ロック→`🔒 LOCKED`、解除→`🔓 unlocked` の遷移を確認
- [x] B-2 合成キーが打てる → `type hello` で最前面アプリに入力
- [x] B-3 Keychain が読める → `keychain set/get`、および**ロック中の `demo` からの
      読み出しに成功**
- [x] B-4 **ロック画面への注入が機能する** → `demo` でロック画面のパスワード欄に
      テスト文字列 (`test123`) が入力されたことを目視確認
      （テスト文字列のためロック解除自体は失敗し、本パスワードで手動解除 — 期待どおり）
- [x] B-5 ロック中以外は注入されない → `demo` はロック検出まで注入せず待機した

**→ 画面ロック解除エージェントの設計前提 (CGSSessionScreenIsLocked 判定 +
ロック中の Keychain 読み出し + CGEventPost によるロック画面へのキー注入) は
macOS 27.0.1 (Apple Silicon) ですべて実証された。Phase 1 は Go。**

補足（観測された挙動・本番設計への反映）:

- `demo` の「✅ 画面ロックが解除されました」判定は「注入後10秒以内にアンロック状態に
  なった」ことを示す。テスト文字列と不一致のはずなので、判定 window 内に手動で
  本パスワードを打ったか、失敗試行後のロック状態遷移が一瞬 false として現れた可能性
  がある。**B-4 の成否はこの判定ではなく「パスワード欄に文字が入ったか」の目視確認で
  判定する**。本番 agent は注入後の成否をこのポーリングに依存しない設計とする
  （注入して終わり。失敗時のリトライは行わない = 誤爆防止）
- 誤ったパスワード注入後もロック画面はロックされたまま維持され、システムに影響なし。
  失敗試行はパスワード欄のシェイクのみ
