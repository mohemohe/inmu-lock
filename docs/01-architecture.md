# 01 — アーキテクチャ設計（言語選定・ディレクトリ構成・コンポーネント設計）

Phase 0（調査・プロトタイプ）時点の設計。実装の進展に合わせて更新する。

---

## 1. 全体像

immurok (IK-1) と同じ分離アーキテクチャを採る。macOS は認証面ごとに使える API が異なるため、
認証面ごとに適切なメカニズムを割り当てる。

```
                                   ┌────────────────────────────┐
                                   │  FT9201 指紋センサー (USB) │
                                   │  VID=2808 PID=93A9 (bulk)  │
                                   └─────────────┬──────────────┘
                                                 │ IOKit (user-space bulk)
┌────────────────────────────────────────────────┴───────────────┐
│                    Core Service (inmu-core)                    │
│  ・センサー制御(登録/照合)  ・テンプレート管理  ・Unix socket API │
│  LaunchDaemon (root) — docs/01 §4.1                           │
└───────┬────────────────────────────────────────────┬──────────┘
        │ Unix domain socket: pam.sock (root:0600)   │ agent.sock (console user)
        │                                            │ (matchイベントのpush購読)
┌───────┴──────────────┐                  ┌──────────┴─────────────────┐
│ PAM モジュール        │                  │ Screen Unlock Agent         │
│ pam_inmu.so (C)      │                  │ inmu-agent (Swift)          │
│ /usr/local/lib/pam/   │                  │ ・ロック判定(CGSession)      │
│                      │                  │ ・KeychainからログインPW取得 │
│ pam_sm_authenticate: │                  │ ・CGEventでキー注入+Return   │
│ waitMatch→成功/失敗   │                  │ LaunchAgent (GUIセッション) │
└───────┬──────────────┘                  └─────────────────────────────┘
        │ /etc/pam.d/sudo_local に1行
┌───────┴──────────────┐
│ sudo (PAMスタック)    │  auth sufficient pam_inmu.so
│ → 指紋成功ならPW不要   │  失敗/未接続なら PAM_IGNORE で従来のPWへ
└──────────────────────┘

対象外: FileVault解除(起動直後)・boot時のloginwindow — OS/デーモン未起動のため必ず手動入力
```

| 認証面 | 仕組み | 理由 |
|---|---|---|
| `sudo` | PAM (`pam_inmu.so`) | macOSの正規ルート。パスワードを保存・再入力しない |
| 画面ロック解除 | キーチェーン + キー注入 | サードパーティPAMはロック画面を解除できないため（OS制約） |

---

## 2. 言語選定

| コンポーネント | 採用言語 | 理由 |
|---|---|---|
| Core Service | **Swift** | IOKit (USB)・CoreGraphics (セッション状態)・Security (Keychain) など必要なシステムAPIがすべて第一級対応。async/await でイベント駆動が書きやすい。SwiftPMのみで完結 |
| PAM モジュール | **C** | PAMはC ABI (OpenPAM)。sudo の root コンテキストで Swift ランタイムをロードするリスクを回避。ソケット通信のみの thin module (Phase 1 で ~200行) なのでCで十分 |
| Unlock Agent | **Swift** | CGEvent / Keychain / CGSession がすべて Swift から直接扱える。Core Service とコード・ノウハウ共通化 |
| センサードライバ | Swift (+ 必要ならC) | USB user-space bulk は IOKit のC API を Swift から直接呼べる。Linux 実装 (C) の移植もC呼び出しが多いが Swift で吸収可能 |

**見送った選択肢と理由:**

- **Rust**: `hidapi` は HID デバイス用。FT9201 は vendor-specific bulk (bInterfaceClass=0xFF) で HID ではないため、結局 IOKit を叩く crates (保守性に難) が必要。セッション/Keychain API の binding も自作になる。唯一の利点は matcher 周りの移植性だが、Phase 1 の検証結果次第で必要になった時のみ検討
- **Go**: cgo 経由の IOKit/CG 呼び出しだけで手間が増え、バイナリも大きい。メリットなし
- **PAM モジュールを Swift 化**: Swift ランタイムのロードが sudo の厳格なコンテキスト (root, 場によっては restricted) で問題を起こす可能性があり、回避しておく価値が高い

---

## 3. ディレクトリ構成 (本番構成 — T4以降で実装済み)

```
inmu-lock/
├── README.md                       … 全体概要・クイックスタート
├── Makefile                        … make / vendor / enroll / install / uninstall
├── docs/
│   ├── 01-architecture.md          … 本書
│   ├── 02-sensor-interface.md      … センサー抽象化インターフェース設計
│   ├── 03-ft9201-notes.md          … センサー技術調査・macOS移植メモ
│   └── 04-testing-rollback.md      … 検証A/B 手順とロールバック
├── src/                            … 本番実装 (C)
│   ├── common/ipc.h                … IPC プロトコル (pam.sock / agent.sock)
│   ├── lib/                        … 検証C/D で実証した共通ライブラリ
│   │   ├── usb_transport.c         … IOKit USB (bulk / vendor control)
│   │   ├── ft9201.c                … FT9201 bring-up + 画像取得
│   │   ├── ft_engine_mac.c         … vendor matcher PE ローダー (Rosetta 2, LGPL-2.1)
│   │   └── ft9201_fw.h             … fw 配列 (make vendor が生成)
│   ├── cored/inmu-cored.c          … Core Service デーモン (root, x86_64)
│   ├── pam/pam_inmu.c              … PAM モジュール (universal)
│   ├── agent/inmu-agent.c          … Unlock Agent (universal)
│   └── enroll/inmu-enroll.c        … 登録ツール (x86_64)
├── scripts/
│   ├── fetch-vendor.sh             … 依存バイナリ取得 (DLL + fw)
│   ├── extract_firmware.py         … fw シンボル抽出
│   └── install.sh / uninstall.sh   … make install / uninstall の実体
├── launchd/
│   ├── com.mohemohe.inmu-lock.cored.plist   … LaunchDaemon
│   └── com.mohemohe.inmu-lock.agent.plist    … LaunchAgent
├── vendor/                         … 取得物 (git管理外)
├── daemon/                         … SwiftPM スケルトン (Swift化への参照実装)
└── prototypes/                     … 検証A〜D の実験記録 (実績の証跡)
```

**プロトタイプから本番への昇格の経緯**: `prototypes/` で検証した各要素
（検証A: PAM、B: ロック、C: PEローダー、D: USB/bring-up）は
`src/lib/` + `src/*/` に本番コードとして統合された。prototypes は
実績の証跡として残す。

---

## 4. コンポーネント詳細

### 4.1 Core Service (`inmu-core`, LaunchDaemon, root)

**責務:**
1. センサーの接続管理（取り外し・再接続への追従、ファームウェア転送）
2. 指紋登録・照合の実行（抽象化は docs/02）
3. 認証要求 (PAM)・認証イベント (Agent) の IPC 提供と ACL

**IPC設計（Phase 1）:**

- Unix domain socket 2本:
  - `/var/run/inmu/pam.sock` — モード `0600 root:wheel`。接続は `getpeereid()` で **uid==0 のみ**許可（PAMはsudo内部でroot実行）
  - `/var/run/inmu/agent.sock` — モード `0660 root:staff`。接続時に **uid==コンソールユーザー** のみ許可
- プロトコル: JSON lines (可読性・デバッグ性を優先)
  - PAM: `{"type":"waitMatch","timeoutSec":5}` → `{"type":"matchResult","ok":true,"templateID":1}` / `{"type":"matchResult","ok":false}` / タイムアウト
  - Agent: `{"type":"subscribe"}` → push `{"type":"matchSucceeded",...}`
- daemon未起動・センサー未接続の場合、PAMモジュールは**即座に** PAM_IGNORE を返す（パスワードへフォールバック）

**状態マシン:** `disconnected → idle ⇄ (enrolling | matching) → fault → 再接続待ち`

**ログ:** `os_log` (unified logging) に認証要求・結果・エラーを記録。**パスワード・テンプレートの生データは絶対に記録しない。**

### 4.2 PAM モジュール (`pam_inmu.so`, C)

- `pam_sm_authenticate`:
  1. `pam.sock` 接続（失敗 → `PAM_IGNORE` で即フォールバック）
  2. `waitMatch` 要求 (デフォルトタイムアウト 5秒)
  3. 照合成功 → `PAM_SUCCESS`／失敗・タイムアウト → `PAM_AUTH_ERR`／センサー未接続・daemon不在 → `PAM_IGNORE`
- 待機中は PAM conversation で「指紋認証中…」を表示（sudo が端末に表示してくれる）
- `pam_sm_setcred` は常に `PAM_SUCCESS`
- 配置: `/usr/local/lib/pam/pam_inmu.so`、設定は `/etc/pam.d/sudo_local` に1行:
  ```
  auth       sufficient     /usr/local/lib/pam/pam_inmu.so
  ```
  `sudo_local` はOSアップデートで消えない公式のローカル追加機構（Sonoma以降、本機 macOS 27 でも `auth include sudo_local` を `/etc/pam.d/sudo` で確認済み）
- 検証Aのダミーモジュールで、この呼び出し経路・返り値の効き方を先に安全に確認する

### 4.3 Screen Unlock Agent (`inmu-agent`, LaunchAgent, ユーザーGUIセッション)

**処理フロー:**
1. daemon の `agent.sock` を購読（matchイベントのpushを受ける）
2. matchイベント受信時、`CGSessionCopyCurrentDictionary()["CGSSessionScreenIsLocked"] == true` を**確認**
3. ロック中のみ、Keychain (Security framework) からログインパスワードを取得
   （保存は利用者が setup 時に1回行う。ACL は agent バイナリのみ許可）
4. `CGEventCreateKeyboardEvent` + `CGEventKeyboardSetUnicodeString` でパスワードを打ち、Return を注入 → ロック解除
5. 注入後、ロックが解除されたかを再度セッション辞書で確認（失敗時はリトライしない）

**誤爆防止（必須要件）:**
- **ロック判定を満たさない場合は何もしない** — デスクトップ操作中にセンサーに触れても、エディタ等に文字が打たれることはない（検証Bの`demo`がこのシーケンスの実測）
- イベントは daemon 経由のみ（センサーの物理イベントは agent から見えない）
- パスワードは注入の直前に取得し、使用後即破棄（プロセス内に常駐させない）
- Keychain の項目は `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`

**権限:** キー注入には TCC のアクセシビリティ権限が必要。本番は Developer ID 署名 + Hardened Runtime にし、TCC の帰属を agent バイナリに安定させる（検証Bではターミナル実行のためターミナルに権限を付与する）

### 4.4 セキュリティ設計

- **脅威モデル:** 同一マシン上の非特権プロセス・他のユーザー
- ソケットの peer 検証（`getpeereid()`）で「誰が要求できるか」を制限（§4.1）
- 認証済みの指紋 = 認証のファクタとしては「その指紋の持ち主がセンサーに触れた」ことのみを証明する。root 権限の付与は PAM の sufficient 行が担う
- **パスワード保存のトレードオフ:** 画面ロック解除は macOS の仕様上パスワード注入しか手段がない（Apple がサードパーティに開けていない）。immurok も同一方式。保存を望まない場合は画面ロック解除機能を無効化できる設計にする
- 強化 TODO: agent のコード署名検証（SecCode）、パスワード保存の専用キーチェーン化、daemon の Hardened Runtime

### 4.5 センサー取り外し・未接続時

- daemon は IOKit の通知で取り外しを検知、状態を `disconnected` に
- PAM は即 `PAM_IGNORE`（パスワード入力にフォールバック。sudoが壊れることはない）
- Agent は matchイベントが来ないだけで害なし

---

## 5. データフロー

### 5.1 `sudo` 実行時

```
user$ sudo apt ...
  └─ sudo (root) → PAMスタック → /etc/pam.d/sudo_local
       └─ pam_inmu.so (root)
            ├─ pam.sock 接続 → waitMatch(5s) 要求 ─┐
            │                                     │ inmu-core (root)
            │                                     ├─ センサー poll → 指検知
            │                                     ├─ 96×96 capture → 64×80 crop
            │                                     ├─ matcher 照合
            │  ← matchResult(ok) ←────────────────┘
            ├─ ok=true  → PAM_SUCCESS → sufficient → sudo 完了 (PW不要)
            └─ ok=false/timeout → PAM_AUTH_ERR → 次のモジュール (pam_opendirectory)
                                     → パスワードプロンプト (従来どおり)
```

### 5.2 画面ロック解除時

```
ユーザーがセンサーに指を置く
  └─ inmu-core: match成功 → agent.sock に push ──┐
                                                │ inmu-agent (GUIセッション)
                                                ├─ CGSSessionScreenIsLocked == true ?
                                                │    (false → 何もしない = 誤爆防止)
                                                ├─ Keychain からログインPW取得
                                                └─ CGEvent で PW + Return 注入 → ロック解除
```

---

## 6. 制約・前提（変更の可能性があるものは再検証する）

| 制約 | 内容 | 影響 |
|---|---|---|
| FileVault / boot ログイン | OS・デーモン起動前のため対象外 | 最初の1回は手動パスワード |
| `sudo_local` | Sonoma以降の公式機構（本機で確認済み） | OS更新で消えない |
| 画面ロック解除 = パスワード注入 | AppleがサードパーティPAMを認めないため（immurokと同じ回避策）→ **ロック画面のパスワード欄への合成キー注入を macOS 27.0.1 で実測成立（検証B B-4）** | パスワードをKeychainに保存する覚悟が必要 |
| `CGSSessionScreenIsLocked` | 非公式キー（実績は豊富、macOS 27 SDKでも当該API自体はdeprecatedなし）→ **macOS 27.0.1 で実測成立（検証B B-1）** | OS更新で壊れたら検証B相当の再検討 |
| センサーは Windows Hello 専用設計 | ホスト側で画像処理・照合が必要（docs/03） | matcher の調達が本体開発の鍵 |

---

## 7. Phase 1 実装計画（タスク分解）

| # | タスク | 内容 | 依存 |
|---|---|---|---|
| T1 | USB bulk トランスポート | ✅ 2026-11-24 成立 (検証D)。EP 0x02 OUT / 0x83 IN 実測、open/control/bulk 動作 | docs/03 |
| T2 | FT9201 bring-up | ✅ 2026-11-24 成立 (検証D)。fw 10,690バイト転送、MCU起動 (a5 5a)、96×96 実画像取得 | T1 |
| T3 | Matcher 選定 | ✅ 2026-11-24 成立 (検証C)。Rosetta 2 + PEローダー。実指紋で genuine 3/3 MATCH / imposter 3/3 reject | T2 |
| T4 | IPC サーバー | ✅ 2026-11-24 (src/cored)。pam.sock (0600) / agent.sock (0666)、行プロトコル、擬似クライアントで検証 | — |
| T5 | pam_inmu.so | ✅ 2026-11-24 (src/pam)。sudo 実機でパスワードレス sudo 成立 | T4 |
| T6 | unlock agent | ✅ 2026-11-24 (src/agent)。実ロック画面での解除成立 (誤爆防止確認済み) | T4, T3 |
| T7 | 統合テスト | ✅ 2026-11-24。実機3コンポーネント同時稼働で sudo 認証 + ロック解除を確認 | T1-T6 |
| — | 本番化 (CLI) | ✅ 2026-11-24: src/ 統合・make vendor/enroll/install/uninstall・launchd | T7 |
| — | 今後のTODO | USB断線時の自動再接続、Swift デーモン化 (docs/02)、パッケージ配布、多指対応 | — |
