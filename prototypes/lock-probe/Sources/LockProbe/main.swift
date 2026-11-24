import ApplicationServices
import Foundation

// MARK: - ヘルプ

let usageText = """
lock-probe — 検証B: 画面ロック判定 + Keychain + キー入力エミュレート (プロトタイプ)

使い方:
  lock-probe status                     現在のセッションとロック状態を表示
  lock-probe watch [--interval 秒]     ロック状態の変化を表示し続ける (Ctrl-Cで終了)
  lock-probe keychain set [テキスト]    テスト文字列をKeychainに保存
  lock-probe keychain get               Keychainからテスト文字列を読み出して表示
  lock-probe type <テキスト>            <テキスト>を実際にタイピング (要アクセシビリティ権限)
  lock-probe demo [オプション]          delay秒待ち、ロックされたらKeychainの文字列を
                                        パスワード欄に打ち込んでReturnを押す

オプション:
  --interval 秒      ロック状態のポーリング間隔 (default 0.5)
  --delay 秒         demo開始までの待ち秒数 (default 10) — この間に ⌃⌘Q でロック
  --text テキスト    Keychainの代わりにこの文字列を打つ (テスト用)
  --no-return        Returnキーを押さない

検証Bの成功基標 (docs/04-testing-rollback.md 参照):
  1. status/watch でロック状態が正しく取れる
  2. demo でロック画面のパスワード欄に文字が入力される
"""

func failUsage(_ message: String? = nil) -> Never {
    if let message = message {
        FileHandle.standardError.write(Data(("エラー: \(message)\n\n").utf8))
    }
    FileHandle.standardError.write(Data((usageText + "\n").utf8))
    exit(64)
}

// MARK: - オプション解析

struct Options {
    var interval: Double = 0.5
    var delay: Double = 10
    var text: String?
    var pressReturn = true
}

/// "--key value" 形式の共通オプションを取り出し、残りを位置引数として返す
func parseOptions(_ args: [String]) -> (Options, [String]) {
    var opts = Options()
    var positional: [String] = []

    var i = 0
    while i < args.count {
        let arg = args[i]
        switch arg {
        case "--interval":
            i += 1
            guard i < args.count, let v = Double(args[i]) else { failUsage("--interval には数値が必要") }
            opts.interval = v
        case "--delay":
            i += 1
            guard i < args.count, let v = Double(args[i]) else { failUsage("--delay には数値が必要") }
            opts.delay = v
        case "--text":
            i += 1
            guard i < args.count else { failUsage("--text には値が必要") }
            opts.text = args[i]
        case "--no-return":
            opts.pressReturn = false
        default:
            positional.append(arg)
        }
        i += 1
    }
    return (opts, positional)
}

func lockStateString() -> String {
    SessionState.isScreenLocked ? "🔒 LOCKED" : "🔓 unlocked"
}

// MARK: - コマンド

func runStatus() {
    print("=== CGSessionCopyCurrentDictionary ===")
    print(SessionState.snapshotDescription())
    print("")
    print("isScreenLocked = \(SessionState.isScreenLocked)")
    if !AXIsProcessTrusted() {
        print("(アクセシビリティ権限なし — status/watch には不要)")
    }
}

func runWatch(opts: Options) {
    print("ロック状態を監視中… Ctrl-C で終了")
    print("現在: \(lockStateString())")
    var last: Bool? = nil
    let formatter = ISO8601DateFormatter()
    while true {
        let locked = SessionState.isScreenLocked
        if locked != last {
            print("\(formatter.string(from: Date())) \(locked ? "🔒 LOCKED" : "🔓 unlocked")")
            last = locked
        }
        Thread.sleep(forTimeInterval: opts.interval)
    }
}

func runKeychain(_ subcommand: String, rest: [String]) {
    let store = KeychainStore()
    switch subcommand {
    case "set":
        let secret: String
        if let arg = rest.first {
            secret = arg
        } else {
            print("保存するテスト文字列 (プロトタイプなので画面に表示されます): ", terminator: "")
            secret = readLine() ?? ""
        }
        guard !secret.isEmpty else { failUsage("空の文字列は保存できません") }
        do {
            try store.set(secret)
            print("保存しました (service=\(store.service), account=\(store.account))")
            print("※本物のログインパスワードは絶対に保存しないこと。テスト文字列のみ。")
        } catch {
            print("保存に失敗: \(error)")
            exit(1)
        }
    case "get":
        do {
            let secret = try store.get()
            print("取得: 「\(secret)」")
        } catch {
            print("読み出しに失敗: \(error)")
            exit(1)
        }
    case "delete":
        store.delete()
        print("削除しました")
    default:
        failUsage("keychain のサブコマンドは set / get / delete")
    }
}

func runType(_ text: String, opts: Options) {
    guard Accessibility.ensureTrusted(promptIfNeeded: true) else { exit(1) }
    print("3秒後に「\(text)」をタイピングします。最前面のアプリ (メモ帳など) にフォーカスしてください…")
    Thread.sleep(forTimeInterval: 3)
    Keyboard.type(text, pressReturn: opts.pressReturn)
    print("打ち込みました。入力されていますか?")
}

func runDemo(opts: Options) {
    guard Accessibility.ensureTrusted(promptIfNeeded: true) else { exit(1) }

    let store = KeychainStore()
    let secret: String
    if let text = opts.text {
        secret = text
    } else {
        do {
            secret = try store.get()
        } catch {
            print("Keychain からの読み出しに失敗: \(error)")
            print("先に `lock-probe keychain set <テスト文字列>` を実行するか、--text で指定してください")
            exit(1)
        }
    }

    print("=== 検証B demo ===")
    print("秘密文字列: 「\(secret)」 (\(opts.text != nil ? "--text" : "Keychain"))")
    print("\(Int(opts.delay))秒待機します。この間に ⌃⌘Q で画面をロックしてください…")
    Thread.sleep(forTimeInterval: opts.delay)
    print("ロック状態の監視を開始 (検出 → 2秒待ち → キー入力注入 → 結果判定)")

    var last: Bool? = nil
    while true {
        let locked = SessionState.isScreenLocked
        if locked != last {
            print("\(Date()) -> \(locked ? "🔒 LOCKED" : "🔓 unlocked")")
            last = locked
        }
        if locked {
            print("ロックを検出。2秒待ってから注入します…")
            Thread.sleep(forTimeInterval: 2)
            Keyboard.type(secret, pressReturn: opts.pressReturn)
            print("注入完了。結果を判定中…")
            // 10秒間ロックが解除されたか見守る
            for _ in 0..<20 {
                Thread.sleep(forTimeInterval: 0.5)
                if !SessionState.isScreenLocked {
                    print("✅ 画面ロックが解除されました — キー注入はロック画面でも機能します")
                    exit(0)
                }
            }
            print("❌ 10秒経っても解除されません:")
            print("   - 合成キーイベントがロック画面のパスワード欄に届いていない (OSによるブロック)")
            print("   - または注入された文字列が実際のパスワードと不一致")
            print("   判別法: パスワード欄の ● が増えていたか目視確認する")
            exit(1)
        }
        Thread.sleep(forTimeInterval: opts.interval)
    }
}

// MARK: - エントリポイント

let args = Array(CommandLine.arguments.dropFirst())
let (opts, positional) = parseOptions(args)

let command: String
if let first = positional.first {
    command = first
} else {
    failUsage("コマンドを指定してください")
}

switch command {
case "status":
    runStatus()
case "watch":
    runWatch(opts: opts)
case "keychain":
    guard let sub = positional.dropFirst().first else {
        failUsage("keychain のサブコマンドが必要 (set/get/delete)")
    }
    runKeychain(sub, rest: Array(positional.dropFirst(2)))
case "type":
    guard let text = positional.dropFirst().first else {
        failUsage("type にはテキストが必要")
    }
    runType(text, opts: opts)
case "demo":
    runDemo(opts: opts)
default:
    failUsage("不明なコマンド: \(command)")
}
