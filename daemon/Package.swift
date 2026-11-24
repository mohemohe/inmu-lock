// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "InmuCore",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .library(name: "InmuCore", targets: ["InmuCore"]),
    ],
    targets: [
        // センサー抽象化・認証状態管理 (Phase 1 でIPC/USBを実装)
        .target(
            name: "InmuCore",
            path: "Sources/InmuCore"
        ),
        // デーモン本体 (現時点はMockセンサーによる動作確認デモ)
        .executableTarget(
            name: "InmuCoreDaemon",
            dependencies: ["InmuCore"],
            path: "Sources/InmuCoreDaemon"
        ),
    ]
)
