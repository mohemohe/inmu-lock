# 01 — アーキテクチャ設計（言語選定・ディレクトリ構成・コンポーネント設計）

§1〜§3・§6・§7 は Phase 0（調査・プロトタイプ）時点の設計と経緯の記録。
§4（コンポーネント詳細・セキュリティ設計・残存リスク）と §5（データフロー）は
現在の実装に合わせて更新してある。

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
│                   Core Service (inmu-cored)                    │
│  ・センサー制御/照合  ・承認ポリシー/ロックアウト  ・Unix socket │
│  LaunchDaemon (root) — docs/01 §4.1                           │
└───────┬────────────────────────────────────────────┬──────────┘
        │ pam.sock (0600 root)                       │ agent.sock (0600 登録オーナー)
        │ 相互に peer euid==0 を検証                  │ core: uid + 実行パスを検証
        │ WAIT <ms> <uid> → OK / NG / ERR            │ SUB → MATCH の push
┌───────┴──────────────┐                  ┌──────────┴─────────────────┐
│ PAM モジュール        │                  │ Screen Unlock Agent         │
│ pam_inmu.so (C)      │                  │ inmu-agent (C)              │
│ /usr/local/lib/pam/   │                  │ ・ロック判定(CGSession)      │
│                      │                  │ ・KeychainからログインPW取得 │
│ pam_sm_authenticate: │                  │ ・CGEventでキー注入+Return   │
│ WAIT → "OK" のみ成功  │                  │ LaunchAgent (GUIセッション) │
└───────┬──────────────┘                  └─────────────────────────────┘
        │ /etc/pam.d/sudo_local に1行
┌───────┴──────────────┐
│ sudo (PAMスタック)    │  auth sufficient pam_inmu.so
│ → 指紋成功ならPW不要   │  それ以外はすべて PAM_IGNORE で従来のPWへ
└──────────────────────┘

図の補足 (詳細は §4):
  ・MATCH 1 回で承認される sudo は 1 件だけ。登録オーナー本人の、コンソール GUI セッション内の、
    画面がロックされていない時の要求に限る (SSH 経由は不可)
  ・画面ロック中の MATCH は agent への通知にだけ使う (agent への通知は画面ロック中だけ)
  ・待機中の sudo が無く、画面もロックされていない時の接触は撮影・照合しない
  ・連続不一致 5 回でロックアウト (/var/db/inmu-lock に永続化)

対象外: FileVault解除(起動直後)・boot時のloginwindow — OS/デーモン未起動のため必ず手動入力
```

| 認証面 | 仕組み | 理由 |
|---|---|---|
| `sudo` | PAM (`pam_inmu.so`) | macOSの正規ルート。パスワードを保存・再入力しない |
| 画面ロック解除 | キーチェーン + キー注入 | サードパーティPAMはロック画面を解除できないため（OS制約） |

---

## 2. 言語選定

> Phase 0 時点の検討の記録。現在の本番実装は Core Service・Agent を含めすべて C
> (`src/`) で、Swift 版は `daemon/` の参照スケルトンのみ。

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
│   │   ├── ft_engine_mac.c         … vendor matcher PE ローダー (Rosetta 2, LGPL-2.1-or-later)
│   │   └── ft9201_fw.h             … fw 配列 (make vendor が生成)
│   ├── cored/inmu-cored.c          … Core Service デーモン (root, x86_64)
│   ├── pam/pam_inmu.c              … PAM モジュール (universal)
│   ├── agent/inmu-agent.c          … Unlock Agent (universal)
│   └── enroll/inmu-enroll.c        … 登録ツール (x86_64)
├── scripts/
│   ├── fetch-vendor.sh             … 依存バイナリ取得 (DLL + fw)
│   ├── extract_firmware.py         … fw シンボル抽出
│   ├── common.sh                   … 共有定数 (vendor の SHA-256)・実ユーザーの特定
│   ├── install.sh / uninstall.sh   … make install / uninstall の実体
│   └── daemon.sh / agent.sh        … launchd の起動・停止 (冪等)
├── launchd/
│   ├── dev.mohemohe.inmu-lock.cored.plist   … LaunchDaemon
│   └── dev.mohemohe.inmu-lock.agent.plist    … LaunchAgent
├── vendor/                         … 取得物 (git管理外)
├── daemon/                         … SwiftPM スケルトン (Swift化への参照実装)
└── tools/                          … 診断ツール (capture / engine / usb_probe)
```

**プロトタイプから本番への昇格の経緯**: `prototypes/` で検証した各要素
（検証A: PAM、B: ロック、C: PEローダー、D: USB/bring-up）は
`src/lib` + `src/*/` + `tools/` に本番コードとして統合された。
prototypes は作業ツリーから削除済みだが、**検証時のコードは git 初期コミット
(`1025ca1`) に永久保存**されている（docs/04 は当時の手順の記録）。

---

## 4. コンポーネント詳細

> この章は現在の実装 (`src/`) に合わせてある。プロトコルと共有定数の正は
> `src/common/ipc.h`。食い違いを見つけたら `ipc.h` とソースを信じること。
> 方針は一貫して **fail-closed**: 判断できないときは指紋を使わず、sudo は
> パスワード入力へフォールバックする (sudo 自体は壊さない)。

### 4.1 Core Service (`inmu-cored`, LaunchDaemon, root, x86_64/Rosetta 2)

**責務:**
1. センサーの bring-up (ファームウェア転送) と画像取得
2. vendor matcher DLL の in-process 実行による照合 (登録は `inmu-enroll`)
3. PAM 要求への MATCH の割り当てと、agent への MATCH 通知
4. 連続失敗のロックアウト (永続化)

**起動時に読むもの (どれも symlink 不可・root 所有を fd 経由で検証):**

| パス | 条件 | 満たさない場合 |
|---|---|---|
| `/usr/local/etc/inmu-lock/owner` | 通常ファイル、group/other 書き込み不可、内容は 10 進 uid 1 行 (uid 0 不可) | 起動はするが**一切承認しない** (PAM は即 NG、agent 接続は拒否、撮影もしない) |
| `/usr/local/etc/inmu-lock/template.bin` | 通常ファイル、group/other に 1 ビットも立っていない (0600)、1 バイト以上 1MB 以下 (エンジンが照合に使える上限 `FT_ENGINE_TEMPLATE_MAX`) | 起動中止 |
| `/usr/local/libexec/inmu-lock/ftWbioEngineAdapter.dll` | 通常ファイル、group/other 書き込み不可、**SHA-256 がソースにピン留めした値と一致** | 起動中止 |
| `/var/db/inmu-lock/` (0700) と `lockout` (0600) | ディレクトリは group/other 書き込み不可。ファイルは無ければ初期状態 | ディレクトリ不正は起動中止。ファイルが壊れている・読めない場合は 900 秒ロックアウト |
| `/var/run/inmu/` (0755) | group/other 書き込み不可 | 起動中止 |

登録オーナー (`owner`) は「この指紋は誰のものか」を表す uid。`inmu-enroll` がテンプレートと
一緒に書き、`install.sh` は無い場合だけ実行ユーザーの uid で作る。

**IPC (Unix domain socket、行ベーステキスト、1 行 128 バイト未満):**

| ソケット | モード / 所有者 | core が接続元に要求する条件 |
|---|---|---|
| `/var/run/inmu/pam.sock` | `0600` root | peer の euid == 0 (`getpeereid`) |
| `/var/run/inmu/agent.sock` | `0600` 登録オーナー | peer の euid == 登録オーナー uid、かつ peer の実行ファイルパス == `/usr/local/bin/inmu-agent` (`LOCAL_PEERTOKEN` → `proc_pidpath_audittoken`)。MATCH 送信の直前にも再検証 |

条件を満たさない接続は何も返さず切断する。ソケットは検証済みディレクトリの中に
umask で最初から 0600 で作る (bind 後に chmod する隙を作らない)。

agent.sock の実行ファイルパス検証は、誤った接続を減らすためのもので、確実な境界ではない
(§4.6 (d))。そのため MATCH 通知は秘密として扱わず、通知する場面そのものを画面ロック中に
限っている。agent の接続枠 (8) が埋まっている時は、最も古い接続を切断して新しい接続を
受け入れる (枠を埋められても正規の agent の再接続を締め出させない)。

```
PAM   → core :  "WAIT <timeout_ms> <uid>\n"   timeout_ms は 1..60000 に丸める。uid = PAM_USER の uid
core  → PAM  :  "OK\n"  その要求に MATCH を 1 件割り当てた
                "NG\n"  期限切れ / ポリシーにより拒否
                "ERR\n" プロトコル違反 / 受付不可
agent → core :  "SUB\n"
core  → agent:  "MATCH\n"
```

WAIT は 1 接続につき 1 回だけ、数字と空白 1 つずつの厳密な形式のみ受理する。接続後 5 秒
以内に WAIT / SUB が来なければ切断する。応答は 1 要求につき 1 行で、送ったら閉じる。

**承認ポリシー (MATCH 1 回につき):**

- **単一消費**: 承認する PAM 要求は「その撮影の開始より前に WAIT 済み」で期限内のもののうち、
  最も古い **1 件だけ**。同時に待っている他の要求はその MATCH では承認されない
  (自分の期限で NG になる)。撮影中に届いた WAIT もその MATCH では承認しない。
- **承認条件** (MATCH の時点ですべて満たすこと):
  1. WAIT の uid == 登録オーナー uid
  2. コンソール上の GUI セッションのユーザー == 登録オーナー uid
     (IORegistry ルートの `IOConsoleUsers`。on-console セッションがちょうど 1 つの場合のみ)
  3. 画面がロックされていない (`CGSSessionScreenIsLocked` / `IOConsoleLocked`)
  4. 要求元プロセス (sudo) の audit session id == コンソール GUI セッションの audit session id
     (`LOCAL_PEERTOKEN` → `audit_token_to_asid`)。SSH セッションや別セッションの中で
     実行した sudo は承認しない (同じ uid のプロセスが GUI セッション側で起動させた sudo は
     区別できない。§4.6 (b))
  
  情報を取得できない場合はすべて拒否。uid 不一致・オーナー不明・ロックアウト中、および
  要求元の audit session がコンソール GUI セッションと違うと分かっている場合 (SSH 等) は
  WAIT の時点で即 NG を返す (パスワード入力へすぐ進む)。WAIT の時点でコンソール情報を
  取得できなければ待機させ、MATCH の時点で改めて全条件を判定する。
- **画面ロック中の MATCH** は agent への通知 (ロック解除) にだけ使う。sudo は承認しない。
- **agent への通知**は、登録オーナーのコンソールセッションが画面ロック中の時だけ行う
  (コンソール情報を取得できなければ通知しない)。agent は MATCH の鮮度を判断できないので、
  非ロック時の一致 (sudo の承認など) を通知すると、読まれずに残った MATCH が後のロック解除に
  使われうるため。
- **使い道の無い接触は照合しない**: 指を検知しても、期限内の WAIT 済み PAM 要求があるか、
  agent へ通知できる状態 (SUB 済みの agent がいて、上の通知条件を満たす) のどちらかで
  なければ撮影も照合もしない。sudo もロック解除もしていない時の接触が不一致として
  ロックアウトに数えられることはない。
- **指の置きっぱなし**: 1 回撮影したら (一致・不一致・品質不足・失敗を問わず) 指が離れるまで
  次の撮影をしない。起動直後に指が置かれている場合も、一度離すまで撮影しない。
- **ロックアウト**: 連続不一致が 5 回に達すると 60 秒、以後 1 回ごとに倍、上限 900 秒。
  ロックアウト中は撮影・照合・承認・agent 通知をすべて行わない。カウンタは一致でのみ 0 に戻る
  (時間経過では減らない。品質不足と撮影失敗は数えない)。したがって閾値に達した後は、
  ロックアウトが明けても不一致 1 回ごとに再びロックアウトに入る。状態は
  `/var/db/inmu-lock/lockout` に保存し、デーモン再起動や USB の抜き差しでは消えない。
  状態ファイルには壁時計の解除時刻に加えて、起動セッション識別子
  (`sysctl kern.bootsessionuuid`) とモノトニック時計の解除時刻を保存する。デーモンの
  再起動時、保存時と同じ起動セッション (OS を再起動していない) ならモノトニック時計で
  残り時間を復元し、壁時計は見ない (時計を進めても戻しても残り時間は変わらない)。
  OS の再起動をまたいだ場合だけ壁時計で復元する (巻き戻しは検出してやり直す。進められた
  場合は短縮されうる。§4.6 (d))。
  **試行は照合の前に「不一致だった場合の状態」として保存し、保存できなければ照合しない**
  (一致したら 0 に戻して保存し直す)。照合の途中でデーモンが落ちた場合や保存できない
  状況でも、数えられないまま照合結果だけが得られることはない。

**ログ:** launchd 経由で `/var/log/inmu-lock/cored{,.err}.log` (ディレクトリ 0750、ファイル 0600、
plist の `Umask` = 077)。パスワード・テンプレート・画像の中身は記録しない。

**オプション:** 本番は `--template` と `--engine` のみ (launchd plist)。`--sockdir` /
`--state-dir` / `--owner-file` / `--agent-path` はテスト用。非 root で `--sockdir` と
`--state-dir` の両方を差し替えた場合だけテストモードになり、pam.sock の peer は自分の uid を
要求する (本物の sudo からは使えない)。不明な引数は終了コード 2。

### 4.2 PAM モジュール (`pam_inmu.so`, C, arm64 + x86_64)

`pam_sm_authenticate` の流れ:

1. `PAM_RHOST` が設定されていれば指紋を使わない (多層防御。SSH の遮断は core の audit
   session 判定が本体)。`PAM_USER` の uid を引けなければ使わない。
2. `pam.sock` が root 所有のソケットであることを `lstat` で確認して接続し、peer (core) の
   euid == 0 を `getpeereid` で検証する。
3. `WAIT <timeout_ms> <uid>` を送り、conversation で「指紋センサーに触れてください」を表示する。
   表示できなければ指紋を使わない。
4. 応答が **`OK\n` と完全一致** した場合だけ `PAM_SUCCESS`。

それ以外 (デーモン不在、NG、ERR、期限切れ、切断、長すぎる行、余分なデータ) はすべて
`PAM_IGNORE` を返し、次のモジュール (パスワード入力) へ進む。`PAM_AUTH_ERR` は返さない。

- モジュール引数: `timeout_ms=N` (1..60000、不正値は既定の 5000)、`sock=PATH`、`debug`
  (固定文言のみ出力。受信内容・ユーザー名は出さない)
- 読み取りは `poll` と単調時計で `timeout_ms + 500ms` まで。`SO_NOSIGPIPE` を使い、sudo の
  シグナル設定には触れない
- `pam_sm_setcred` は常に `PAM_SUCCESS`
- 配置: `/usr/local/lib/pam/pam_inmu.so`。設定は `/etc/pam.d/sudo_local` に 1 行
  (`install.sh` がマーカー付きで追加する):
  ```
  auth       sufficient     /usr/local/lib/pam/pam_inmu.so timeout_ms=5000
  ```
  `sudo_local` は OS アップデートで消えない公式のローカル追加機構 (Sonoma 以降)

### 4.3 Screen Unlock Agent (`inmu-agent`, C, LaunchAgent, ユーザー GUI セッション)

**処理フロー:**
1. `agent.sock` へ接続し、peer (core) の euid == 0 を検証してから `SUB` を送る。
   失敗・切断時は 2〜10 秒待って再接続する
2. `MATCH` 行 (完全一致のみ) を受けたら、`CGSessionCopyCurrentDictionary()` で
   **自セッションがコンソール上にあり (`kCGSessionOnConsoleKey`)、かつ
   `CGSSessionScreenIsLocked` が true** であることを確認する (どちらも CFBoolean 型で
   あること。キー無し・型違いは「ロックされていない」扱い = 何もしない)
3. ロック中のみ Keychain からログインパスワードを取得する
4. `CGEventKeyboardSetUnicodeString` でパスワードを 1 文字ずつ打ち、最後に Return を送る。
   **毎文字の前と Return の直前にロック状態を再確認**し、外れていたら即中止する
   (Return は送らない)

**誤爆・悪用の防止:**
- ロック判定を満たさない場合は何もしない (デスクトップ操作中にセンサーへ触れても、
  エディタ等にパスワードが打たれることはない)
- 注入を試みた後 5 秒間は次の MATCH を無視する。1 回の受信に MATCH が複数あっても 1 回だけ処理
- 正しい UTF-8 でないパスワード、制御文字を含むパスワードは 1 文字も送らず拒否する
  (`--set-password` の時点でも拒否)。非 ASCII・BMP 外の文字は UTF-16 に変換して送る
- パスワードは注入の直前に取得し、自前で確保した領域は `memset_s` で消去してから解放する
- `--set-password` は端末からのみ受け付ける (パイプ不可)。エコーを切れない場合は読まない

**署名と権限:**
- `make` が **Hardened Runtime 付きの ad-hoc 署名** (`codesign -s - --options runtime`) を
  行う。`DYLD_INSERT_LIBRARIES` 等によるコード注入を無効にするためで、`install.sh` は
  runtime フラグが無いバイナリをインストールしない。追加の entitlement は不要
- キー注入には TCC のアクセシビリティ権限が必要。ad-hoc 署名は**ビルドのたびに署名
  (cdhash) が変わる**ため、再インストール後はアクセシビリティの再許可と
  `--set-password` のやり直しが必要になる
- `/usr/local/bin/inmu-agent` 以外のパスから起動した agent は core に接続を拒否される

### 4.4 セキュリティ設計

**脅威モデル:** 同一マシン上の非特権プロセスと他のユーザー、および SSH 等のリモートセッション。
root を取った攻撃者と、物理的にセンサーをすり替えられる攻撃者は防御対象外 (§4.6)。

**指紋が証明すること:** 「登録された指がセンサーに触れた」ことだけ。そのタッチを
「どの要求の承認に使うか」は core が決める (§4.1 の承認ポリシー)。

| 対策 | 内容 |
|---|---|
| 要求元の束縛 | 承認は登録オーナー本人の、コンソール GUI セッション内からの、画面非ロック時の sudo に限る |
| 単一消費 | MATCH 1 回で承認する要求は 1 件。置きっぱなしでの再承認なし |
| 総当たり対策 | 連続不一致 5 回でロックアウト (60〜900 秒、永続) |
| 相互の peer 検証 | core は PAM (euid 0) と agent (uid + 実行ファイルパス) を、PAM と agent は core (euid 0) を検証 |
| コード注入対策 | agent は Hardened Runtime。インストール先ディレクトリはすべて root 所有・他者書き込み不可であることを `install.sh` が検証 |
| 供給元の固定 | vendor の .cab / .deb / DLL / 生成ヘッダを SHA-256 で固定 (`scripts/common.sh`)。DLL はロード直前にもメモリ上で照合し、照合したバイト列だけを実行する |
| ファイル権限 | テンプレート 0600 root、状態 0700/0600 root、ログ 0750/0600 root、ソケット 0600 |

**パスワード保存のトレードオフ:** 画面ロック解除は macOS の仕様上パスワード注入しか手段が
ない (Apple がサードパーティに開けていない。immurok も同一方式)。保存を望まない場合は
`inmu-agent --set-password` を実行しなければよい (sudo の指紋認証だけが有効になる)。

### 4.5 センサー取り外し・未接続時

- core は USB エラーで終了し、launchd の KeepAlive が再起動する (自動再接続は未実装)。
  センサーが無い間はソケットを作らない
- PAM はソケットに繋がらないので即 `PAM_IGNORE` (パスワード入力にフォールバック。sudo が
  壊れることはない)
- agent は再接続を待つだけで害なし
- 再起動してもロックアウト状態は引き継がれる

### 4.6 セキュリティ上の残存リスク

ソフトウェアの修正では解消できない、または今回解消していないもの。導入の判断材料として
正直に書く。**sudo と画面ロックのどちらも、最終的な強度はログインパスワードではなく
この一覧で決まる。**

**(a) センサーを認証できない**
- センサーは USB の VID/PID でしか識別できず、画像は平文で USB を流れる。センサーと
  ホストの間に暗号・署名は無い
- 生体検知 (liveness) も無い。偽造指、あるいは USB に挿した偽デバイスからの画像再生を
  本物と区別できない
- ロックアウトは「当てずっぽうの総当たり」を遅くするだけで、本人の指紋画像を入手した
  攻撃者には効かない。物理的にマシンへ触れられる相手に対しては、パスワードより弱い
- 偽デバイスが送る画像は、root で動くクローズドソースの matcher DLL に入力される。
  DLL 内の画像処理に欠陥があれば root でのコード実行につながりうる (検査していない)

**(b) 同一ユーザーのマルウェアによるタッチの便乗**
- 登録オーナーの権限で GUI セッション内に常駐するマルウェアは、自分で `sudo` を起動して
  待機させ、ユーザーが別の目的 (自分の sudo、ロック解除の習慣的なタッチ) でセンサーに
  触れるのを待てる
- 単一消費・画面ロック中の不承認・セッション束縛・5 秒の期限で機会は減らしたが、
  「いま何を承認しようとしているか」を示す信頼できる UI (Touch ID のシステムダイアログに
  相当するもの) は無い。**見覚えのない指紋プロンプトが出ている端末があるときは触れないこと**
- 同じ理由で、tmux / screen 等を GUI セッション内で動かしている場合、その中の sudo は
  承認対象になる
- **SSH について**: 遮断するのは「SSH セッション内で直接実行した sudo」(audit session が
  コンソール GUI セッションと異なる) だけ。同じ uid のプロセスの間に境界は無いので、
  登録オーナー本人の権限で SSH ログインできる相手は、GUI セッション側でコードを起動する
  手段 (`gui/<uid>` ドメインへの launchd ジョブ登録、`open` によるアプリ起動、GUI セッション
  内の tmux への接続など) を使って GUI セッションの audit session を持つ sudo を待機させ
  られる。したがってオーナーの SSH 資格情報を持つリモート攻撃者は、この (b) のマルウェアと
  同じ立場になる (承認に本人のタッチと画面非ロックが必要な点は変わらない)。この経路は
  コードでは塞げない

**(c) ログインパスワードを Keychain に保存する設計そのもの**
- 画面ロック解除を使う限り、ログインパスワードが login keychain (ファイルベース) に残る
- ad-hoc 署名のコマンドラインツールは data protection keychain を使えない (プロビジョニング
  プロファイルが必要)。そのため `kSecAttrAccessible` 指定は実効性が無く、項目の保護は
  keychain の ACL (アプリ単位の許可ダイアログ) に依存する
- 同一ユーザー権限のマルウェアは、ユーザーに許可ダイアログを承認させる・ロック解除済みの
  keychain を別経路で読む、といった攻撃を試みられる。これは Keychain 一般の性質で、
  このソフトでは防げない
- 取得したパスワードの複製は Security framework / CoreGraphics の内部にも一時的に残り、
  公開 API では消去できない (自前で確保した領域だけを消去している)

**(d) 個別の未解決事項**
- **ロック画面がユーザー名欄を表示する構成**: フォーカスがユーザー名欄にあると、パスワードが
  平文で表示され Return が送られる。確実に検出する方法が無いため未対策。ロック画面を
  「名前とパスワード」表示にしている場合は画面ロック解除機能を使わないこと
- **ロック確認から `CGEventPost` までのレース**: 確認と送出をアトミックにできない。ロック解除と
  同時に最大 1 文字 (最後の確認直後なら Return) が解除後の画面に届く可能性が残る
- **agent の同一性はパスと ad-hoc 署名まで**: Developer ID 署名ではないので、core は agent を
  「root 所有ディレクトリにある実行ファイルのパス」でしか識別できない
- **agent.sock の MATCH 通知は秘密ではない**: core が検証できるのは「そのソケットを最後に
  操作したプロセス」だけで、接続の fd を保持している全プロセスは検証できない。登録オーナーの
  権限で動くプロセスは、接続して `SUB` を送ってから fd を子プロセスに残して自分は
  `inmu-agent` を exec することで検証を通り、子プロセスで MATCH を受信できる。MATCH は承認では
  なく通知で、送るのは画面ロック中の一致だけなので、漏れるのは「画面ロック中に指紋が一致した
  時刻」にとどまる。同じ権限のプロセスは接続枠を取り合って正規 agent を切断させることも
  できる (ロック解除が効かなくなるだけ。同じ権限なら agent を止めることもできる)
- **停止中の agent に残る MATCH**: 画面ロック中に送った MATCH を agent がすぐ読まなかった場合
  (同一ユーザーのプロセスが agent を SIGSTOP で止めていた等)、後で読んだ時点で画面が
  ロックされていれば注入が走る。プロトコルに鮮度の情報が無いため未対策。非ロック時の一致は
  通知しないので、sudo の指紋承認がロック解除に流用されることはない
- **テンプレートは matcher DLL にとって信頼された入力**: DLL はテンプレートの中身を検証せず、
  壊れた内容を渡すと不正なポインタを参照して落ちることを確認している (承認側に倒れる経路は
  見つかっていない)。core が検査するのは所有者・モード (root 0600)・サイズだけで、内容の
  完全性は「`/usr/local/etc/inmu-lock` が root 所有で他者が書き込めないこと」に依存する。
  ファイルが壊れた場合は、照合時にデーモンが落ちて launchd に再起動される (照合中に落ちた
  試行は失敗として数えられる。パスワードは使える)。`make enroll` で登録し直すこと
- **`inmu-cored` / `inmu-enroll` は Hardened Runtime なし**: PE ローダーが実行可能マッピングを
  作れなくなるため付けていない。どちらも root (launchd / sudo) からしか起動しない
- **自作 PE ローダーと Win32 シム**: Windows 用 DLL を root プロセス内で実行している。DLL は
  SHA-256 で固定し、ローダーは境界検査とファジングを行ったが、形式的な保証は無い。
  DLL が致命エラーを起こした場合はプロセスを終了する (launchd が再起動)
- **ロックアウトによる妨害**: 1 回のロックアウトは最大 15 分だが、失敗カウンタは一致する
  まで減らない。センサーに触れられる人は、sudo の待機中や画面ロック中に登録していない指で
  触れることで指紋認証を止められ、ロックアウト明けに触り続ければ止め続けられる (パスワードは
  使える)。カウンタが溜まった状態では、本人の読み取り失敗 1 回でも最大 15 分のロックアウトに
  なる
- **時計を進めて OS を再起動するとロックアウトの残り時間を短縮できる**: モノトニック時計は
  OS の起動で 0 に戻るため、OS の再起動をまたいだ残り時間は壁時計から復元する。時計を
  進められる攻撃者 (管理者権限、または認証の無い時刻同期への介入) が OS の再起動と
  組み合わせると、残り時間を 0 にできる。失敗カウンタは残るので、得られるのは再起動
  1 回につき 1 回の試行 (次の不一致で再びロックアウト)。デーモンだけの再起動 (USB の
  抜き差しを含む) では短縮できない
- **`PAM_RHOST`**: sudoers で `pam_rhost` を有効にしている環境では指紋が常に無効になる
- **`/usr/local` が root 所有でない環境** (Intel Mac の Homebrew 等) では `make install` が中止する
- **実機で未確認のもの**: 今回のセキュリティ修正は、センサーと matcher をスタブにした
  デーモン・偽クライアント・単体ハーネスで確認した。root の LaunchDaemon としての動作、
  本物の sudo からの承認、SSH セッションの拒否、実際の画面ロック中の挙動、Hardened Runtime
  付き agent の Keychain 読み出しとキー注入、`install.sh` / `uninstall.sh` の通し実行は
  **未確認**。インストール後に README の「動作確認」を必ず行うこと

**(e) 既存インストールからの移行**

README の「既存インストールからの更新」を参照。要点: 再 `make install` が必要 (プロトコルが
変わったため新旧の混在は不可)、登録オーナーファイルが無いと core は一切承認しない、
agent は署名が変わるのでアクセシビリティの再許可と `--set-password` のやり直しが必要。

---

## 5. データフロー

### 5.1 `sudo` 実行時

```
user$ sudo ...
  └─ sudo (root) → PAMスタック → /etc/pam.d/sudo_local
       └─ pam_inmu.so (root)
            ├─ PAM_RHOST なし・PAM_USER の uid を取得
            ├─ pam.sock 接続 → peer(core) euid==0 を検証
            ├─ "WAIT 5000 <uid>" ───────────────┐
            │                                   │ inmu-cored (root)
            │                                   ├─ peer(sudo) euid==0 を検証
            │                                   ├─ uid != オーナー / ロックアウト中 /
            │                                   │    別の audit session (SSH 等) → 即 "NG"
            │                                   ├─ 指検知 → 撮影 → 試行を保存 → matcher 照合
            │                                   ├─ 不一致 → 失敗カウンタ +1 のまま (5 回でロックアウト)
            │                                   ├─ 一致 → 最も古い待機要求 1 件について
            │                                   │    コンソールユーザー・非ロック・audit session を判定
            │  ← "OK" / "NG" / "ERR" ←──────────┘
            ├─ "OK" と完全一致 → PAM_SUCCESS → sufficient → sudo 完了 (PW不要)
            └─ それ以外すべて   → PAM_IGNORE → 次のモジュール (pam_opendirectory)
                                     → パスワードプロンプト (従来どおり)
```

### 5.2 画面ロック解除時

```
ユーザーがセンサーに指を置く
  └─ inmu-cored: 画面ロック中で agent が購読中なので撮影・照合 → 一致
       │  (ロック中なので PAM は承認しない。非ロック時は agent に通知しない)
       └─ agent.sock の購読者を再検証して "MATCH" ──┐
                                                   │ inmu-agent (GUIセッション)
                                                   ├─ コンソール上 かつ CGSSessionScreenIsLocked ?
                                                   │    (否 → 何もしない = 誤爆防止)
                                                   ├─ Keychain からログインPW取得
                                                   ├─ 1 文字ごとにロック状態を再確認しながら注入
                                                   └─ ロック中のままなら Return → ロック解除
```

---

## 6. 制約・前提（変更の可能性があるものは再検証する）

| 制約 | 内容 | 影響 |
|---|---|---|
| FileVault / boot ログイン | OS・デーモン起動前のため対象外 | 最初の1回は手動パスワード |
| `sudo_local` | Sonoma以降の公式機構（本機で確認済み） | OS更新で消えない |
| 画面ロック解除 = パスワード注入 | AppleがサードパーティPAMを認めないため（immurokと同じ回避策）→ **ロック画面のパスワード欄への合成キー注入を macOS 27.0.1 で実測成立（検証B B-4）** | パスワードをKeychainに保存する覚悟が必要（§4.6 (c)） |
| `CGSSessionScreenIsLocked` | 非公式キー（実績は豊富、macOS 27 SDKでも当該API自体はdeprecatedなし）→ **macOS 27.0.1 で実測成立（検証B B-1）** | OS更新で壊れたら検証B相当の再検討 |
| センサーは Windows Hello 専用設計 | ホスト側で画像処理・照合が必要（docs/03） | matcher の調達が本体開発の鍵 |

---

## 7. Phase 1 実装計画（タスク分解）

| # | タスク | 内容 | 依存 |
|---|---|---|---|
| T1 | USB bulk トランスポート | ✅ 2026-11-24 成立 (検証D)。EP 0x02 OUT / 0x83 IN 実測、open/control/bulk 動作 | docs/03 |
| T2 | FT9201 bring-up | ✅ 2026-11-24 成立 (検証D)。fw 10,690バイト転送、MCU起動 (a5 5a)、96×96 実画像取得 | T1 |
| T3 | Matcher 選定 | ✅ 2026-11-24 成立 (検証C)。Rosetta 2 + PEローダー。実指紋で genuine 3/3 MATCH / imposter 3/3 reject | T2 |
| T4 | IPC サーバー | ✅ 2026-11-24 (src/cored)。pam.sock (0600) / agent.sock (当初 0666 → 現在は 0600 登録オーナー + peer 検証、§4.1)、行プロトコル、擬似クライアントで検証 | — |
| T5 | pam_inmu.so | ✅ 2026-11-24 (src/pam)。sudo 実機でパスワードレス sudo 成立 | T4 |
| T6 | unlock agent | ✅ 2026-11-24 (src/agent)。実ロック画面での解除成立 (誤爆防止確認済み) | T4, T3 |
| T7 | 統合テスト | ✅ 2026-11-24。実機3コンポーネント同時稼働で sudo 認証 + ロック解除を確認 | T1-T6 |
| — | 本番化 (CLI) | ✅ 2026-11-24: src/ 統合・make vendor/enroll/install/uninstall・launchd | T7 |
| — | 今後のTODO | USB断線時の自動再接続、Swift デーモン化 (docs/02)、パッケージ配布、多指対応 | — |
