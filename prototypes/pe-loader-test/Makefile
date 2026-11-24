# 検証C: Rosetta 2 (x86_64) でビルド・実行する
#
#   make / make all      stage0_gs と engine_test をビルド
#   make stage0-run     Stage 0 (%gs 制御プローブ) を実行
#   make fetch          DLL (vendor blob) を取得 (scripts/fetch_dll.sh)
#   make run            Stage 2 (実DLLでエンジン駆動) を実行
#
# ビルドは -arch x86_64 (Apple Silicon 上では Rosetta 2 で実行される)

CC ?= cc
ARCHS = -arch x86_64
CFLAGS = $(ARCHS) -O0 -g -Wall
DLL = blobs/ftWbioEngineAdapter.dll

.PHONY: all stage0-run run fetch clean

all: stage0 engine_test

stage0: src/stage0_gs.c
	$(CC) $(CFLAGS) -o $@ $<

engine_test: src/engine_test.c src/ft_engine_mac.c src/ft_engine.h
	$(CC) $(CFLAGS) -o $@ src/engine_test.c src/ft_engine_mac.c

stage0-run: stage0
	arch -x86_64 ./stage0

fetch: scripts/fetch_dll.sh
	./scripts/fetch_dll.sh

run: engine_test $(DLL)
	arch -x86_64 ./engine_test $(DLL)

$(DLL): scripts/fetch_dll.sh
	./scripts/fetch_dll.sh

clean:
	rm -f stage0 engine_test
