# inmu-lock — 本番ビルド & 運用 (CLI)
#
#   make vendor      依存バイナリを取得 (FocalTech DLL + MCU fw → ヘッダ生成)
#   make / make all  全コンポーネントをビルド (build/ へ)
#   make enroll     指紋を登録 (ビルド + sudo 実行。対話的に指を置く)
#                  使用前: sudo make stop-daemon (デーモン停止)
#                  使用後: sudo make start-daemon (デーモン再開)
#   make install     システムへインストール (バイナリ・DLL・launchd・sudo_local)
#   make uninstall   全解除
#
#   デーモン操作 (すべて冪等):
#     sudo make stop-daemon / start-daemon / restart-daemon   (cored — root権限)
#     make stop-agent / start-agent / restart-agent            (agent — ユーザー権限)
#     make status / make logs

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -g
INCLUDES = -Isrc/common -Isrc/lib
LIBSRC   = src/lib/usb_transport.c src/lib/ft9201.c src/lib/ft_engine_mac.c
FW_HDR   = src/lib/ft9201_fw.h

BINDIR     = /usr/local/bin
PAMDIR     = /usr/local/lib/pam
LIBEXECDIR = /usr/local/libexec/inmu-lock
ETCDIR     = /usr/local/etc/inmu-lock
LOGDIR     = /var/log/inmu-lock
DAEMON_ID  = com.mohemohe.inmu-lock.cored
AGENT_ID   = com.mohemohe.inmu-lock.agent
DAEMON_PLIST = /Library/LaunchDaemons/com.mohemohe.inmu-lock.cored.plist
AGENT_PLIST  = $(HOME)/Library/LaunchAgents/com.mohemohe.inmu-lock.agent.plist

# sudo 実行時の実際のユーザー uid (agent は GUI ドメインのため root 不可)
REAL_UID  = $(shell id -u $(SUDO_USER) 2>/dev/null || id -u)
REAL_USER = $(shell echo $(SUDO_USER) 2>/dev/null || whoami)

.PHONY: all vendor enroll install uninstall clean
.PHONY: stop-daemon start-daemon restart-daemon
.PHONY: stop-agent start-agent restart-agent
.PHONY: status logs

all: build/inmu-cored build/pam_inmu.so build/inmu-agent build/inmu-enroll

# --- ビルド -------------------------------------------------------------

$(FW_HDR):
	./scripts/fetch-vendor.sh

vendor:
	./scripts/fetch-vendor.sh

build:
	mkdir -p build

build/inmu-cored: src/cored/inmu-cored.c $(LIBSRC) $(FW_HDR) src/common/ipc.h | build
	$(CC) $(CFLAGS) -arch x86_64 -o $@ src/cored/inmu-cored.c $(LIBSRC) $(INCLUDES) \
	    -framework IOKit -framework CoreFoundation

build/pam_inmu.so: src/pam/pam_inmu.c src/common/ipc.h | build
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -dynamiclib -o $@ src/pam/pam_inmu.c $(INCLUDES) -lpam

build/inmu-agent: src/agent/inmu-agent.c src/common/ipc.h | build
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -o $@ src/agent/inmu-agent.c $(INCLUDES) \
	    -framework CoreGraphics -framework Security -framework ApplicationServices \
	    -framework CoreFoundation

build/inmu-enroll: src/enroll/inmu-enroll.c $(LIBSRC) $(FW_HDR) src/common/ipc.h | build
	$(CC) $(CFLAGS) -arch x86_64 -o $@ src/enroll/inmu-enroll.c $(LIBSRC) $(INCLUDES) \
	    -framework IOKit -framework CoreFoundation

# --- 運用 -------------------------------------------------------------

install: all vendor
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

enroll: build/inmu-enroll
	@echo "!! inmu-cored が起動中の場合:"
	@echo "   sudo make stop-daemon && sudo make enroll && sudo make start-daemon"
	@echo ""
	sudo ./build/inmu-enroll

# --- デーモン操作 (冪等 — scripts/{daemon,agent}.sh に委譲) ----------------
# cored (root権限): sudo make stop-daemon / start-daemon / restart-daemon
# agent (ユーザー権限): make stop-agent / start-agent / restart-agent

stop-daemon:
	@if [ "$$(id -u)" != "0" ]; then echo "  !! 'sudo make stop-daemon' を実行してください (root権限が必要)"; exit 1; fi
	@./scripts/daemon.sh stop

start-daemon:
	@if [ "$$(id -u)" != "0" ]; then echo "  !! 'sudo make start-daemon' を実行してください (root権限が必要)"; exit 1; fi
	@./scripts/daemon.sh start

restart-daemon:
	@if [ "$$(id -u)" != "0" ]; then echo "  !! 'sudo make restart-daemon' を実行してください (root権限が必要)"; exit 1; fi
	@./scripts/daemon.sh restart

stop-agent:
	@if [ "$$(id -u)" = "0" ]; then echo "  !! 'make stop-agent' を実行してください (ユーザー権限で実行)"; exit 1; fi
	@./scripts/agent.sh stop

start-agent:
	@if [ "$$(id -u)" = "0" ]; then echo "  !! 'make start-agent' を実行してください (ユーザー権限で実行)"; exit 1; fi
	@./scripts/agent.sh start

restart-agent:
	@if [ "$$(id -u)" = "0" ]; then echo "  !! 'make restart-agent' を実行してください (ユーザー権限で実行)"; exit 1; fi
	@./scripts/agent.sh restart

# --- 情報表示 -------------------------------------------------------------

status:
	@echo "=== cored (LaunchDaemon) ==="
	@./scripts/daemon.sh status
	@echo ""
	@echo "=== agent (LaunchAgent) ==="
	@./scripts/agent.sh status

logs:
	sudo tail -f $(LOGDIR)/cored.log

clean:
	rm -rf build
