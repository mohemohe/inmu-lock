# 検証B — lock-probe（画面ロック判定 + Keychain + キー入力注入）

画面ロック時のみ、Keychain から取得したテスト文字列を合成キーイベントで入力する——
本番の Screen Unlock Agent の核心シーケンスを先に実測するプローブ。

確認すること:

1. `CGSessionCopyCurrentDictionary()` の `CGSSessionScreenIsLocked` で
   **ロック状態を正確に判定できるか**（誤爆防止の要）
2. アクセシビリティ権限でキー入力をエミュレートできるか
3. **ロック画面のパスワード欄に合成キーが届くか**（macOS が認める範囲の実測）
4. ロック中以外は絶対に打たないことを確認

## ビルド

```sh
cd prototypes/lock-probe
swift build
alias lock-probe="$(pwd)/.build/debug/lock-probe"
```

## コマンド

```
lock-probe status                  セッション辞書とロック状態の表示
lock-probe watch                   ロック状態の変化を監視 (Ctrl-Cで終了)
lock-probe keychain set [テキスト]  テスト文字列をKeychainに保存
lock-probe keychain get            テスト文字列の読み出し
lock-probe type <テキスト>          <テキスト>を打つ (権限必要)
lock-probe demo [--delay N] [--text T] [--no-return]
                                   delay秒→ロック待ち→Keychainの文字列を注入→判定
```

## 手順（要約）

```sh
lock-probe status                  # 1. 辞書が取れるか
lock-probe watch                   # 2. ⌃⌘Q で LOCKED / 解除で unlocked と出るか
lock-probe type hello              # 3. 権限付与後、最前面アプリに打たれるか
lock-probe keychain set test123    # 4. 「常に許可」を選ぶ
lock-probe demo --delay 15         # 5. 15秒以内に ⌃⌘Q → ロック→注入→結果判定
```

`demo` の判定:

- `✅ ロック解除` … テスト文字列が実パスワードと一致した場合のみ
- `❌` + **パスワード欄に ● が入った** … **注入はロック画面でも機能している** → 実運用可能
- `❌` + パスワード欄が空 … OSがロック画面への合成イベントをブロック → 代替手段の検討が必要

詳細・期待値・トラブルシュートは [docs/04-testing-rollback.md](../../docs/04-testing-rollback.md) 検証B。

## アクセシビリティ権限（type / demo に必要）

1. 実行すると System Settings のプロンプトが出る（または手動で開く）
2. システム設定 → プライバシーとセキュリティ → アクセシビリティ
3. **「ターミナル」**（実行元アプリ）を追加してオン — ターミナルから実行する場合、
   TCC の帰属先がターミナルになる
4. ターミナルを**完全に再起動**してから再実行（TCC 反映のため）

## ⚠️ テスト文字列だけを扱うこと

このプローブはログに文字列を表示する（`demo` は確認のため打つ文字列を明示する）。
**本物のログインパスワードを保存しないこと。**
実運用の agent はログ非表示・専用キーチェーン・ACL限定を実装する
（[docs/01-architecture.md](../../docs/01-architecture.md) §4.3/4.4）。

## 実装メモ

- `SessionState.swift` — `CGSessionCopyCurrentDictionary()` は macOS 27 SDK でも
  deprecated なし。`CGSSessionScreenIsLocked` は非公式キー（実績のある定番手法）
- `Keyboard.swift` — `CGEventKeyboardSetUnicodeString`（Swift名:
  `keyboardSetUnicodeString(stringLength:unicodeString:)`）で Unicode 直接注入。
  キー配列に依存しない。1イベント20 UTF-16単位までのため1文字ずつ注入
- `KeychainStore.swift` — `SecItemAdd/CopyMatching`。`kSecAttrAccessibleAfterFirstUnlock`
  （初回ログイン後ならロック中も読める）
- `Accessibility.swift` — `AXIsProcessTrustedWithOptions` + `kAXTrustedCheckOptionPrompt`
