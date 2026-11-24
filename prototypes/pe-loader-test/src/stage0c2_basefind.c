/*
 * stage0c2_basefind.c — 検証C補足: libc の %gs 基底 (TSDベース) を特定する。
 *
 * 確立済みの事実:
 *   - gs:0x00 (native) == pthread_t (=p)。基底そのものではない (stage0b)
 *   - mach trap 0x3000003 の rax は新ベースの echo (stage0)
 *   - 基底 B は *(B+0x00) == p を満たす (native観測と整合)
 *
 * 本プローブ (fork して子プロセスで実験 — 親は native gs を維持):
 *   仮説1: trap の rdx (第2戻り値) が旧ベース
 *   仮説2: 基底 B = p + K。pthread構造体内をスキャンし *(p+off)==p となる
 *          自己参照を見つける → trap(B) で gs:0x00==p が読めるか検証
 *   両者が失敗しても親プロセスは生きている。
 *
 * 出力は stderr (非バッファ) — クラッシュしてもどこまで進んだかわかる。
 */

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef uint64_t u64;

static u64 g_fake_teb[512] __attribute__((aligned(4096)));
#define TEB_MAGIC 0xCAFEF00DBADF00DULL

static void tsd_set_base2(void *base, u64 *rax_out, u64 *rdx_out) {
    __asm__ volatile(
        "movl $0x3000003, %%eax\n"
        "movl $0, %%esi\n"
        "syscall"
        : "=a"(*rax_out), "=d"(*rdx_out)
        : "D"(base)
        : "rcx", "r11", "rsi");
}

static u64 gs_slot(int off) {
    u64 v;
    __asm__ volatile("movq %%gs:(%q1), %0" : "=r"(v) : "r"((u64)(uintptr_t)off));
    return v;
}

int main(void) {
    *(u64 *)((char *)g_fake_teb + 0x30) = TEB_MAGIC;

    pid_t pid = fork();
    if (pid == 0) {
        /* --- 子プロセス: 実験 --- */
        u64 p = gs_slot(0x00);
        dprintf(2, "[child] pthread_t (gs:0x00) = %#llx\n", (unsigned long long)p);
        dprintf(2, "[child] fake_teb addr        = %p\n", (void *)g_fake_teb);

        /* --- 仮説2: p+K の自己参照をスキャン (まず native gs のまま安全に) --- */
        u64 candidate = 0;
        for (u64 off = 0x08; off < 0x400; off += 8) {
            u64 v = *(u64 *)(uintptr_t)(p + off);
            if (v == p) {
                dprintf(2, "[child] 自己参照発見: *(p+%#llx) == p → 基底候補 p+%#llx\n",
                        (unsigned long long)off, (unsigned long long)off);
                candidate = p + off;
                break;
            }
        }
        if (!candidate) {
            dprintf(2, "[child] p+K に自己参照なし — TSDは別割り当てか\n");
        }

        /* --- 仮説1: trap の rdx --- */
        u64 ra1, rd1;
        tsd_set_base2(g_fake_teb, &ra1, &rd1);
        dprintf(2, "[child] trap(fake): rax=%#llx rdx=%#llx\n",
                (unsigned long long)ra1, (unsigned long long)rd1);
        u64 m = gs_slot(0x30);
        dprintf(2, "[child] gs:0x30=%#llx (%s)\n", (unsigned long long)m,
                m == TEB_MAGIC ? "偽TEB✅" : "❌");

        /* rdx を検証: gs=rdx にして gs:0x00==p ? */
        u64 ra2, rd2;
        tsd_set_base2((void *)(uintptr_t)rd1, &ra2, &rd2);
        u64 s0 = gs_slot(0x00);
        dprintf(2, "[child] trap(rdx): rax=%#llx rdx=%#llx (fake一致? %s)\n",
                (unsigned long long)ra2, (unsigned long long)rd2,
                rd2 == (u64)(uintptr_t)g_fake_teb ? "★旧ベース説✅" : "✗");
        dprintf(2, "[child] gs=rdx での gs:0x00=%#llx (p と一致? %s)\n",
                (unsigned long long)s0, s0 == p ? "✅ rdxは基底!" : "✗ rdxは基底ではない");

        if (s0 == p) {
            /* rdx が基底 → libc 動作テスト */
            char *q = malloc(100);
            if (q) {
                strcpy(q, "OK");
                dprintf(2, "[child] rdx基底で malloc ✅\n");
                free(q);
            } else {
                dprintf(2, "[child] rdx基底で malloc 失敗\n");
            }
            _exit(0);
        }

        /* rdx が基底でない → 仮説2の候補で復帰を試みる */
        if (candidate) {
            u64 ra3, rd3;
            tsd_set_base2((void *)(uintptr_t)candidate, &ra3, &rd3);
            u64 s1 = gs_slot(0x00);
            dprintf(2, "[child] trap(p+%#llx): gs:0x00=%#llx (p一致? %s)\n",
                    (unsigned long long)(candidate - p), (unsigned long long)s1,
                    s1 == p ? "✅" : "✗");
            char *q = malloc(100);
            if (q && s1 == p) {
                strcpy(q, "OK");
                dprintf(2, "[child] スキャン候補で malloc ✅\n");
                free(q);
                _exit(0);
            }
            dprintf(2, "[child] スキャン候補では libc 動作せず\n");
        }
        _exit(99);
    }

    /* --- 親プロセス --- */
    int status = 0;
    waitpid(pid, &status, 0);
    dprintf(2, "[parent] 子の終了: status=%#x (exit=%d sig=%d)\n", status,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
