# inmu-lock — 本番ビルド & 運用 (CLI)
#
#   make vendor      依存バイナリを取得 (FocalTech DLL + MCU fw → ヘッダ生成)
#   make / make all  全コンポーネントをビルド (build/ へ)
#   make enroll     指紋を登録 (ビルド + sudo 実行。対話的に指を置く)
#   make install     システムへインストール (バイナリ・DLL・launchd・sudo_local)
#   make uninstall   全解除
#   make status      launchd 上の稼働状態を表示
#   make logs        cored のログを tail
#   make clean       ビルド生成物を削除

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -g
INCLUDES = -Isrc/common -Isrc/lib
LIBSRC   = src/lib/usb_transport.c src/lib/ft9201.c src/lib/ft_engine_mac.c
FW_HDR   = src/lib/ft9201_fw.h

BINDIR   = /usr/local/bin
PAMDIR   = /usr/local/lib/pam
LIBEXECDIR = /usr/local/libexec/inmu-lock
ETCDIR   = /usr/local/etc/inmu-lock
LOGDIR   = /var/log/inmu-lock
DAEMON_ID  = com.mohemohe.inmu-lock.cored
AGENT_ID   = com.mohemohe.inmu-lock.agent

.PHONY: all vendor enroll install uninstall status logs clean

all: build/inmu-cored build/pam_inmu.so build/inmu-agent build/inmu-enroll

# --- ビルド -------------------------------------------------------------

# fw ヘッダは取得・生成が必要 (make vendor)
$(FW_HDR):
	./scripts/fetch-vendor.sh

vendor:
	./scripts/fetch-vendor.sh

build:
	mkdir -p build

# Core Service デーモン: x86_64 (Rosetta 2 — vendor DLL を in-process 実行)
build/inmu-cored: src/cored/inmu-cored.c $(LIBSRC) $(FW_HDR) src/common/ipc.h | build
	$(CC) $(CFLAGS) -arch x86_64 -o $@ src/cored/inmu-cored.c $(LIBSRC) $(INCLUDES) \
	    -framework IOKit -framework CoreFoundation

# PAM モジュール: universal (sudo は host arch でロードする)
build/pam_inmu.so: src/pam/pam_inmu.c src/common/ipc.h | build
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -dynamiclib -o $@ src/pam/pam_inmu.c $(INCLUDES) -lpam

# Unlock Agent: universal
build/inmu-agent: src/agent/inmu-agent.c src/common/ipc.h | build
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -o $@ src/agent/inmu-agent.c $(INCLUDES) \
	    -framework CoreGraphics -framework Security -framework ApplicationServices \
	    -framework CoreFoundation

# 登録ツール: x86_64 (matcher DLL を使うため)
build/inmu-enroll: src/enroll/inmu-enroll.c $(LIBSRC) $(FW_HDR) src/common/ipc.h | build
	$(CC) $(CFLAGS) -arch x86_64 -o $@ src/enroll/inmu-enroll.c $(LIBSRC) $(INCLUDES) \
	    -framework IOKit -framework CoreFoundation

# --- 運用 -------------------------------------------------------------

enroll: build/inmu-enroll
	sudo ./build/inmu-enroll

install: all vendor
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

status:
	sudo launchctl print system/$(DAEMON_ID) 2>/dev/null | head -8 || echo "(cored 未登録/未起動)"
	launchctl print gui/$$(id -u)/$(AGENT_ID) 2>/dev/null | head -8 || echo "(agent 未登録/未起動)"

logs:
	sudo tail -f $(LOGDIR)/cored.log

clean:
	rm -rf build