# 検証A: ダミーPAMモジュールのビルド
#
#   make            モジュールとハーネスをビルド
#   make check      ハーネスを /etc/pam.d/inmutest 向けに実行 (要 install.sh)
#   make clean      生成物を削除
#
# システムへのインストール/アンインストールは install.sh / uninstall.sh を使うこと
# (root 権限が必要な処理とロールバックをまとめている)。

CC         ?= cc
CFLAGS     ?= -O2 -Wall -Wextra
PAM_LIBDIR  = /usr/local/lib/pam
MODULE      = pam_inmu_dummy.so
HARNESS     = pam_harness

.PHONY: all check clean

all: $(MODULE) $(HARNESS)

# PAMモジュールは共有ライブラリ (macOSでは -dynamiclib)
# pam_get_item / pam_get_user を呼ぶため -lpam をリンク
$(MODULE): src/pam_inmu_dummy.c
	$(CC) $(CFLAGS) -fPIC -dynamiclib -o $@ $< -lpam

$(HARNESS): src/pam_harness.c
	$(CC) $(CFLAGS) -o $@ $< -lpam

check: $(HARNESS)
	@echo "== pam_harness を /etc/pam.d/inmutest に対して実行します =="
	./$(HARNESS) inmutest || true

clean:
	rm -f $(MODULE) $(HARNESS)
