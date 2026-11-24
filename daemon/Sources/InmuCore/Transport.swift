import Foundation

// MARK: - トランスポート層 (USB I/O の抽象化)

/// FT9201 は bulk 転送で通信する (HID ではない)。Phase 1 では
/// IOKit (IOUSBHostInterface / IOUSBHostPipe) のユーザースペース実装を書く。
/// テスト用の MockTransport もこのプロトコルで差し替える。
public enum TransportEvent: Sendable {
    case connected
    case disconnected
    case error(String)
}

public protocol SensorTransport: AnyObject {
    /// 指定バイト数の書き込み (bulk out)
    func write(_ data: Data, timeout: TimeInterval) async throws
    /// 指定バイト数の読み込み (bulk in)。返り値は実際に読めたバイト列。
    func read(count: Int, timeout: TimeInterval) async throws -> Data
    /// クローズ (デバイス取り外し時にも呼ばれる)
    func close() async
    /// 接続状態の変化通知
    var events: AsyncStream<TransportEvent> { get }
}
