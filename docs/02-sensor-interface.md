# 02 — センサー通信インターフェース設計（抽象化）

「センサーの通信プロトコルが後から差し替えられる」こと・「センサーが無くても上位層の
開発・テストが完結できる」ことを最優先にした4層設計。

実装は `daemon/Sources/InmuCore/` にあり、Phase 0 時点でビルド・実行可能
（`swift run InmuCoreDaemon demo` で Mock センサーによる動作確認ができる）。

---

## 1. 層構造

```
┌────────────────────────────────────────────────────────┐
│  PAM モジュール / Unlock Agent / CLI (enroll)           │  ← IPC (Unix socket)
├────────────────────────────────────────────────────────┤
│  FingerprintSensor (プロトコル)                          │  ← デーモンが見る面
│    ・connect / enroll / waitAndMatch / events           │
├───────────────────────┬────────────────────────────────┤
│  (実装の内側)          │                                │
│  SensorTransport      │  FingerprintMatcher            │  ← 差し替えポイント
│    ・USB bulk I/O     │    ・登録テンプレート生成        │
│    ・HID 版も取り替え可 │    ・1:1照合                    │
│    ・Mock 版           │    ・Mock 版                    │
├───────────────────────┴────────────────────────────────┤
│  FT9201 (実機) 96×96 → 64×80 crop → matcher            │
└────────────────────────────────────────────────────────┘
```

- **FingerprintSensor**: デーモン（SensorManager）が依存する唯一の面。実機・Mock を差し替える
- **SensorTransport**: USB I/O。bulk (FT9201) と HID、Mock を同じ面で扱う
- **FingerprintMatcher**: 照合アルゴリズム。Apple Silicon で vendor DLL がそのまま使えない問題
  （docs/03 §3）を、ドライバ本体から切り離して吸収する
- この分離により、**matcher だけを差し替えた実験**・**トランスポートだけを差し替えた実験**が独立にできる

---

## 2. プロトコル定義（実コード）

### 2.1 FingerprintSensor — `daemon/Sources/InmuCore/Sensor.swift`

```swift
public protocol FingerprintSensor: AnyObject {
    var info: SensorInfo { get }          // VID/PID/名前
    var state: SensorState { get }         // disconnected / idle / busy / fault

    func connect() async throws           // USBオープン・fw転送・初期化を含む
    func disconnect() async

    func enroll() async throws -> TemplateID   // 登録 (進捗はeventsに流れる)
    func deleteTemplate(id: TemplateID) async throws
    func listTemplates() async -> [TemplateID]

    func waitAndMatch(timeout: TimeInterval) async throws -> MatchResult
    // ↑ PAM からの要求1回に対応。「指が乗るのを待つ→照合→結果」の1サイクル

    var events: AsyncStream<SensorEvent> { get }
    // ↑ 常時ストリーム: fingerDetected / matchSucceeded / matchFailed /
    //    enrollmentProgress / deviceAttached / deviceDetached / stateChanged
    //    (Unlock Agent の購読に使う)
}
```

設計判断：

- `waitAndMatch(timeout:)` を独立させた → PAM の「この要求に応じて1回だけ待つ」セマンティクスに
  直接対応し、PAMモジュールはソケットの時間切れ処理だけを書けばよい
- 一方で `events` ストリームも持たせた → Agent は「常時待機」モードを同じドライバで実現できる
- 例外で失敗を伝える (`SensorError.timeout` 等) → C (PAM) 側での変換が単純
- VID/PID を `SensorInfo` に持たせる → 将来のデバイス対応表 (設定ファイル) にそのまま載る

### 2.2 SensorTransport — `daemon/Sources/InmuCore/Transport.swift`

```swift
public protocol SensorTransport: AnyObject {
    func write(_ data: Data, timeout: TimeInterval) async throws   // bulk out
    func read(count: Int, timeout: TimeInterval) async throws -> Data  // bulk in
    func close() async
    var events: AsyncStream<TransportEvent> { get }   // connected/disconnected/error
}
```

- FT9201 は HID ではなく **vendor-specific bulk (bInterfaceClass=0xFF, 2 endpoints)** —
  本機での実測は docs/03 §2
- Phase 1 で `UsbBulkTransport` (IOKit user-space) を実装する。libusb (Homebrew) を
  先に使ってプロトコルを確定させ、その後 IOKit 直呼びに置き換える選択肢もある
- 「FT9201 以外のセンサーに差し替える」場合はここを実装するだけでよい（HID なら `HidTransport`）

### 2.3 FingerprintMatcher — `daemon/Sources/InmuCore/Matcher.swift`

```swift
public protocol FingerprintMatcher: AnyObject {
    var name: String { get }
    func beginEnrollment() -> EnrollmentSession
    func addCapture(_ image: FingerprintImage, to session: EnrollmentSession) throws -> Bool
        // true = 登録に必要なキャプチャ数に達した
    func finishEnrollment(_ session: EnrollmentSession, id: TemplateID) throws -> FingerprintTemplate
    func verify(_ image: FingerprintImage, against templates: [FingerprintTemplate]) throws -> MatchResult
}
```

- `FingerprintImage` は 8bitグレースケール (`width/height/gray`)。FT9201 は 96×96 を撮り
  **中央 64×80 に crop** してから matcher に渡す（docs/03 §4。拡大縮小はしない）
- `FingerprintTemplate.data` は不透明バイト列 = matcher の実装依存。
  matcher を差し替えたら登録済みテンプレートは作り直す（仕様として明記）
- match-on-device 型のセンサー（内部照合）に変える場合は `FingerprintSensor` 実装の中に
  照合を閉じ込め、`FingerprintMatcher` を使わない — この層構造なら上位に影響しない

---

## 3. Mock実装による全面テスト

`MockFingerprintSensor` はプロトコルをフル実装するテスト用ドライバ：

```
$ cd daemon && swift run InmuCoreDaemon demo
=== InmuCore デーモンスケルトン (Mockセンサー) ===
-- connect() / enroll() / waitAndMatch(timeout: 5s) / タイムアウト / listTemplates()
完了: すべてのインターフェースが期待どおり動作しました ✅
```

Phase 1 ではこの Mock に対して IPC・PAM・Agent を開発し、実機ドライバは
最後に差し替える（実機のファーム/プロトコル開発と並行して進められる）:

| テスト対象 | センサー |
|---|---|
| PAM: waitMatch 要求・タイムアウト・フォールバック | Mock (matchDelayで即答) |
| Agent: matchイベント購読・ロック判定連動 | Mock (events) |
| 登録フロー・テンプレート管理 | Mock (3キャプチャ) |
| USB実装・プロトコル・精度 | 実機 FT9201 |

---

## 4. 実機 (FT9201) 実装のときに足すもの

Phase 1 で `daemon/Sources/InmuCore/` に追加する構成：

```
Usb/
  └── UsbBulkTransport.swift        … SensorTransport 実装 (IOKit IOUSBLib user client)
      - IOServiceMatching で VID=0x2808/PID=0x93A9 の IOUSBHostInterface を検索
      - 取り外し通知 (IOServiceAddInterestNotification) → TransportEvent.disconnected
Ft9201/
  ├── Ft9201Sensor.swift            … FingerprintSensor 実装
  │    connect = [fw転送 → AFE/電源レジスタ → reset → status 0xa5 0x5a 待ち]
  │    waitAndMatch = [指 poll → 96×96 capture → 64×80 crop → matcher.verify]
  ├── Ft9201Command.swift           … コマンド/レジスタ定数 (Linux実装から移植)
  └── Ft9201Firmware.swift          … fw blob (取得方法は docs/03 §5)
Matching/
  └── VendorMatcherClient.swift     … matcher ヘルパー (x86_64 プロセス, Rosetta 2) への
      Unix socket クライアント。ヘルパーは検証C の engine_test を発展させた
      ft_engine_mac (LGPL-2.1) ベースで vendor DLL を実行する
      (検証C 2026-11-24 成立 — docs/03 §3-a, prototypes/pe-loader-test/)
```

Matcher 選定の判断基準（詳細は docs/03 §3）:

| 候補 | 判定基準 |
|---|---|
| Rosetta2 + vendor DLL (PE ローダー移植) | **✅ 2026-11-24 検証C で成立**。精度は Windows 同等のはず (実画像での genuine 5/5 + imposter 0/5 は T2 のセンサー画像で確認) |
| SourceAFIS 等のオープン実装 | フォールバック用に格下げ。vendor matcher に問題が出た場合の検討 |

---

## 5. 他センサーへの差し替え手順（将来向けメモ）

1. `SensorInfo` (VID/PID) を持つ `FingerprintSensor` 実装を書く
   （内蔵照合型なら Transport+Matcher は不要なこともある）
2. VID/PID → 実装 の対応表を daemon の設定に追加
3. 既存の IPC・PAM・Agent は無変更（`FingerprintSensor` プロトコルが境界）
