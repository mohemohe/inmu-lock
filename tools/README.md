# 診断ツール (tools/)

本番コンポーネント (`src/`) の動作を個別に確認・デバッグするためのツール群。
いずれも `src/lib` の検証済みライブラリを直接使う（デーモン不要）。

```sh
make          # 3ツールをビルド (fw ヘッダが無ければ make vendor も自動実行)
```

## capture_test — センサー画像をファイルに保存 (照合不良時の原因調査用)

```sh
./capture_test --out ft          # bring-up → 指待ち → 96×96画像をPGM保存
                                # (Previewで開ける。--raw併用で engine_test 用 .bin も出力)
./capture_test --bg --out ft     # 指なしで背景(FPN)のみ取得 (非対話の動作確認)
```

照合がうまくいかないとき: `ft_01.pgm` を見て指紋が鮮明に写っているか、
`quality=0xdd 0xdd` (正常) が出ているかを確認する。

## engine_test — matcher (vendor DLL) を画像ファイルで駆動

```sh
# 画像から登録 (テンプレートを ft_template.bin に保存)
arch -x86_64 ./engine_test --enroll img_*.bin

# 登録テンプレートで照合 (同じ指の画像 → MATCH が期待値)
arch -x86_64 ./engine_test --verify ft_template.bin img_*.bin

# 引数なし → 合成画像モード (DllMain/Attach/エンジン起動の確認)
arch -x86_64 ./engine_test
```

x86_64 (Rosetta 2) 専用 — vendor DLL を in-process 実行するため。
デーモンを介さず matcher 単体の登録・照合・reject 挙動を検証できる。

## usb_probe — USB 構成の確認

```sh
./usb_probe     # VID/PID検索・エンドポイント一覧・string descriptor 表示
```

センサーが認識されないとき、まずこれと `scripts/inspect_sensor.sh` で
レベルの切り分けをする (OS が見えているか / user-space から開けるか)。
