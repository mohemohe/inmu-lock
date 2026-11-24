/*
 * stage0c_traprdx.c — 検証C補足: mach trap 0x3000003 の rdx (第2戻り値) に
 * 「旧ベース」が返っているかを確認するプローブ。
 *
 * 仮説: XNU の thread_set_tsd_base は kern_return 的な値を rax に (実際には
 * 新ベースの echo)、旧ベースを rdx (mach syscall の第2戻り値) に返す。
 *
 * 検証: 偽TEBへ切替えた際の rdx を取得 → それで復帰 → 復帰呼び出しの rdx
 * が偽TEBのアドレスと一致すれば「rdx = 旧ベース」が確定する。
 */

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint64_t u64;

static u64 g_fake_teb[512];
#define TEB_MAGIC 0xCAFEF00DBADF00DULL

static sigjmp_buf g_jmp;
static volatile int g_caught = 0;
static u64 g_libc_base;
static int g_have_base = 0;

/* rax = 戻り値1 (echo), rdx = 戻り値2 (旧ベースの候補) を両方取得 */
static void tsd_set_base2(void *base, u64 *rax_out, u64 *rdx_out) {
    __asm__ volatile(
        "movl $0x3000003, %%eax\n"
        "movl $0, %%esi\n"
        "syscall"
        : "=a"(*rax_out), "=d"(*rdx_out)
        : "D"(base)
        : "rcx", "r11", "rsi");
}

static void crash_handler(int sig) {
    g_caught = sig;
    if (g_have_base) {
        u64 ra, rd;
        tsd_set_base2((void *)(uintptr_t)g_libc_base, &ra, &rd);
    }
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

    /* 1. 切替え: rdx に旧ベース (libc基底) が返るか */
    u64 ra1, rd1;
    tsd_set_base2(g_fake_teb, &ra1, &rd1);
    printf("[stage0c] 切替え: rax=%#llx (echo想定), rdx=%#llx (旧ベース候補)\n",
           (unsigned long long)ra1, (unsigned long long)rd1);
    u64 m = gs_slot(0x30);
    printf("[stage0c] gs:0x30 = %#llx (%s)\n", (unsigned long long)m,
           m == TEB_MAGIC ? "偽TEB ✅" : "❌");

    /* 2. rdx の値で復帰 — 復帰呼び出しの rdx が偽TEBアドレスなら確定 */
    u64 ra2, rd2;
    tsd_set_base2((void *)(uintptr_t)rd1, &ra2, &rd2);
    printf("[stage0c] 復帰: rax=%#llx, rdx=%#llx (%s)\n", (unsigned long long)ra2,
           (unsigned long long)rd2,
           rd2 == (u64)(uintptr_t)g_fake_teb ? "★ rdx=旧ベース 確定 ✅" : "不一致 ❌");

    /* 3. 復帰状態で libc が動作するか (rdx が正しい基底の検証) */
    g_libc_base = rd1;
    g_have_base = 1;
    char *p = malloc(100);
    if (!p) {
        printf("[stage0c] malloc 失敗 ❌\n");
        return 1;
    }
    strcpy(p, "malloc OK");
    printf("[stage0c] rdx復帰で libc 動作: %s ✅\n", p);
    printf("[stage0c] pthread_self = %p (gs:0x00 参照)\n", pthread_self());

    /* 4. 往復ストレステスト: 1000回 切替え↔復帰 を malloc/free しながら */
    for (int i = 0; i < 1000; i++) {
        u64 ra, rd;
        tsd_set_base2(g_fake_teb, &ra, &rd);
        if (gs_slot(0x30) != TEB_MAGIC) {
            printf("[stage0c] !! ループ%d: 偽TEBが見えない\n", i);
            return 1;
        }
        tsd_set_base2((void *)(uintptr_t)rd, &ra, &rd);
        char *q = malloc(64);
        if (!q) {
            printf("[stage0c] !! ループ%d: malloc 失敗\n", i);
            return 1;
        }
        free(q);
    }
    printf("[stage0c] 1000往復 + malloc/free: 破損なし ✅\n");
    printf("[stage0c] 完了: libc基底(rdx) = %#llx\n", (unsigned long long)g_libc_base);
    return 0;
}
