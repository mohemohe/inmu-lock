# 検証D — IOKit USB bulk トランスポート + FT9201 bring-up (T1/T2)

FT9201 (VID 2808:93A9) を IOKit ユーザースペース API (IOUSBLib) で扱う
トランスポート、**fw 転送による bring-up、96×96 画像取得**の実装と実機検証。

## 結果: ✅ 成立 (2026-11-24, macOS 27.0.1 / Apple Silicon)

### T1: トランスポート (実機で確定)

| 項目 | 実測値 |
|---|---|
| bulk OUT | **EP 0x02** (pipeRef 1), maxPacketSize=16 |
| bulk IN | **EP 0x83** (pipeRef 2), maxPacketSize=32 |
| interface 0 | class/subclass/protocol = 0xFF/0xFF/0xFF, alt 0, 2 endpoints |
| string[1] / string[2] | "Focal-systems.Corp" / "FT9201Fingerprint.̚" |

- サービス列挙・USBDeviceOpen/USBInterfaceOpen・標準 control request・
  bulk 転送の受理 (未セットアップで kIOUSBTransactionTimeout = 想定値) すべて
  **エンタイトルメント不要**で動作

### T2: bring-up (3回連続で同一結果・安定)

```
type_raw=0x26 → sensor_type=3 (FT9348W(var))     ← SPI 型判定
firmware upload 完了 (10690 bytes, 168 chunks)     ← bulk OUT
MCU ready after 1 polls ✅ (a5 5a)                 ← fw がセンサー MCU で起動
sensor dimensions: 96x96                          ← レジスタから寸法
背景 (FPN) フレーム: bulk IN 9218 bytes ✅        ← bulk 画像経路
```

シーケンス: wake → SUI/F906 → bulk payload → SPI型判定 → fw転送 → AFE設定 →
reset (0x40) → reg 0x20 poll [a5 5a] → 寸法読み → 型別 power-on。
(OMGrant/ft9201-libfprint の状態機械の同期移植 — docs/03 §4)

## 構成

```
Makefile                  make / make firmware / make bg / make capture
scripts/fetch_firmware.sh  fw blob 取得 (mrrbrilliant/ft9201-static .deb → 抽出)
scripts/extract_firmware.py  FOCALFP_9348_FW_APP シンボル抽出 → ft9201_fw.h
src/usb_transport.h/.c    T1 トランスポート (find/open/ctrl/bulk/descriptor)
src/ft9201.h/.c           T2 bring-up (同期版) + FPN背景/指待ち/画像取得
src/usb_probe.c           エンドポイント確認 (make probe)
src/transport_test.c      bulk 転送テスト (安全モード + --bulk-out)
src/capture_test.c        bring-up + 画像取得 (PGM 保存、--raw で matcher 用も)
```

## 使い方

```sh
make firmware        # 初回: fw blob (ft9201_fw.h) を取得・生成
make bg              # bring-up + 背景取得 (指なし・非対話の動作確認)
make capture         # bring-up → 指待ち → 画像取得 (PGM を Preview で確認)
# 精度検証 (T2×T3 統合) — vendor matcher へ実画像:
./capture_test --out enroll --loop 8 --raw     # 登録用に指を8回置く
cd ../pe-loader-test
arch -x86_64 ./engine_test --enroll ../usb-transport/enroll_*.bin
cd ../usb-transport
./capture_test --out verify --loop 3 --raw      # 同じ指で3回
cd ../pe-loader-test
arch -x86_64 ./engine_test --verify ft_template.bin ../usb-transport/verify_*.bin
# → genuine は MATCH、別の指 (imposter) は no match が期待値
```

## 安全上の注意

- init シーケンス途中の失敗はデバイスを不安定にすることがある →
  失敗したらセンサーを抜き差し (電源の入れ直しで fw はリセットされる)
- 本実装はエラー時に継続せず即 abort する
- `transport_test --bulk-out` (指なし bulk 書き込み) は init を伴わないため
  原則使わない

## API の要点 (macOS 27 SDK で確認)

- インターフェースは **kIOUSB{Device,Interface}InterfaceID942** (最新)。
  汎用マクロ `kIOUSBDeviceInterfaceID` は旧 ID100 を指すため明示指定
- CFPlugIn COM 型は `<IOKit/IOCFPlugIn.h>` (IOKitLib.h には無い)
- control: `DeviceRequestTO` / bulk: `ReadPipeTO` / `WritePipeTO`
- プロパティマッチング (idVendor/idProduct を辞書に入れる) は
  機能しなかったため、クラス列挙後にプロパティ読み取りで手動フィルタ

## Phase 1 (T4+) への接続

`usb_transport.c` + `ft9201.c` はそのまま Swift daemon (arm64) の
`UsbBulkTransport` / `Ft9201Sensor` の参照実装になる (Swift から IOKit C API
を呼ぶか、この C コードをヘルパーとして使う)。matcher は別プロセス
(x86_64, Rosetta) — 検証C の engine_test が同じ構成。
