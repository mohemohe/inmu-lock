/*
 * engine_test.c — 検証C Stage 2 + T2統合: vendor matcher DLL を実画像で駆動する。
 *
 *   ./engine_test blobs/ftWbioEngineAdapter.dll                    → 合成画像 (従来)
 *   ./engine_test --enroll <img1.bin> [img2.bin ...]                → 実画像で登録
 *       (各 .bin は capture_test --raw の出力: 96×96 8bit, FPN補正+伸長済み)
 *       テンプレートは ft_template.bin に保存
 *   ./engine_test --verify ft_template.bin <img.bin> [img.bin ...]  → 実画像で照合
 *
 * 成功基準 (docs/03 §7): genuine 5/5 (自分の指の登録・照合が一致) +
 * imposter 0/5 (別の指・他人の画像が不一致)。合成画像は reject が想定。
 */

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "ft_engine.h"

#define IMG_W 96
#define IMG_H 96
#define IMG_BYTES (IMG_W * IMG_H)

/* クラッシュ診断: フォールト時の RIP (DLL内ならオフセット) と周辺バイトを表示 */
static void crash_handler(int sig, siginfo_t *si, void *ctx_) {
    (void)si;
    ucontext_t *uc = (ucontext_t *)ctx_;
    uint64_t rip = uc->uc_mcontext->__ss.__rip;
    static const uint64_t imgbase = 0x180000000ULL;
    dprintf(2, "\n[crash] signal=%d rip=0x%llx", sig, (unsigned long long)rip);
    if (rip >= imgbase && rip < imgbase + 0x10000000) {
        dprintf(2, " (DLL+%#llx)", (unsigned long long)(rip - imgbase));
    }
    /* 常に周辺バイトをダンプ (SIGILLの原因命令の特定用) */
    uint8_t *p = (uint8_t *)(uintptr_t)rip;
    dprintf(2, " bytes[rip-8..+16] = ");
    for (int i = -8; i < 16; i++) {
        uint8_t b = 0;
        // 読めるか確実ではないので簡易ダンプ (フォルト時は0で諦め)
        memcpy(&b, p + i, 1);
        dprintf(2, "%02x ", b);
    }
    dprintf(2, "\n");
    _exit(99);
}

/* 疑似指紋縞 (合成画像テスト用) */
static void make_synth(unsigned char *img, int seed) {
    for (int y = 0; y < IMG_H; y++) {
        for (int x = 0; x < IMG_W; x++) {
            double p = 3.14159265 * x / 7.0;
            double v = 128 + 90 * sin(p + seed * 0.7 + y * 0.05);
            img[y * IMG_W + x] = (unsigned char)v;
        }
    }
}

static unsigned char *load_raw(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *img = malloc(IMG_BYTES);
    size_t n = fread(img, 1, IMG_BYTES, f);
    fclose(f);
    if (n != IMG_BYTES) {
        fprintf(stderr, "[engine] %s: サイズが不正 (%zu != %d)\n", path, n, IMG_BYTES);
        free(img);
        return NULL;
    }
    return img;
}

static int save_file(const char *path, const unsigned char *p, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(p, 1, len, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *dll = NULL; /* --dll で指定。なければ既定位置から探索 */
    const char *mode = NULL;

    /* 引数: [--dll PATH] [--enroll img...] | [--verify tmpl img...] */
    int argi = 1;
    if (argc > 1 && strcmp(argv[1], "--dll") == 0 && argc > 2) {
        dll = argv[2];
        argi = 3;
    }
    if (argi < argc) {
        if (strcmp(argv[argi], "--enroll") == 0 || strcmp(argv[argi], "--verify") == 0) {
            mode = argv[argi];
            argi++;
        }
    }
    if (!dll) {
        /* 既定位置から探索: インストール先 → 開発用 vendor/ */
        const char *candidates[] = {
            "/usr/local/libexec/inmu-lock/ftWbioEngineAdapter.dll",
            "vendor/ftWbioEngineAdapter.dll",
            "../vendor/ftWbioEngineAdapter.dll",
        };
        for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
            if (access(candidates[i], R_OK) == 0) {
                dll = candidates[i];
                break;
            }
        }
    }
    if (!dll) {
        fprintf(stderr, "!! ftWbioEngineAdapter.dll が見つからない "
                        "(make vendor を実行、または --dll で指定)\n");
        return 2;
    }

    /* ---------- 実画像モード ---------- */
    if (mode && strcmp(mode, "--enroll") == 0) {
        if (argi >= argc) {
            fprintf(stderr, "usage: engine_test --enroll img1.bin [img2.bin ...]\n");
            return 2;
        }
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = crash_handler;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        fprintf(stderr, "=== 実画像で登録 (T2×T3 統合) ===\n");
        int r = ft_engine_open(dll);
        fprintf(stderr, "ft_engine_open() = %d\n", r);
        if (r != 0) return 1;

        ft_engine_enroll_begin();
        int accepted = 0, rejected = 0, complete = 0;
        for (int i = argi; i < argc; i++) {
            unsigned char *img = load_raw(argv[i]);
            if (!img) return 1;
            uint32_t hr = ft_engine_accept(img, IMG_W, IMG_H, 4 /* enroll */);
            if (hr != 0) {
                fprintf(stderr, "[%s] accept reject hr=%#x\n", argv[i], hr);
                rejected++;
                free(img);
                continue;
            }
            int st = ft_engine_enroll_update();
            fprintf(stderr, "[%s] accept OK → update=%d (0=完成 1=継続 2=reject)\n",
                    argv[i], st);
            if (st == 2) rejected++;
            else accepted++;
            free(img);
            if (st == 0) {
                complete = 1;
                break;
            }
        }
        if (!complete) {
            /* 画像を使い切ったが未完成 → そのまま commit を試みる (engine 判定) */
            fprintf(stderr, "画像を消費 (accepted=%d rejected=%d) — commit を試みる\n",
                    accepted, rejected);
        }
        unsigned char *tmpl = NULL;
        size_t tmpllen = 0;
        int cr = ft_engine_enroll_commit(&tmpl, &tmpllen);
        if (cr != 0 || !tmpl) {
            fprintf(stderr, "=> 登録失敗 (accepted=%d rejected=%d) ❌\n", accepted,
                    rejected);
            return 1;
        }
        save_file("ft_template.bin", tmpl, tmpllen);
        fprintf(stderr, "=> 登録完了 ✅ accepted=%d rejected=%d, template=%zu bytes "
                         "→ ft_template.bin\n",
                accepted, rejected, tmpllen);
        free(tmpl);
        fprintf(stderr, "=== 登録完了 ===\n");
        _exit(0);
    }

    if (mode && strcmp(mode, "--verify") == 0) {
        if (argi + 1 >= argc) {
            fprintf(stderr, "usage: engine_test --verify template.bin img1.bin [img2...]\n");
            return 2;
        }
        {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = crash_handler;
            sa.sa_flags = SA_SIGINFO;
            sigaction(SIGILL, &sa, NULL);
            sigaction(SIGSEGV, &sa, NULL);
            sigaction(SIGBUS, &sa, NULL);
        }
        fprintf(stderr, "=== 実画像で照合 (T2×T3 統合) ===\n");
        const char *tmpl_path = argv[argi++];
        FILE *tf = fopen(tmpl_path, "rb");
        if (!tf) {
            fprintf(stderr, "[engine] テンプレートが開けない: %s\n", tmpl_path);
            return 1;
        }
        fseek(tf, 0, SEEK_END);
        size_t tlen = ftell(tf);
        fseek(tf, 0, SEEK_SET);
        unsigned char *tmpl = malloc(tlen);
        if (fread(tmpl, 1, tlen, tf) != tlen) return 1;
        fclose(tf);

        int r = ft_engine_open(dll);
        if (r != 0) {
            fprintf(stderr, "ft_engine_open() = %d\n", r);
            return 1;
        }

        int match = 0, nomatch = 0, rej = 0;
        for (int i = argi; i < argc; i++) {
            unsigned char *img = load_raw(argv[i]);
            if (!img) return 1;
            uint32_t hr = ft_engine_accept(img, IMG_W, IMG_H, 1 /* verify */);
            if (hr != 0) {
                fprintf(stderr, "[%s] accept reject hr=%#x\n", argv[i], hr);
                rej++;
                free(img);
                continue;
            }
            int m = ft_engine_verify(tmpl, tlen);
            fprintf(stderr, "[%s] → %s\n", argv[i], m ? "MATCH ✅" : "no match ❌");
            if (m) match++;
            else nomatch++;
            free(img);
        }
        fprintf(stderr, "=== 照合結果: match=%d no_match=%d reject=%d ===\n", match,
                nomatch, rej);
        free(tmpl);
        _exit((match > 0) ? 0 : 1);
    }

    /* ---------- 従来: 合成画像モード ---------- */
    fprintf(stderr, "=== 検証C Stage 2: PEローダー + vendor matcher DLL (Rosetta 2) ===\n");
    fprintf(stderr, "DLL: %s\n", dll);

    int w = 0, h = 0;
    ft_engine_geometry(&w, &h);
    fprintf(stderr, "engine geometry: %dx%d\n", w, h);

    int r = ft_engine_open(dll);
    fprintf(stderr, "ft_engine_open() = %d\n", r);
    if (r != 0) {
        fprintf(stderr, "=> ロード失敗 ❌\n");
        return 1;
    }
    fprintf(stderr, "=> ロード成功: PE map + DllMain + Query + Attach すべて通過 ✅\n");

    static unsigned char img[IMG_BYTES];

    /* 1. 合成画像を accept (purpose=1: verify用の採取) */
    make_synth(img, 1);
    uint32_t hr = ft_engine_accept(img, IMG_W, IMG_H, 1);
    fprintf(stderr, "accept(synth, verify) hr = %#x %s\n", hr,
            hr == 0 ? "✅ (特徴抽出された)" : "(reject — 合成画像なら想定内)");

    /* 2. 登録フロー: begin → accept(enroll) → update ×n → commit */
    ft_engine_enroll_begin();
    for (int i = 0; i < 6; i++) {
        make_synth(img, i % 3); /* 3種の縞を交互に (多様性のあるキャプチャの模拟) */
        hr = ft_engine_accept(img, IMG_W, IMG_H, 4); /* purpose=4: enroll */
        fprintf(stderr, "  enroll accept #%d hr = %#x\n", i, hr);
        int up = ft_engine_enroll_update();
        fprintf(stderr, "  enroll update #%d = %d (0=完成 1=継続 2=reject)\n", i, up);
        if (up == 0) break;
    }
    uint8_t *tmpl = NULL;
    size_t tmpllen = 0;
    int cr = ft_engine_enroll_commit(&tmpl, &tmpllen);
    if (cr == 0 && tmpl) {
        fprintf(stderr, "enroll commit OK: template %zu bytes ✅ (vendor形式)\n", tmpllen);

        /* 3. 同一合成画像で照合 → 同じ画像だから一致するはず */
        make_synth(img, 0);
        hr = ft_engine_accept(img, IMG_W, IMG_H, 1);
        int m = ft_engine_verify(tmpl, tmpllen);
        fprintf(stderr, "verify(同じ縞) accept hr=%#x → match=%d %s\n", hr, m,
                m ? "✅ (一致)" : "(不一致 — reject濃度の合成画像ならあり得る)");

        /* 4. 別の縞で照合 → 不一致であることが望ましい (FRR/FAR の当て) */
        make_synth(img, 40);
        hr = ft_engine_accept(img, IMG_W, IMG_H, 1);
        int m2 = ft_engine_verify(tmpl, tmpllen);
        fprintf(stderr, "verify(別の縞) accept hr=%#x → match=%d %s\n", hr, m2,
                m2 ? "⚠️ (不一致のはずが一致 — FAR懸念)" : "✅ (不正解を拒否)");
        free(tmpl);
    } else {
        fprintf(stderr, "enroll commit = %d (合成画像のため reject は想定内)\n", cr);
    }

    fprintf(stderr, "=== Stage 2 完了: クラッシュせず全API通過 ✅ ===\n");
    /* %gs は偽TEB に固定したままなので、通常の exit (libc teardown) を
     * 経由せず _exit で終了する */
    _exit(0);
}
