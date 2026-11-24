# InmuCore — Core Service スケルトン

指紋センサーとの通信・認証イベントを管理する常駐デーモンの SwiftPM スケルトン。
Phase 0 時点では**センサー抽象化インターフェース + Mock 実装**のみ。
設計の意図は [docs/02-sensor-interface.md](../docs/02-sensor-interface.md)。

## 動作確認

```sh
swift run InmuCoreDaemon demo
```

Mock センサーで connect → enroll (3キャプチャ) → waitAndMatch (成功) →
waitAndMatch (タイムアウト) → listTemplates → disconnect の全シーケンスが流れ、
events ストリームにfingerDetected / enrollmentProgress / matchSucceeded 等が流れる。

## 構成

```
Sources/InmuCore/              ライブラリ (センサー抽象化)
  Sensor.swift                 FingerprintSensor プロトコル + 共通型 (イベント・状態・エラー)
  Transport.swift              SensorTransport (USB I/O 抽象化: bulk/HID/Mock)
  Matcher.swift                FingerprintMatcher (照合アルゴリズム抽象化) + MockMatcher
  MockFingerprintSensor.swift  FingerprintSensor の Mock 実装
Sources/InmuCoreDaemon/        実行可能ターゲット (現在はdemoコマンドのみ)
```

## Phase 1 で追加するもの

- `Usb/UsbBulkTransport.swift` — IOKit user-space bulk (VID 2808/PID 93A9 の
  IOUSBHostInterface を開く。本機で「HID未登録・Appleドライバ未アタッチ」を確認済み)
- `Ft9201/` — fw 転送・初期化レジスタ・96×96キャプチャ・64×80 crop
  （プロトコルは Linux 実装から移植 — docs/03-ft9201-notes.md）
- `Ipc/` — Unix domain socket (pam.sock root限定 / agent.sock コンソールユーザー限定)
- `Matching/` — matcher 実装（Rosetta2+vendor DLL 実験 or SourceAFIS 精度評価）

タスク分解は [docs/01-architecture.md](../docs/01-architecture.md) §7。
