import Foundation
import Security

enum KeychainError: Error, CustomStringConvertible {
    case unexpectedStatus(OSStatus)
    case dataNotUTF8

    var description: String {
        switch self {
        case .unexpectedStatus(let status):
            let message = SecCopyErrorMessageString(status, nil) as String? ?? "unknown"
            return "Keychainエラー (OSStatus \(status)): \(message)"
        case .dataNotUTF8:
            return "Keychainエラー: 保存されたデータがUTF-8文字列ではありません"
        }
    }
}

/// テスト文字列を macOS Keychain (generic password) に保存/読み出しする最小実装。
///
/// 注意:
/// - プロトタイプなのでアクセス制御 (ACL) の設定は行わない。
///   読み出し時に「lock-probe (または実行元のターミナル) がキーチェーンに
///   アクセスしようとしています」ダイアログが出るので「常に許可」を選ぶこと。
/// - 本番 (unlock-agent) では専用キーチェーン + kSecAttrAccessible の厳格化 +
///   プロセスの署名によるACL固定を行う (docs/01-architecture.md 参照)。
struct KeychainStore {
    let service: String
    let account: String

    init(service: String = "com.mohemohe.inmu-lock.probe",
         account: String = "test-secret") {
        self.service = service
        self.account = account
    }

    /// 保存 (既存項目があれば上書き)。
    func set(_ secret: String) throws {
        let data = Data(secret.utf8)
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        // 冪等にするため先に削除
        SecItemDelete(base as CFDictionary)

        var add = base
        add[kSecValueData as String] = data
        // スクリーンロック中も (初回ログイン後なら) 読み出せるように
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock

        let status = SecItemAdd(add as CFDictionary, nil)
        guard status == errSecSuccess else {
            throw KeychainError.unexpectedStatus(status)
        }
    }

    /// 読み出し。
    func get() throws -> String {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess else {
            throw KeychainError.unexpectedStatus(status)
        }
        guard let data = result as? Data,
              let string = String(data: data, encoding: .utf8) else {
            throw KeychainError.dataNotUTF8
        }
        return string
    }

    /// 削除 (uninstall用)。
    func delete() {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        SecItemDelete(query as CFDictionary)
    }
}
