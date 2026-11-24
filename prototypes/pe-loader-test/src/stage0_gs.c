/*
 * stage0_gs.c — 検証C Stage 0: Rosetta 2 (x86_64) 上で %gs ベース (TEB) を
 * 制御できるかを調べるプローブ。(v2)
 *
 * v1 での実測 (2026-11-24):
 *   - Rosetta 2 翻訳実行: YES
 *   - FSGSBASE (wrgsbase/rdgsbase): CPUID bit16=0、rdgsbase は SIGILL → 使用不可
 *   - mach trap 0x3000003 (thread_set_tsd_base): %gs 切替えに成功
 *     (gs:0x30 から偽TEBのマジックが読めた = Rosetta は gsbase 変更を反映する)
 *   - 戻り値 rax はポインタ様の値 (0x100DB7010) → 「旧ベース」の可能性が高い
 *   - v1 のバグ: 復元に pthread_self() (≠gsbase) を使ったため %gs が壊れたまま
 *     malloc が SIGSEGV → シグナルハンドラの longjmp がループした
 *
 * v2 の確認項目:
 *   1. 翻訳実行 / CPUID FSGSBASE (参考)
 *   2. trap の戻り値 == 旧ベースであることの検証
 *      (切替え→復元→もう一度切替えで、2回目の戻り値が偽TEBのアドレスになる)
 *   3. 復元後の libSystem (malloc/stdio) 健全性
 *   4. gs 切替え中は libSystem を呼ばない規律の実証
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
static volatile int g_caught_sig = 0;
static volatile int g_fault_count = 0;

/* __thread_set_tsd_base 相当: mach trap 0x3000003 (rdi=base, esi=0)。
 * 戻り値 (rax) は旧ベースの可能性 (v1 の実測に基づく)。
 * %gs を読まないので %gs が壊れている状態からでも呼べる。 */
static u64 set_tsd_base(void *base) {
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
    /* 暴走ループ防止: 3回までは復元して続行、それ以上は諦めて終了 */
    if (++g_fault_count > 3) {
        static const char msg[] = "\n[stage0] フォルト多発のため終了\n";
        write(2, msg, sizeof msg - 1);
        _exit(70);
    }
    g_caught_sig = sig;
    siglongjmp(g_jmp, 1);
}

static u64 gs_0x30(void) {
    u64 v;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(v));
    return v;
}

static int translated(void) {
    int ret = 0;
    size_t len = sizeof(ret);
    sysctlbyname("sysctl.proc_translated", &ret, &len, NULL, 0);
    return ret;
}

int main(void) {
    *(u64 *)((char *)g_fake_teb + 0x30) = TEB_MAGIC;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    printf("[stage0] Rosetta 2 翻訳実行: %s\n",
           translated() ? "YES (x86_64 → arm64)" : "NO (ネイティブ?)");
    {
        u32 ecx = 0;
        __asm__ volatile("cpuid" : "=c"(ecx) : "a"((u32)1) : "ebx", "edx");
        printf("[stage0] CPUID FSGSBASE(bit16)=%d (0=命令使用不可)\n", (ecx >> 16) & 1);
    }
    fflush(stdout);

    /* --- trap の戻り値 == 旧ベース の検証と、正しい復元 --- */
    g_caught_sig = 0;
    g_fault_count = 0;
    if (sigsetjmp(g_jmp, 1) == 0) {
        u64 old_base = set_tsd_base(g_fake_teb);   /* 切替え。戻り値=旧ベースのはず */
        u64 v1 = gs_0x30();                        /* 偽TEBが見えるはず */
        u64 old2 = set_tsd_base((void *)(uintptr_t)old_base); /* 復元 (戻り値で検証) */
        u64 v2 = gs_0x30();                        /* もう偽TEBは見えないはず */
        printf("[stage0] 切替え: gs:0x30 = %#llx (%s)\n",
               (unsigned long long)v1,
               v1 == TEB_MAGIC ? "偽TEBが見えた ✅" : "見えない ❌");
        printf("[stage0] 旧ベース候補: %#llx, 復元時の戻り値: %#llx (%s)\n",
               (unsigned long long)old_base, (unsigned long long)old2,
               old2 == (u64)(uintptr_t)g_fake_teb ? "戻り値=旧ベースと判明 ✅"
                                                  : "戻り値の意味は要検討: " "?");
        (void)v2;
        fflush(stdout);

        /* --- 復元後の libSystem 健全性 --- */
        char *p = malloc(64);
        if (p == NULL) {
            printf("[stage0] malloc 失敗 ❌\n");
            return 1;
        }
        strcpy(p, "malloc/printf OK");
        printf("[stage0] 復元後 libSystem: %s ✅\n", p);
        free(p);

        /* --- 2度目の切替えサイクル (往復の安定性) --- */
        {
            u64 save = set_tsd_base(g_fake_teb);
            u64 v3 = gs_0x30();
            set_tsd_base((void *)(uintptr_t)save);
            char *q = malloc(32);
            strcpy(q, "cycle2 OK");
            printf("[stage0] 2度目の切替え: %s / %s\n",
                   v3 == TEB_MAGIC ? "偽TEB ✅" : "❌", q);
            free(q);
        }
    } else {
        printf("[stage0] 例外 SIG=%d → %s\n", g_caught_sig,
               "切替え/復元中にフォルト ❌");
    }
    fflush(stdout);

    printf("[stage0] 完了\n");
    return 0;
}
