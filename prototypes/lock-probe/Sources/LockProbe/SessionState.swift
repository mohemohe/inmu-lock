import CoreGraphics
import Foundation

/// CGSessionCopyCurrentDictionary() による画面ロック状態の判定 (検証Bの核心)。
///
/// - CGSessionCopyCurrentDictionary は macOS 10.3 以降で利用可能 (macOS 27 SDKでも deprecated なし)。
/// - "CGSSessionScreenIsLocked" は公開ヘッダに載っていない非公式キーだが、
///   数多くのロック検出ツール・immurok 等の実績がある定番の方法。
/// - SSH や GUI セッション外で実行した場合は nil が返るため false 扱いにする。
enum SessionState {
    /// ロック判定。辞書が取得できない (SSH等) の場合は false を返す。
    static var isScreenLocked: Bool {
        guard let dict = CGSessionCopyCurrentDictionary() as? [String: Any] else { return false }
        // CFBoolean は NSNumber に toll-free bridge される
        guard let value = dict["CGSSessionScreenIsLocked"] as? NSNumber else { return false }
        return value.boolValue
    }

    /// デバッグ表示用: セッション辞書の中身をそのまま表示する。
    static func snapshotDescription() -> String {
        guard let dict = CGSessionCopyCurrentDictionary() as? [String: Any] else {
            return "CGSessionCopyCurrentDictionary() = nil (GUIセッション外、またはWindowServer非接続の可能性)"
        }
        var lines: [String] = ["CGSessionCopyCurrentDictionary() の内容:"]
        for key in dict.keys.sorted() {
            lines.append("  \(key) = \(dict[key] ?? "nil")")
        }
        return lines.joined(separator: "\n")
    }
}
