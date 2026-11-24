import ApplicationServices
import Foundation

/// アクセシビリティ (補助機能) 権限の判定・要求。
///
/// - CGEventPost / キー入力のエミュレートにはアクセシビリティ権限が必要。
/// - AXIsProcessTrustedWithOptions で System Settings の許可ダイアログを出せる。
/// - ターミナルから実行した場合、権限の帰属先が「実行元のターミナル.app」に
///   なることがある (TCC の responsible process)。README の手順参照。
enum Accessibility {
    static var isTrusted: Bool {
        AXIsProcessTrusted()
    }

    /// 未許可の場合は System Settings のプロンプトを表示し、案内を印字して false を返す。
    @discardableResult
    static func ensureTrusted(promptIfNeeded: Bool) -> Bool {
        if AXIsProcessTrusted() {
            return true
        }
        if promptIfNeeded {
            let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true]
                as CFDictionary
            _ = AXIsProcessTrustedWithOptions(options)
        }
        FileHandle.standardError.write(Data("""
        ⚠️  アクセシビリティ権限がありません。キー入力のエミュレートには権限が必要です。
          1. システム設定 → プライバシーとセキュリティ → アクセシビリティ
          2. 「lock-probe」または実行元の「ターミナル」を追加してオンにする
          3. 追加した場合はプロセスを再起動して再度実行する

        """.utf8))
        return false
    }
}
