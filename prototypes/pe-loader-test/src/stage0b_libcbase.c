/*
 * stage0b_libcbase.c — 検証C補足: libSystem の %gs 基底 (TLSベース) を
 * 「値として」取得できるかを調べるプローブ。
 *
 * 背景: sh_HeapAlloc 等の shim が malloc を gs=TEB のまま呼び続けると
 * 破損が蓄積し、libSystem 内部の整合性チェックが意図的トラップ (SIGILL)
 * に至る (実画像処理で観測)。→ shim が libc を呼ぶ間だけ %gs を
 * libc基底へ戻す「gs切替え規律」が必要。
 * そのためには libc基底の「値」が必要。
 *
 * 仮説: Darwin x86_64 の TLS 領域 gs:0x00 に自己参照ポインタ (== gsbase)
 * が入っている → それを読めば基底が得られる。
 *
 * 確認手順:
 *   1. v0 = gs:0x00 を読む (通常TLS状態)
 *   2. gs を偽TEBへ切替え (magic が見えることを確認)
 *   3. gs を v0 へ戻す → gs:0x00 が v0 なら自己参照確定 = v0 が基底
 *   4. その状態で malloc/printf が動けば v0 は「機能するlibc基底」
 */

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef uint32_t u32;
typedef uint64_t u64;

static u64 g_fake_teb[512];
#define TEB_MAGIC 0xCAFEF00DBADF00DULL

static sigjmp_buf g_jmp;
static volatile int g_caught = 0;
static u64 g_saved_v0;
static int g_have_v0 = 0;

static u64 tsd_set_base(void *base) {
    u64 ret;
    __asm__ volatile(
        "movl $0x3000003, %%eax\n"
        "movl $0, %%esi\n"
        "syscall"
        : "=a"(ret)
        : "D"(base)
        : "rcx", "r11", "rsi");
    return ret;
}

static void crash_handler(int sig) {
    g_caught = sig;
    if (g_have_v0) tsd_set_base((void *)(uintptr_t)g_saved_v0);
    siglongjmp(g_jmp, 1);
}

static u64 gs_slot(int off) {
    u64 v;
    __asm__ volatile("movq %%gs:(%q1), %0" : "=r"(v) : "r"((u64)(uintptr_t)off));
    return v;
}

int main(void) {
    *(u64 *)((char *)g_fake_teb + 0x30) = TEB_MAGIC;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    /* 1. 通常TLS状態で gs スロットをダンプ */
    u64 v0 = gs_slot(0x00);
    printf("[stage0b] pthread_self = %p\n", pthread_self());
    printf("[stage0b] gs:0x00 = %#llx (自己参照候補)\n", (unsigned long long)v0);
    {
        int offs[] = {0x00, 0x08, 0x10, 0x30, 0x48, 0x58, 0x60};
        for (size_t i = 0; i < sizeof offs / sizeof *offs; i++)
            printf("    gs:%#04x = %#llx\n", offs[i],
                   (unsigned long long)gs_slot(offs[i]));
    }
    fflush(stdout);

    /* 2. 偽TEBへ切替え */
    g_caught = 0;
    if (sigsetjmp(g_jmp, 1) == 0) {
        tsd_set_base(g_fake_teb);
        u64 m = gs_slot(0x30);
        printf("[stage0b] 切替え後 gs:0x30 = %#llx (%s)\n", (unsigned long long)m,
               m == TEB_MAGIC ? "偽TEB ✅" : "❌");
    } else {
        printf("[stage0b] 切替えでフォルト SIG=%d\n", g_caught);
        return 1;
    }

    /* 3. v0 へ戻す — 自己参照かどうかと、libc が動くか */
    g_saved_v0 = v0;
    g_have_v0 = 1;
    g_caught = 0;
    if (sigsetjmp(g_jmp, 1) == 0) {
        tsd_set_base((void *)(uintptr_t)v0);
        u64 back = gs_slot(0x00);
        printf("[stage0b] v0へ復帰後 gs:0x00 = %#llx (%s)\n", (unsigned long long)back,
               back == v0 ? "自己参照確定 → v0 が libc基底 ✅" : "v0 は基底ではない ❌");
        /* 4. 機能確認: malloc/printf */
        char *p = malloc(100);
        if (p) {
            strcpy(p, "malloc OK");
            printf("[stage0b] v0 で libc 動作: %s ✅\n", p);
        }
    } else {
        printf("[stage0b] v0 復帰でフォルト SIG=%d → v0 は基底ではない ❌\n", g_caught);
        return 1;
    }

    printf("[stage0b] 完了: libc基底 = %#llx\n", (unsigned long long)v0);
    return 0;
}
