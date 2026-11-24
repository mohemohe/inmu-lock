# 診断ツールのビルド (src/lib の検証済みライブラリを直接使用)
#
#   make                     3ツールをビルド
#   make capture / engine / probe   個別ビルド
#   make clean

CC ?= cc
CFLAGS = -Wall -Wextra -O2 -g
INCLUDES = -I../src/common -I../src/lib
FW_HDR = ../src/lib/ft9201_fw.h
USB_LIB = ../src/lib/usb_transport.c
USB_FULL = ../src/lib/usb_transport.c ../src/lib/ft9201.c

.PHONY: all capture engine probe clean

all: capture_test usb_probe engine_test

$(FW_HDR):
	cd .. && ./scripts/fetch-vendor.sh

# センサー画像キャプチャ (universal: USB のみで matcher 不要)
capture_test: capture_test.c $(FW_HDR)
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -o $@ capture_test.c $(USB_FULL) \
	    $(INCLUDES) -framework IOKit -framework CoreFoundation

# USB 構成確認 (universal)
usb_probe: usb_probe.c
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -o $@ usb_probe.c $(USB_LIB) \
	    $(INCLUDES) -framework IOKit -framework CoreFoundation

# matcher 駆動テスト (x86_64 / Rosetta 2 専用 — vendor DLL を実行するため)
engine_test: engine_test.c ../src/lib/ft_engine_mac.c
	$(CC) $(CFLAGS) -arch x86_64 -o $@ engine_test.c ../src/lib/ft_engine_mac.c $(INCLUDES)

capture: capture_test
engine: engine_test
probe: usb_probe

clean:
	rm -f capture_test usb_probe engine_test
