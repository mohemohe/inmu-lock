import Foundation
import InmuCore

// デーモンスケルトン (Phase 0):
// センサー抽象化インターフェースの動作をMockセンサーで確認する。
// Phase 1 で Unix domain socket サーバー (PAM/エージェントとのIPC) と
// FT9201 実機ドライバを実装する。

let sensor = MockFingerprintSensor()

let eventTask = Task {
    for await event in sensor.events {
        print("  event: \(event)")
    }
}

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data(("エラー: \(message)\n").utf8))
    exit(1)
}

let args = Array(CommandLine.arguments.dropFirst())
let command = args.first ?? "demo"

switch command {
case "demo":
    print("=== InmuCore デーモンスケルトン (Mockセンサー) ===")
    print("sensor: \(sensor.info)")

    print("\n-- connect()")
    try? await sensor.connect()
    guard sensor.state == .idle else { fail("接続に失敗") }

    print("\n-- enroll() (3タッチを模擬)")
    let templateID: TemplateID
    do {
        templateID = try await sensor.enroll()
    } catch {
        fail("登録に失敗: \(error)")
    }

    print("\n-- waitAndMatch(timeout: 5s) (PAMからの要求を模擬)")
    do {
        let match = try await sensor.waitAndMatch(timeout: 5)
        print("  認証成功: \(match)")
    } catch {
        fail("照合に失敗: \(error)")
    }

    print("\n-- waitAndMatch(timeout: 0.2s) (タイムアウトの模擬)")
    do {
        _ = try await sensor.waitAndMatch(timeout: 0.2)
        fail("タイムアウトになるはずが成功してしまった")
    } catch let error as SensorError {
        print("  期待どおりタイムアウト: \(error)")
    } catch {
        fail("予期しないエラー: \(error)")
    }

    print("\n-- listTemplates()")
    print("  templates: \(await sensor.listTemplates()) (登録ID: \(templateID))")

    print("\n-- disconnect()")
    await sensor.disconnect()
    eventTask.cancel()
    print("完了: すべてのインターフェースが期待どおり動作しました ✅")

default:
    FileHandle.standardError.write(Data(("不明なコマンド: \(command) ('demo' のみ)\n").utf8))
    exit(64)
}
