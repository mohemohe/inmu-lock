# 検証A — ダミーPAMモジュール (pam_inmu_dummy)

`/etc/pam.d/sudo_local` 経由で sudo から PAM モジュールが呼ばれること、
返り値ごとの sudo の挙動、そしてロールバックを安全に確認するためのプロトタイプ。
実機センサーやデーモンとの通信は含まない（Phase 1 で `pam_inmu.so` として実装）。

## モジュールの挙動

`pam_sm_authenticate` は呼び出しを `/tmp/inmu_pam_dummy.log` と syslog に記録した上で:

| 条件 | 返り値 | sudo上の効果 |
|---|---|---|
| デフォルト | `PAM_IGNORE` | 何も起きない（パスワードプロンプトへ） |
| `demo_success` オプション + root所有の `/tmp/inmu_pam_demo_success` あり | `PAM_SUCCESS` | `sufficient` なのでパスワード無しで通る |
| `fail` オプション | `PAM_AUTH_ERR` | パスワードプロンプトへ |
| `wait=N` オプション | N秒待ってから上記判定 | タイムアウトUXの確認 |

オプションは `/etc/pam.d/*` のモジュール行末に付ける:
`auth sufficient /usr/local/lib/pam/pam_inmu_dummy.so debug demo_success`

## 構成

```
Makefile                    make / make check / make clean
src/pam_inmu_dummy.c        モジュール本体 (-dynamiclib)
src/pam_harness.c           単体テストハーネス (root不要, /etc/pam.d/inmutest 使用)
install.sh                  ビルド + /usr/local/lib/pam 配置 + inmutest 作成
                            + sudo_local 組み込み (バックアップ付き)
uninstall.sh                ロールバック (sudo_local の当該行のみ除去)
```

### テスト用PAMサービス (`/etc/pam.d/inmutest`) のスタック

```
auth      sufficient  pam_inmu_dummy.so debug demo_success
auth      required    pam_deny.so
account   required    pam_permit.so
```

実運用の sudo スタック（`sufficient` + フォールバック）と同じ形。
auth行1個だけのスタックでモジュールが `PAM_IGNORE` を返すと OpenPAM の集計が
`PAM_SUCCESS` になって成否の区別がつかないため、`pam_deny.so` の壁を置いている
（OpenPAM/macOS 27 での実測に基づく。docs/04 参照）。

ハーネスはスタックの結果（`pam_authenticate`）に加えて、モジュール自身の判定
（`/tmp/inmu_pam_dummy.log` の今回実行分）を表示し、exit 0/1 を `pam_authenticate`
だけで決める。

## テスト手順

**安全のため、別ターミナルで `sudo -s` した root シェルを開いたままテストすること。**

```sh
./install.sh               # 1. インストール (デフォルト: sudoの挙動は無変更)

./pam_harness inmutest     # 2. 単体: exit 1 (PAM_IGNORE → pam_deny が失敗させる)
sudo touch /tmp/inmu_pam_demo_success
./pam_harness inmutest     # 3. 単体: exit 0 (PAM_SUCCESS → sufficient が短絡)
sudo rm /tmp/inmu_pam_demo_success

sudo -k && sudo whoami     # 4. 組込: パスワードを聞かれる(挙動無変更)
tail /tmp/inmu_pam_dummy.log   #    ログに service=sudo の記録があればOK

./install.sh --passwordless-demo   # 5. sudo_local の行に demo_success を追加
sudo touch /tmp/inmu_pam_demo_success
sudo -k && sudo whoami     # 6. 組込: パスワード無しで "root" が返る
sudo rm /tmp/inmu_pam_demo_success   # ← 必ず解除
sudo -k && sudo whoami     # 7. 再びパスワードプロンプトに戻る

./uninstall.sh             # 8. ロールバック
sudo -k && sudo whoami     #    パスワードを聞かれれば完全復旧
```

※デフォルトのインストールのままで手順5〜6を行うと、モジュールは常に
`PAM_IGNORE` を返すため `Password:` プロンプトが出る。ここで**何も入力せず Enter を
押す**と OpenDirectory が空パスワードを拒否し、3回で `sudo: 3 incorrect password
attempts` になる（システムには影響しない。正しいパスワードを打てば通る）。

詳細な手順・期待値・ログ確認コマンドは
[docs/04-testing-rollback.md](../../docs/04-testing-rollback.md) 検証A。

## ロールバック

- `./uninstall.sh` で `sudo_local` から当該行だけを除去（実質空ならファイル削除）。
  `/etc/pam.d/inmutest` とモジュールも削除
- `install.sh` は既存 `sudo_local` を `sudo_local.bak.<日時>` にバックアップしてから編集
- 万一 sudo が壊れた場合: root シェルから `uninstall.sh` → 無ければ別管理者ユーザーで
  `sudo_local` を削除 → 最悪はリカバリーモード (⌘R) で `/etc/pam.d/sudo_local` を削除

## 注意

- `demo_success` は**root所有のファイル**がトリガーなので、一般ユーザーには arm できないが、
  **テスト後は必ず解除すること**（root が `touch` すれば誰でもパスワードレス sudo になる）
- テスト中の `sudo_local` の内容は最小構成 (デフォルト):
  ```
  # BEGIN inmu-lock pam-dummy (prototype)
  auth       sufficient     /usr/local/lib/pam/pam_inmu_dummy.so debug
  # END inmu-lock pam-dummy (prototype)
  ```
  `--passwordless-demo` でインストールした場合は行末に `demo_success` が付く。
