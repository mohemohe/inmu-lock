import Foundation

// MARK: - 画像・照合 (Matcher) 層の抽象化

/// センサーから取り出した指紋画像 (8bitグレースケール)。
/// FT9201 は 96×96 を撮り、FocalTech のマッチングエンジンは
/// 中央 64×80 の切り抜きを期待する (docs/03-ft9201-notes.md 参照)。
public struct FingerprintImage: Sendable {
    public let width: Int
    public let height: Int
    public let gray: [UInt8]

    public init(width: Int, height: Int, gray: [UInt8]) {
        precondition(gray.count == width * height, "gray.count は width*height と一致すること")
        self.width = width
        self.height = height
        self.gray = gray
    }
}

/// 登録済みテンプレート (中身は Matcher の実装依存の不透明バイト列)
public struct FingerprintTemplate: Sendable {
    public let id: TemplateID
    public let data: [UInt8]

    public init(id: TemplateID, data: [UInt8]) {
        self.id = id
        self.data = data
    }
}

public enum MatcherError: Error, CustomStringConvertible {
    case enrollmentIncomplete
    case noMatch
    case internalError(String)

    public var description: String {
        switch self {
        case .enrollmentIncomplete: return "登録に必要なキャプチャ回数に達していません"
        case .noMatch: return "照合失敗 (どのテンプレートとも不一致)"
        case .internalError(let m): return "Matcher内部エラー: \(m)"
        }
    }
}

/// 照合アルゴリズムの抽象化。
///
/// macOS (Apple Silicon) では FocalTech の Windows 用マッチングエンジン
/// (x86_64 PE) をそのまま実行できないため、次の選択肢を差し替え可能にする:
///   1. VendorDLLMatcher — PEローダー + Rosetta 2 の実験的構成 (Phase 1で検証)
///   2. SourceAfisMatcher — オープンソースの分岐マッチング (精度要評価)
///   3. MockMatcher      — 常に成功するテスト用 (本ファイルの下)
///
/// センサーが内蔵マッチングを行うデバイスに差し替える場合は、
/// Matcher を実装せず FingerprintSensor 実装内で完結させる。
public protocol FingerprintMatcher: AnyObject {
    var name: String { get }

    /// 登録セッションを開始
    func beginEnrollment() -> EnrollmentSession
    /// キャプチャ画像を1枚追加。true = 登録に必要な回数に達した
    func addCapture(_ image: FingerprintImage, to session: EnrollmentSession) throws -> Bool
    /// 登録を確定しテンプレートを生成
    func finishEnrollment(_ session: EnrollmentSession, id: TemplateID) throws -> FingerprintTemplate
    /// 1枚の画像をテンプレート群と照合
    func verify(_ image: FingerprintImage, against templates: [FingerprintTemplate]) throws -> MatchResult
}

/// 登録セッション (複数枚キャプチャが必要なセンサー向け)
public final class EnrollmentSession {
    public internal(set) var captures: [FingerprintImage] = []

    public init() {}
}

// MARK: - MockMatcher (テスト用: 常に照合成功)

public final class MockMatcher: FingerprintMatcher {
    public let name = "MockMatcher (always-success)"

    public init() {}

    public func beginEnrollment() -> EnrollmentSession {
        EnrollmentSession()
    }

    public func addCapture(_ image: FingerprintImage, to session: EnrollmentSession) throws -> Bool {
        session.captures.append(image)
        return session.captures.count >= 3
    }

    public func finishEnrollment(_ session: EnrollmentSession, id: TemplateID) throws -> FingerprintTemplate {
        guard session.captures.count >= 3 else { throw MatcherError.enrollmentIncomplete }
        return FingerprintTemplate(id: id, data: [0x4D, 0x4F, 0x43, 0x4B])
    }

    public func verify(_ image: FingerprintImage, against templates: [FingerprintTemplate]) throws -> MatchResult {
        guard let first = templates.first else { throw MatcherError.noMatch }
        return MatchResult(templateID: first.id, score: 0.93)
    }
}
