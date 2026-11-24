# 検証D (T1/T2): IOKit USB トランスポート + FT9201 bring-up
#
#   make firmware     fw blob (ft9201_fw.h) を取得・生成 (初回)
#   make              全ツールをビルド
#   make probe        エンドポイント確認 (descriptor ダンプ)
#   make test         bulk 転送テスト (安全モード)
#   make bg           bring-up + 背景フレーム取得 (指なし・非対話の T2 検証)
#   make capture      bring-up → 指待ち → 画像取得 (対話的)
#   make clean

CC ?= cc
CFLAGS = -Wall -Wextra -O2 -g
FRAMEWORKS = -framework IOKit -framework CoreFoundation
COMMON = src/usb_transport.c

.PHONY: all probe test bg capture firmware clean

all: usb_probe transport_test capture_test

usb_probe: src/usb_probe.c $(COMMON) src/usb_transport.h
	$(CC) $(CFLAGS) -o $@ src/usb_probe.c $(COMMON) $(FRAMEWORKS)

transport_test: src/transport_test.c $(COMMON) src/usb_transport.h
	$(CC) $(CFLAGS) -o $@ src/transport_test.c $(COMMON) $(FRAMEWORKS)

capture_test: src/capture_test.c src/ft9201.c src/ft9201_fw.h $(COMMON) \
              src/ft9201.h src/usb_transport.h
	$(CC) $(CFLAGS) -o $@ src/capture_test.c src/ft9201.c $(COMMON) $(FRAMEWORKS)

# fw blob (FocalTech所有物 — 生成物はコミットしない)
src/ft9201_fw.h:
	./scripts/fetch_firmware.sh

firmware:
	./scripts/fetch_firmware.sh

probe: usb_probe
	./usb_probe

test: transport_test
	./transport_test

bg: capture_test
	./capture_test --bg --out ft

capture: capture_test
	./capture_test --out ft

clean:
	rm -f usb_probe transport_test capture_test
