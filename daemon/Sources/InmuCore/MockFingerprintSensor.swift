import Foundation

/// FingerprintSensor プロトコルのテスト用モック実装。
///
/// 実機 (FT9201) が無くても以下が検証できる:
/// - デーモン・PAM・エージェントのシーケンス
/// - waitAndMatch のタイムアウト処理
/// - events ストリームの購読
///
/// 挙動:
/// - connect(): 0.3秒で接続完了扱い
/// - waitAndMatch(): 指定遅延後に (デフォルトで) 照合成功イベントを返す
/// - enroll(): 3回の「タッチ」を模擬しテンプレートを1つ登録
public final class MockFingerprintSensor: FingerprintSensor {
    public let info = SensorInfo(vendorID: 0x2808, productID: 0x93A9,
                                 name: "Mock Fingerprint Sensor")

    private let lock = NSLock()
    private var _state: SensorState = .disconnected
    public var state: SensorState {
        lock.lock(); defer { lock.unlock() }
        return _state
    }
    private func setState(_ new: SensorState) {
        lock.lock(); defer { lock.unlock() }
        _state = new
        continuation.yield(.stateChanged(new))
    }

    public let events: AsyncStream<SensorEvent>
    private let continuation: AsyncStream<SensorEvent>.Continuation

    /// 照合に要する模擬時間 (秒)
    private let matchDelay: TimeInterval
    private var templates: [TemplateID] = []
    private var nextTemplateID: TemplateID = 1

    public init(matchDelay: TimeInterval = 1.0) {
        self.matchDelay = matchDelay
        (events, continuation) = AsyncStream.makeStream(of: SensorEvent.self,
                                                        bufferingPolicy: .bufferingNewest(64))
    }

    public func connect() async throws {
        print("  [mock] connect()")
        try await Task.sleep(nanoseconds: 300_000_000) // USB初期化の模擬
        setState(.idle)
        continuation.yield(.deviceAttached)
    }

    public func disconnect() async {
        print("  [mock] disconnect()")
        setState(.disconnected)
        continuation.yield(.deviceDetached)
    }

    public func enroll() async throws -> TemplateID {
        guard state == .idle else { throw SensorError.notConnected }
        setState(.busy)
        defer { setState(.idle) }

        let id = nextTemplateID
        nextTemplateID += 1
        for step in 1...3 {
            try await Task.sleep(nanoseconds: 400_000_000) // タッチ待ちの模擬
            continuation.yield(.fingerDetected)
            continuation.yield(.enrollmentProgress(percent: Double(step) / 3.0))
        }
        templates.append(id)
        continuation.yield(.enrollmentCompleted(templateID: id))
        print("  [mock] enroll() -> template \(id)")
        return id
    }

    public func deleteTemplate(id: TemplateID) async throws {
        templates.removeAll { $0 == id }
    }

    public func listTemplates() async -> [TemplateID] {
        templates
    }

    public func waitAndMatch(timeout: TimeInterval) async throws -> MatchResult {
        guard state == .idle else { throw SensorError.notConnected }
        setState(.busy)
        defer { setState(.idle) }

        print("  [mock] waitAndMatch(timeout: \(timeout)s) — 指が乗るのを待ちます…")
        try await Task.sleep(nanoseconds: UInt64(min(matchDelay, timeout) * 1_000_000_000))
        continuation.yield(.fingerDetected)

        guard matchDelay <= timeout else {
            continuation.yield(.matchFailed(reason: "timeout"))
            throw SensorError.timeout
        }
        guard let template = templates.first else {
            continuation.yield(.matchFailed(reason: "登録済みテンプレートがありません"))
            throw SensorError.enrollmentFailed("no enrolled templates")
        }
        let result = MatchResult(templateID: template, score: 0.93)
        continuation.yield(.matchSucceeded(result))
        print("  [mock] waitAndMatch -> \(result)")
        return result
    }
}
