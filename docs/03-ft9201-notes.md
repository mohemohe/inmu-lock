# 03 — FT9201 (2808:93a9) 技術調査メモ

対象デバイス: **Focal-systems.Corp FT9201Fingerprint** (USB VID=2808 PID=93A9)
「Windows Hello専用」として売られている安価なUSB指紋センサー。

---

## 1. デバイスの正体

- FocalTech (Focal-systems) の**光式 96×96** センサー。市販品の多くは FT9348W バリアント
  （ARCANITE 等のドングル、ASIN B0DK7LQZGH）
- **Match-on-host 型**: センサー自身は指紋照合を行わない。画像をホストへ送り、
  ホスト側の matcher が照合する → macOS では matcher の調達が主課題
- センサーの MCU は **8051 コアでファームウェアを持たない**。電源投入のたびにホストが
  bulk 転送でファームウェアをアップロードし、特定のレジスタ列を書いて起動する必要がある

## 2. 本機 (macOS 27.0.1 / Apple Silicon) での実測 — 2026-11-24

センサー挿入時に `scripts/inspect_sensor.sh` で採取した結果：

| 項目 | 値 | 意味 |
|---|---|---|
| デバイス名 | `FT9201Fingerprint.̚ @03132000` | ioreg上の認識名 |
| idVendor / idProduct | `10248 (0x2808)` / `37801 (0x93A9)` | 対象デバイスと一致 ✓ |
| bDeviceClass | 0 (vendor-specific) | デバイスクラスは IF側で定義 |
| bInterfaceClass / Sub / Proto | **0xFF / 0xFF / 0xFF** | **HIDではない**。vendor bulk |
| bNumEndpoints | **2** | **EP 0x02 bulk OUT (maxPkt 16) + EP 0x83 bulk IN (maxPkt 32)** — 検証D (2026-11-24) で config descriptor から実測、Linux実装の定数と完全一致 |
| bcdUSB / 速度 | 512 (USB 2.0) / Full-Speed (12Mbps) | bulk 転送で十分な帯域 |
| HID (IOHIDDevice) | 登録なし | HIDドライバに握られていない |
| Apple製ドライバのアタッチ | なし | ユーザースペースから自由に扱える |
| IOCFPlugInTypes | `IOUSBLib.bundle` | **ユーザースペースの IOUSBLib user client でオープン可能** (検証Dで実証: open/control/bulk すべてエンタイトルメント不要) |

→ macOS の IOKit ユーザースペースAPI (IOUSBDeviceInterface942 / IOUSBInterfaceInterface942) で
bulk 通信できる見込み。**カーネル拡張も DriverKit も不要 (検証D で実証済み)。**

## 3. 最大の課題: matcher は x86_64 の Windows DLL

Linux コミュニティの成果（§5）で、このセンサーは:

- センサー画像 96×96 をそのまま libfprint 標準の NBIS/Bozorth3 matcher に食べさせると
  **精度が出ない**（genuine finger が自分の登録に 0/5 で不一致）
- FocalTech 製 Windows ドライバの `ftWbioEngineAdapter.dll`（x86_64 PE）が
  このセンサー用にチューニングされた matcher であり、**96×96 を中央 64×80 に crop して渡すと 5/5 で一致**

つまり Windows Hello 専用センサーの「専用」の実体はこの DLL にある。
macOS (Apple Silicon) では x86_64 PE を直接実行できないため、Matcher 層（docs/02）で
次の候補を評価する:

| 候補 | 内容 | リスク |
|---|---|---|
| (a) Rosetta 2 + PE ローダー移植 | x86_64 プロセス (Rosetta) 内で DLL を map して実行。Linux 実装 (`ft_engine.c`, 約450行, kernel32 の shim ~90関数, W^X保持) を参考に macOS に移植 | ~~Rosetta 上で x64 呼び出し規約が動くか未知数~~ → **2026-11-24 に実証成功** (下記) |
| (b) オープンソース matcher | SourceAFIS / NBIS 等を 64×80 画像向けに再評価 | 96×96 では NBIS が 0/5 だった前例あり。要 FRR/FAR 実測 |
| (c) x86_64 別ホストを matcher サーバー化 | 別マシンで DLL を実行し daemon と通信 | 運用が現実的でない場合が多い |

### §3-a の実証結果 (検証C, 2026-11-24 実施) — ✅ 成立

検証Cプロジェクトとして実施（コードは `src/lib/ft_engine_mac.c` に昇格、
診断ツールは `tools/engine_test`）。**ftWbioEngineAdapter.dll (x86-64 PE) が
Apple Silicon (M1系, macOS 27.0.1) 上の Rosetta 2 で完全に動作した。**

実測で確定した技術要素:

| 要素 | 結果 |
|---|---|
| FSGSBASE (wrgsbase/rdgsbase) | ❌ 使用不可 (CPUID bit16=0, SIGILL) |
| **%gs 切替え (TEB)** | ✅ **mach trap 0x3000003 (thread_set_tsd_base)** を直接 syscall で呼ぶ。libsystem_kernel のスタブと同一の呼び出し方 (rax=0x3000003, rdi=base, rsi=0)。Rosetta は gsbase 変更を正しく反映する |
| trap の戻り値 | 「設定した新しいベース」の echo (旧ベースではない) |
| gs=偽TEB 中の libSystem | malloc/printf 等は正常動作 (偽TEBはゼロ埋めBSS)。→ %gs をTEBに固定したまま使う設計で成立 |
| W^X PE マッピング | ✅ mkstemp + mmap (セクションごと R+X / R+W)。コードはfile-backedで実行 (JIT制限に依存しない) |
| DLL 実行 | ✅ 推奨ベース 0x180000000 にロード、DllMain=1、`WbioQueryEngineInterface` が interface v2.0 を返す |
| エンジン初期化 | ✅ Attach 成功。アルゴリズム設定は Linux 版の解析と完全一致 (v1.3.3, chip_type=3, width=64, height=80, max_tpl=18, sub_tpl=32) |
| 合成画像の accept | `WINBIO_E_NO_CAPTURE (0x80098008)` で reject — 品質チェックが正しく動作している証拠 (実画像の精度検証は T2 実装後) |
| 登録/照合/ストレージコールバック | ✅ UpdateEnrollment / CommitEnrollment / storage vtable コールバックがすべてクラッシュ無しで通過 |

アーキテクチャへの反映: Core Service (arm64 Swift) は DLL を直接ロードできないため、
**matcher を専用の x86_64 ヘルパープロセス (Rosetta, `engine_test` の発展形) に分離し、
daemon と Unix socket で通信する**構成が確定した (docs/01 §4.1, docs/02 §4 参照)。

### 実画像での登録・照合 (T2×T3 統合, 2026-11-24) — ✅ 成立

FT9201 から取得した実指紋画像 (96×96) で matcher 全経路を検証:

| テスト | 結果 |
|---|---|
| 登録 (実画像8枚) | `quality:78, area:99, humility:50` — 8/8 受理、テンプレート **176,058 bytes** 生成 ✅ |
| 照合 (同じ指の画像3枚) | **`[Engine] I EngineAdapterVerifyFeatureSet: Match!` — genuine 3/3 MATCH** ✅ |
| ネガティブコントロール (指なし背景) | accept段階で reject (`0x80098008`)、偽陽性なし ✅ |
| 別指 (imposter) | 未実施 (FAR確認は実機で継続) |

過程で判明した実装要件 (docs と README に反映済み):
**macOS の libSystem は %gs (TLS) を malloc/stdio で使うため、shim 内で libc を
呼ぶ間だけ %gs を libc TSD 基底へ戻す「gs 切替え規律」が必須。**
固定 gs=TEB のまま数百回の malloc/stdio を呼ぶとヒープ破損し、
libSystem 内部の整合性チェックが SIGILL で停止する。
libc 基底は pthread 構造体内の自己参照 `*(pthread_t + 0xE0) == pthread_t`
(macOS 27) をスキャンして動的に特定する (stage0c2 で確立)。

## 4. 判明済みのプロトコル詳細（Linux 実装から）

`connect()` (= bring-up) のシーケンス（docs/02 の `Ft9201Sensor.connect` が担う部分）:

1. bulk エンドポイントで **8051 ファームウェア blob を転送**
2. `InitMcuConfig` + `SwitchNextSensorWorkMode`:
   - AFE レジスタ `0x01`, `0x41`, `0x30`, `0x54`
   - 電源オンレジスタ `0x1f`, `0x1e`
   - reset
3. status レジスタ `0x20` が `0xa5 0x5a` を返したら準備完了
4. 照合時は指の有無を poll → 96×96 フレームを bulk in で取得
5. **中央 64×80 に crop**（リサイズ禁止。歪みが ridge 間隔を壊して不一致になる）

不揮発のレジスタが初期化で壊れることがあり、Linux 実装では「初期化に失敗したら
Windows マシンに挿して安定状態に戻す」運用が報告されている → daemon は bring-up 失敗時に
リトライ戦略を持つこと。

## 5. 参考実装・文献（重要度順）

| ソース | 内容 |
|---|---|
| https://github.com/OMGrant/ft9201-libfprint | ★本命の参考。libfprint ドライバ + vendor DLL の PE ローダー (`ft_engine.c`)。LGPL-2.1。`docs/how-it-works.md`, `PORTING.md` が詳細 |
| https://github.com/banianitc/ft9201-fingerprint-driver | USB プロトコルの一次リバースエンジニアリング (Linux kernel module, LGPL-2.1)。init/capture のコマンド列はここから |
| https://github.com/mrrbrilliant/ft9201-static | FocalTech の Linux blob。fw `FOCALFP_9348_FW_APP` と初期化シーケンスの出典 |
| https://grantgarrison.com/blog/solving-linuxs-fingerprint-reader-problem/ | 経緯と 64×80 crop の発見の記録 (2026-11) |
| https://github.com/ryenyuku/libfprint-ft9201, https://github.com/Romk-a/ft9201-linux-setup | FocalTech 製の Linux 用 TOD ドライバー (x86_64 ELF, PID 9338→93a9 patch)。macOS では使えないが動作仕様の傍証 |

**ライセンス注意:** 参考実装はいずれも LGPL-2.1。プロトコル定数の移植は事実上の事実利用なので
実害は低いが、コードを直接コピーする場合は派生物にも LGPL を適用すること。

## 6. fw blob / DLL の入手方法（リポジトリには含めない）

| ファイル | 入手元 |
|---|---|
| `ftWbioEngineAdapter.dll` | Microsoft Update Catalog で「FocalTech Electronics Biometric」(driver 1.0.3.58, HW ID `USB\VID_2808&PID_93A9`) を検索 → .cab → cabextract |
| MCU fw (`FOCALFP_9348_FW_APP`) | `mrrbrilliant/ft9201-static` の blob から `extract-firmware.py` で抽出 |

どちらも FocalTech の所有物のため配布せず、ユーザーが自身の権利で取得する形式にする
（ft9201-libfprint と同じ方針）。

## 7. 調査TODO

- [x] ~~config descriptor から **bulk IN/OUT の bEndpointAddress を確定**~~ → **✅ 2026-11-24 成立** (検証D: EP 0x02 OUT / 0x83 IN を実測、Linux実装と一致。実装は `src/lib/usb_transport.c`、診断は `tools/usb_probe`)
- [x] ~~**Rosetta 2 + PE ローダー最小実験**~~ → **✅ 2026-11-24 成立** (検証C。詳細は §3-a。実装は `src/lib/ft_engine_mac.c`)
- [ ] banianitc `ft9201.c` のコマンド定数を整理して `Ft9201Command.swift` に移植
- [ ] fw blob の抽出・保存形式の決定（署名検証など改ざん対策も）
- [ ] **matcher ヘルパーを x86_64 プロセスとして実装** (engine_test をベースに Unix socket 化。実画像での genuine 5/5 + imposter 0/5 が目標 — T2 のセンサー画像で評価)
- [ ] SourceAFIS の 64×80 クロップに対する精度評価（vendor matcher のフォールバック用。優先度は下がった）
- [ ] bring-up 失敗時のリトライ戦略の設計
