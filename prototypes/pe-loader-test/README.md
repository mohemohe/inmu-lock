# 検証C — Rosetta 2 + PEローダー実験 (T3)

FocalTech の Windows 用マッチングエンジン `ftWbioEngineAdapter.dll` (x86-64 PE) を
**Apple Silicon の macOS で (Rosetta 2 経由で) in-process 実行できるか**を検証する実験。
FT9201 プロジェクト全体の最大技術リスク (matcher の調達) を潰すためのもの。

## 結果: ✅ 成立 (2026-11-24, macOS 27.0.1 / Apple Silicon / Rosetta 2)

### Stage 2 (合成画像): PEロード + エンジン実行

```
[loader] DllMain returned 1
[Engine] WbioQueryEngineInterface: Implementing interface version 2.0
[Engine] EngineAdapterAttach: v1.3.3, chip_type=3, width=64, height=80
=== クラッシュせず全API通過 ===
```

### Stage 3 (実指紋画像 — T2×T3統合): 登録・照合が成立

```
quality:78, area:99, humility:50          ← 実画像の品質評価 (vendorエンジン)
[storage] AddRecord(sub=240) size=176058  ← テンプレート生成 (~176KB, 18サブtpl想定)
[Engine] I EngineAdapterVerifyFeatureSet: Match!   ← 同じ指の画像 3/3 MATCH ✅
背景フレーム(指なし) → accept reject (偽陽性なし) ✅
```

**→ FT9201 実画像 → vendor matcher → 照合判定 までの全経路が Apple Silicon で実証された。**
残る精度評価: 別指 (imposter) で no match であることの確認のみ。

### 実装上の重要な確定事項 (gs 切替え規律)

macOS の libSystem は %gs (TLS) を malloc/stdio 等で使うため、
**shim が libc を呼ぶ間だけ %gs を libc 基底へ戻す規律が必須**
(固定 gs=TEB のまま malloc/stdio を数百回呼ぶとヒープ破損し、
libSystem 内部の整合性チェックが SIGILL で停止する — 実画像処理で観測・解決)。

- libc TSD 基底は pthread 構造体内の自己参照 `*(pthread_t + K) == pthread_t`
  をスキャンして動的に特定 (macOS 27 で K=0xE0。stage0c2 で確立)
- 各エンジン呼び出し・危険なshim (malloc/free/realloc/stdio)・
  storage コールバックは `gs_to_teb()` ↔ `gs_to_libc()` で包む
- mach trap 0x3000003 の rdx は旧ベースではない (rax は新ベースの echo)

## 実験の段階と確定事項

| Stage | 内容 | 結果 |
|---|---|---|
| 0 | `%gs` (Windows TEB) の制御可否 | ✅ mach trap 0x3000003 (`thread_set_tsd_base`) を直接 syscall 呼び出しで制御。FSGSBASE 命令は Rosetta 上で使用不可 (SIGILL)。libc は偽TEB (ゼロ埋めBSS) を指したまま正常動作 → 復元不要の設計 |
| 2 | 実DLL のロード・駆動 | ✅ DllMain / Query(v2.0) / Attach / Accept / Enroll / Verify / storageコールバックがすべて動作 |

## 構成

```
Makefile                make / make run / make fetch / make stage0-run
scripts/fetch_dll.sh    DLL を MS Update Catalog から取得 (cabextract)
blobs/                  ftWbioEngineAdapter.dll (git管理外。FocalTech所有物)
src/stage0_gs.c         %gs 制御プローブ (Stage 0)
src/ft_engine_mac.c     PEローダー + kernel32 shim群 + エンジン駆動 (LGPL-2.1)
src/ft_engine.h         エンジンAPI (ft9201-libfprint と同一)
src/engine_test.c       Stage 2 ドライバ (合成画像でエンジンを駆動)
```

## 実行方法

```sh
brew install cabextract   # 初回のみ (fetch_dll.sh が入れてもよい)
make fetch                # DLL 取得 (MS Update Catalog から)
make run                  # Stage 2: arch -x86_64 ./engine_test blobs/...dll
make stage0-run           # Stage 0 単体
```

## macOS 移植の要点 (ft_engine.c からの差分)

1. **%gs 切替え**: `syscall(SYS_arch_prctl, ARCH_SET_GS)` → mach trap 0x3000003 を
   インラインアセンブリで直接呼ぶ (libsystem_kernel の `__thread_set_tsd_base`
   スタブと同一。プライベートシンボルでリンク/dlsym 不可能のため trap 直叩き)。
   戻り値 rax は「設定したベースの echo」で、復元には使えない
2. **gs 切替え規律 (実画像処理で確立)**: shim 内で libc (malloc/stdio) を
   呼ぶ間だけ %gs を libc TSD 基底へ戻す。基底は pthread 構造体内の自己参照
   `*(pthread_t + K) == pthread_t` (K=0xE0@macOS 27) をスキャンして動的特定。
   固定 gs=TEB のまま libc を呼び続けるとヒープ破損 → SIGILL となる
3. **memfd_create** → `mkstemp` + `mmap` + `unlink` (mappingは有効のまま)
4. **MAP_FIXED_NOREPLACE** → `MAP_FIXED` (推奨ベース 0x180000000 は競合しない)
5. `__attribute__((ms_abi))` は clang x86_64 でそのまま使用可

## 実画像モード (T2×T3 統合)

```sh
# 登録 (capture_test --raw で取得した 96×96 8bit 生画像)
arch -x86_64 ./engine_test --enroll ../usb-transport/enroll_*.bin
# → ft_template.bin (vendor形式, ~176KB)

# 照合 (同じ指の画像 → MATCH が期待値)
arch -x86_64 ./engine_test --verify ft_template.bin ../usb-transport/verify_*.bin
# → [Engine] I EngineAdapterVerifyFeatureSet: Match!

# 引数なし → 合成画像モード (検証C Stage 2 の従来動作)
```

## Phase 1 への接続

本実験の `engine_test` を発展させ、**matcher ヘルパー (x86_64 プロセス) として
Unix socket 化する** (arm64 の Core Service から DLL は直接ロードできないため)。
実画像での精度評価 (genuine 5/5 + imposter 0/5) は T2 (FT9201 bring-up) で
センサーから画像を取得してから行う。

## ライセンス

- `src/ft_engine_mac.c` / `src/ft_engine.h`: **LGPL-2.1-or-later**
  (OMGrant/ft9201-libfprint `src/ft_engine.c` の移植物)
- `ftWbioEngineAdapter.dll`: FocalTech の所有物。MS Update Catalog から
  ユーザー自身が取得する形式 (リポジトリには含めない)
