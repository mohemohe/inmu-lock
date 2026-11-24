// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "lock-probe",
    platforms: [
        .macOS(.v13)
    ],
    targets: [
        .executableTarget(
            name: "lock-probe",
            path: "Sources/LockProbe"
        )
    ]
)
