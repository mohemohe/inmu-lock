# inmu-lock — 本番ビルド & 運用 (CLI)
#
#   make vendor      依存バイナリを取得 (FocalTech DLL + MCU fw → ヘッダ生成)
#   make / make all  全コンポーネントをビルド (build/ へ)
#   make enroll     指紋を登録 (インストール済みの inmu-enroll を sudo 実行。対話的に指を置く)
#                  使用前: sudo make stop-daemon (デーモン停止)
#                  使用後: sudo make start-daemon (デーモン再開)
#   make install     システムへインストール (バイナリ・DLL・launchd・sudo_local)
#   make uninstall   解除 (Keychain のパスワードも削除。残るものは README 参照)
#
#   デーモン操作 (すべて冪等):
#     sudo make stop-daemon / start-daemon / restart-daemon   (cored — root権限)
#     make stop-agent / start-agent / restart-agent            (agent — ユーザー権限)
#     make status / make logs

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -g
# 堅牢化フラグ (CFLAGS を上書きしても外れないよう別変数にする)。
# x86_64 の PE ローダーは %gs を差し替えるが、macOS のスタックカナリアは %gs 経由
# ではなく ___stack_chk_guard (GOT 経由) を参照するため影響しない。
HARDEN   = -fstack-protector-strong -D_FORTIFY_SOURCE=2
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
# inmu-agent のコード署名識別子 (リンカ既定の "inmu-agent-arm64.out" 等に依存させない)
AGENT_SIGN_ID = com.mohemohe.inmu-lock.agent

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
	$(CC) $(CFLAGS) $(HARDEN) -arch x86_64 -o $@ src/cored/inmu-cored.c $(LIBSRC) $(INCLUDES) \
	    -framework IOKit -framework CoreFoundation -lbsm

build/pam_inmu.so: src/pam/pam_inmu.c src/common/ipc.h | build
	$(CC) $(CFLAGS) $(HARDEN) -arch arm64 -arch x86_64 -dynamiclib -o $@ src/pam/pam_inmu.c $(INCLUDES) -lpam

# inmu-agent は Keychain のログインパスワードとアクセシビリティ権限を持つ。
# Hardened Runtime (--options runtime) で署名し、DYLD_INSERT_LIBRARIES 等による
# コード注入を無効にする (universal バイナリの両スライスに適用される)。
# 署名に失敗したら未署名の生成物を残さない (install.sh は runtime 無しを拒否する)。
#
# inmu-cored / inmu-enroll には付けない: 実行時に vendor DLL を一時ファイルから
# 実行可能マップする PE ローダーが Hardened Runtime 下では動かない (map section:
# Operation not permitted)。両者は root (launchd / sudo) からのみ起動され、
# 非特権ユーザーが環境変数を渡せる経路が無い (sudo は DYLD_* を除去する)。
build/inmu-agent: src/agent/inmu-agent.c src/common/ipc.h | build
	$(CC) $(CFLAGS) $(HARDEN) -arch arm64 -arch x86_64 -o $@ src/agent/inmu-agent.c $(INCLUDES) \
	    -framework CoreGraphics -framework Security -framework ApplicationServices \
	    -framework CoreFoundation
	codesign --force -s - --options runtime -i $(AGENT_SIGN_ID) $@ || { rm -f $@; exit 1; }

build/inmu-enroll: src/enroll/inmu-enroll.c $(LIBSRC) $(FW_HDR) src/common/ipc.h | build
	$(CC) $(CFLAGS) $(HARDEN) -arch x86_64 -o $@ src/enroll/inmu-enroll.c $(LIBSRC) $(INCLUDES) \
	    -framework IOKit -framework CoreFoundation

# --- 運用 -------------------------------------------------------------

install: all vendor
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

# ユーザーが書き込める build/ の生成物を root で実行しない。
# インストール済み (root 所有) の inmu-enroll だけを実行する。
enroll:
	@if [ ! -x "$(BINDIR)/inmu-enroll" ]; then \
	    echo "  !! $(BINDIR)/inmu-enroll がありません — make install を先に実行してください"; \
	    exit 1; \
	fi
	@echo "!! inmu-cored が起動中の場合:"
	@echo "   sudo make stop-daemon && sudo make enroll && sudo make start-daemon"
	@echo ""
	@# 既に root (sudo make enroll) なら sudo を重ねない: 重ねると SUDO_UID / SUDO_USER が
	@# root を指し、呼び出し元の実ユーザーが分からなくなる。
	@if [ "$$(id -u)" = "0" ]; then \
	    $(BINDIR)/inmu-enroll; \
	else \
	    sudo $(BINDIR)/inmu-enroll; \
	fi

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
