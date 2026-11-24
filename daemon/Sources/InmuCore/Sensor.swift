import Foundation

// MARK: - 共通型

public typealias TemplateID = Int

/// センサーの識別情報
public struct SensorInfo: Sendable, CustomStringConvertible {
    public let vendorID: Int
    public let productID: Int
    public let name: String

    public init(vendorID: Int, productID: Int, name: String) {
        self.vendorID = vendorID
        self.productID = productID
        self.name = name
    }

    public var description: String {
        String(format: "%@ (VID=0x%04X PID=0x%04X)", name, vendorID, productID)
    }
}

/// 照合結果
public struct MatchResult: Sendable, CustomStringConvertible {
    public let templateID: TemplateID
    public let score: Double

    public init(templateID: TemplateID, score: Double) {
        self.templateID = templateID
        self.score = score
    }

    public var description: String {
        String(format: "match(template=%d, score=%.3f)", templateID, score)
    }
}

/// センサーの状態
public enum SensorState: Sendable, Equatable {
    case disconnected
    case idle           // 待機・照合受付可能
    case busy           // 登録・キャプチャ処理中
    case fault(String)
}

public enum SensorError: Error, CustomStringConvertible {
    case notConnected
    case busy
    case timeout
    case enrollmentFailed(String)
    case transport(String)

    public var description: String {
        switch self {
        case .notConnected: return "センサーが未接続です"
        case .busy: return "センサーがビジー状態です"
        case .timeout: return "タイムアウトしました"
        case .enrollmentFailed(let reason): return "登録に失敗: \(reason)"
        case .transport(let reason): return "通信エラー: \(reason)"
        }
    }
}

/// センサーからのコールバックイベント。
/// PAM 側 (waitAndMatch) と画面ロック解除エージェント側 (eventsストリーム購読) の
/// 両方がこの情報を利用する。
public enum SensorEvent: Sendable, CustomStringConvertible {
    case deviceAttached
    case deviceDetached
    case fingerDetected
    case matchSucceeded(MatchResult)
    case matchFailed(reason: String)
    case enrollmentProgress(percent: Double)
    case enrollmentCompleted(templateID: TemplateID)
    case stateChanged(SensorState)

    public var description: String {
        switch self {
        case .deviceAttached: return "deviceAttached"
        case .deviceDetached: return "deviceDetached"
        case .fingerDetected: return "fingerDetected"
        case .matchSucceeded(let match): return "matchSucceeded(\(match))"
        case .matchFailed(let reason): return "matchFailed(\(reason))"
        case .enrollmentProgress(let p): return String(format: "enrollmentProgress(%.0f%%)", p * 100)
        case .enrollmentCompleted(let id): return "enrollmentCompleted(template=\(id))"
        case .stateChanged(let s): return "stateChanged(\(s))"
        }
    }
}

// MARK: - FingerprintSensor プロトコル (センサー抽象化の最上位)

/// 指紋センサードライバーの抽象インターフェース。
///
/// 設計意図 (docs/02-sensor-interface.md 参照):
/// - このプロトコルより下位に「Transport (USB I/O)」と「Matcher (照合アルゴリズム)」の
///   2つのプロトコルがあり、実機ドライバはそれらの組み合わせで実装する。
/// - デーモン (SensorManager) はこのプロトコルのみに依存し、
///   FT9201 と MockFingerprintSensor (テスト用) を差し替えられる。
/// - VID/PID によるデバイス検索・特定も実装側の責務とする。
public protocol FingerprintSensor: AnyObject {
    /// デバイスの識別情報
    var info: SensorInfo { get }
    /// 現在の状態
    var state: SensorState { get }

    /// 接続 (USBオープン・ファームウェア転送・初期化レジスタ書き込みなどを含む)
    func connect() async throws
    func disconnect() async

    /// 指紋の登録。複数回のタッチが必要なセンサーもあるため、
    /// 進捗は events に enrollmentProgress として流れる。
    func enroll() async throws -> TemplateID
    func deleteTemplate(id: TemplateID) async throws
    func listTemplates() async -> [TemplateID]

    /// 1回の照合を待つ (PAM モジュールからの要求で使う)。
    /// 指が乗って照合に成功すれば MatchResult を返す。
    /// タイムアウト・照合失敗・未接続は SensorError を投げる。
    func waitAndMatch(timeout: TimeInterval) async throws -> MatchResult

    /// 常時イベントストリーム (画面ロック解除エージェントが購読する)。
    /// 1コンシューマ想定。複数購読が必要になったら makeEvents() を足す。
    var events: AsyncStream<SensorEvent> { get }
}
