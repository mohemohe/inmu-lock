import ApplicationServices
import CoreGraphics
import Foundation

/// キーボード入力のエミュレート (CGEventPost)。
///
/// - CGEventKeyboardSetUnicodeString で Unicode 直接注入するため、
///   キー配列 (日本語入力等) に依存せず文字を打てる。
/// - 修飾キーの処理は不要 (Unicode注入はキーコードの意味を持たない)。
/// - アクセシビリティ権限が必要 (Accessibility.ensureTrusted で判定)。
/// - kCGHIDEventTap に post する。
enum Keyboard {
    /// kVK_Return (Carbon HIToolbox の定義値)
    static let returnKeyCode: CGKeyCode = 0x24

    /// 文字列を打ち、必要なら Return を押す。
    static func type(_ text: String, pressReturn: Bool = true,
                     interKeyDelay: TimeInterval = 0.02) {
        for char in text {
            postUnicodeKey(String(char))
            Thread.sleep(forTimeInterval: interKeyDelay)
        }
        if pressReturn {
            Thread.sleep(forTimeInterval: interKeyDelay)
            postKey(returnKeyCode)
        }
    }

    /// 1文字をUnicode合成キーイベントとして投げる。
    private static func postUnicodeKey(_ string: String) {
        var chars = Array(string.utf16)
        if chars.isEmpty {
            chars = [0]
        }
        let down = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: true)
        down?.keyboardSetUnicodeString(stringLength: chars.count, unicodeString: &chars)
        down?.post(tap: .cghidEventTap)

        let up = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: false)
        up?.keyboardSetUnicodeString(stringLength: chars.count, unicodeString: &chars)
        up?.post(tap: .cghidEventTap)
    }

    /// 物理キー(キーコード)の押下。Return など制御キーに使う。
    private static func postKey(_ keyCode: CGKeyCode) {
        CGEvent(keyboardEventSource: nil, virtualKey: keyCode, keyDown: true)?
            .post(tap: .cghidEventTap)
        CGEvent(keyboardEventSource: nil, virtualKey: keyCode, keyDown: false)?
            .post(tap: .cghidEventTap)
    }
}
