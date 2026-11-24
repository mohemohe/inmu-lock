// ft_engine_mac.c — ftWbioEngineAdapter.dll (x86-64 PE, FocalTech の
// Windows 用マッチングエンジン) を macOS (Rosetta 2) 上で in-process 実行する。
//
// OMGrant/ft9201-libfprint の src/ft_engine.c (LGPL-2.1-or-later) の
// macOS x86_64 移植。元の Linux 実装の前提と、本移植での確定事項:
//
//   Linux 版: glibc は %fs を TLS に使う → %gs が空いている。
//             arch_prctl(ARCH_SET_GS) で偽 Windows TEB を %gs へ。
//             memfd_create + mmap で W^X を維持した PE マッピング。
//
//   macOS x86_64 (Rosetta 2) での実測 (検証C Stage 0, 2026-11-24):
//     1. FSGSBASE (wrgsbase/rdgsbase) は使用不可 (CPUID bit16=0, SIGILL)。
//     2. mach trap 0x3000003 (thread_set_tsd_base) で %gs を切替えられる。
//        (libsystem_kernel の __thread_set_tsd_base スタブと同一の呼び出し方。
//         戻り値 rax は「設定した新しいベース」の echo)
//     3. Rosetta 2 は gsbase の変更を正しく反映する (gs:0x30 から偽TEBが読めた)。
//     4. macOS の libSystem は %gs を TLS に使うが、偽TEB (ゼロ埋めBSS) を
//        指したままでも malloc/printf 等は正常動作する。
//        → 本移植ではロード後に %gs を TEB へ固定し、復元しない
//          (プロセスは _exit で終了。他スレッドの %gs は影響を受けない)。
//
//   その他の移植: memfd_create → mkstemp + mmap + unlink,
//                MAP_FIXED_NOREPLACE → MAP_FIXED (推奨ベース 0x180000000 は
//                競合しない前提。元実装と同じくリロケーションは処理しない)
//
// ラセンス: LGPL-2.1-or-later (ft9201-libfprint 由来の部分)

#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define MS __attribute__((ms_abi))
#define DLL_PROCESS_ATTACH 1

static u8 *g_image;      /* mapped image base */
static u64 g_imagebase;  /* preferred base 0x180000000 */
static const char *g_dllpath;

// ---------- tiny PE structures (only fields we use) ----------
#pragma pack(push, 1)
typedef struct {
    u16 e_magic;
    u8 pad[58];
    u32 e_lfanew;
} DOS;
typedef struct {
    u32 Signature;
    u16 Machine, NumSecs;
    u32 TimeStamp, PtrSym, NumSym;
    u16 OptSize, Chars;
} FILEHDR;
typedef struct {
    u8 Name[8];
    u32 VSize, VAddr, RawSize, RawPtr;
    u32 pad[3];
    u16 pad2[2];
    u32 Chars;
} SEC;
#pragma pack(pop)

static u8 *g_file;
static long g_filelen;
static u32 f32(u64 o) { return *(u32 *)(g_file + o); }
static u64 f64(u64 o) { return *(u64 *)(g_file + o); }
static u16 f16(u64 o) { return *(u16 *)(g_file + o); }

// ---------- %gs 制御 (mach trap 0x3000003 / thread_set_tsd_base) ----------
/* libsystem_kernel.dylib の __thread_set_tsd_base スタブと同一:
 *   rax = 0x3000003, rdi = 新ベース, rsi = 0, syscall
 * 戻り値 rax は設定したベースの echo (検証C Stage 0 実測)。 */
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

static int set_gs(void *base) {
    return tsd_set_base(base) == (u64)(uintptr_t)base ? 0 : -1;
}

// ---------- fake TEB / PEB / TLS ----------
static u8 g_teb[0x2000];
static u8 g_peb[0x1000];
static void *g_tls_array[64];
static u8 g_tls_block[0x2000];

// ---------- gs 切替え規律 (検証C stage0c2 で確立) ----------
// macOS の libSystem は %gs (TLS) を malloc/stdio 等で使うため、shim 内で
// libc を呼ぶ間だけ %gs を libc 基底へ戻す。libc 基底は pthread 構造体内の
// 自己参照 (*(p+K)==p) をスキャンして動的に特定する (macOS 27 で K=0xE0)。

static u64 g_libc_base = 0; /* TSD基底 (pthread構造体内) */

static void gs_to_teb(void) {
    tsd_set_base(g_teb);
}

static void gs_to_libc(void) {
    if (g_libc_base) tsd_set_base((void *)(uintptr_t)g_libc_base);
}

static u64 gs_slot(int off) {
    u64 v;
    __asm__ volatile("movq %%gs:(%q1), %0" : "=r"(v) : "r"((u64)(uintptr_t)off));
    return v;
}

/* native gs 状態で呼ぶこと。pthread 構造体内の自己参照から TSD 基底を特定。 */
static int discover_libc_base(void) {
    u64 p = gs_slot(0x00); /* == pthread_t */
    for (u64 off = 8; off < 0x400; off += 8) {
        if (*(u64 *)(uintptr_t)(p + off) == p) {
            u64 cand = p + off;
            /* 自己検証: 基底に設定して gs:0x00 が p を返すか */
            tsd_set_base((void *)(uintptr_t)cand);
            if (gs_slot(0x00) == p) {
                g_libc_base = cand;
                return 0;
            }
        }
    }
    return -1;
}

// ---------- import shim table ----------
typedef struct {
    const char *name;
    void *fn;
} Shim;
static Shim shims[128];
static int nshims;
static void reg(const char *n, void *f) {
    if (nshims >= (int)(sizeof shims / sizeof *shims)) {
        fprintf(stderr, "ft_engine: shim table full (%d) — raise shims[]\n", nshims);
        abort();
    }
    shims[nshims].name = n;
    shims[nshims].fn = f;
    nshims++;
}
static void *find_shim(const char *n) {
    for (int i = 0; i < nshims; i++)
        if (!strcmp(shims[i].name, n)) return shims[i].fn;
    return 0;
}

// ================= SHIMS (all MS ABI) =================
static u32 g_lasterr = 0;
static void MS sh_SetLastError(u32 e) { g_lasterr = e; }
static u32 MS sh_GetLastError(void) { return g_lasterr; }

static void *MS sh_GetProcessHeap(void) { return (void *)0x100; }
static void *MS sh_HeapAlloc(void *h, u32 flags, u64 size) {
    (void)h;
    gs_to_libc(); /* malloc は TLS (%gs) を使うため必須 */
    void *p = malloc(size ? size : 1);
    if (p && (flags & 8)) memset(p, 0, size);
    gs_to_teb();
    return p;
}
static int MS sh_HeapFree(void *h, u32 f, void *p) {
    (void)h;
    (void)f;
    gs_to_libc();
    free(p);
    gs_to_teb();
    return 1;
}
static void *MS sh_HeapReAlloc(void *h, u32 f, void *p, u64 s) {
    (void)h;
    (void)f;
    gs_to_libc();
    void *np = realloc(p, s);
    gs_to_teb();
    return np;
}
static u64 MS sh_HeapSize(void *h, u32 f, void *p) {
    (void)h;
    (void)f;
    (void)p;
    return 0;
}
static int MS sh_HeapValidate(void *h, u32 f, void *p) {
    (void)h;
    (void)f;
    (void)p;
    return 1;
}
static int MS sh_HeapQueryInformation(void *h, int c, void *b, u64 l, u64 *r) {
    (void)h;
    (void)c;
    (void)b;
    (void)l;
    if (r) *r = 0;
    return 0;
}

static void MS sh_InitCS(void *p) { (void)p; }
static u32 MS sh_InitCSSpin(void *p, u32 s) {
    (void)p;
    (void)s;
    return 1;
}
static void MS sh_EnterCS(void *p) { (void)p; }
static void MS sh_LeaveCS(void *p) { (void)p; }
static void MS sh_DeleteCS(void *p) { (void)p; }
static void MS sh_InitSList(void *p) { memset(p, 0, 16); }
static void *MS sh_FlushSList(void *p) {
    (void)p;
    return 0;
}

// TLS
static u32 g_tls_next = 1;
static void *g_tls_vals[1088];
static u32 MS sh_TlsAlloc(void) { return g_tls_next++; }
static int MS sh_TlsFree(u32 i) {
    (void)i;
    return 1;
}
static void *MS sh_TlsGetValue(u32 i) {
    g_lasterr = 0;
    return i < 1088 ? g_tls_vals[i] : 0;
}
static int MS sh_TlsSetValue(u32 i, void *v) {
    if (i < 1088) g_tls_vals[i] = v;
    return 1;
}

// process/thread/module
static void *MS sh_GetCurrentProcess(void) { return (void *)-1; }
static u32 MS sh_GetCurrentProcessId(void) { return (u32)getpid(); }
static u32 MS sh_GetCurrentThreadId(void) { return 5678; }
static void *MS sh_GetModuleHandleW(void *n) {
    (void)n;
    return (void *)g_image;
}
static void *MS sh_GetModuleHandleA(void *n) {
    (void)n;
    return (void *)g_image;
}
static int MS sh_GetModuleHandleExW(u32 f, void *n, void **h) {
    (void)f;
    (void)n;
    if (h) *h = (void *)g_image;
    return 1;
}
static u32 MS sh_GetModuleFileNameA(void *m, char *b, u32 n) {
    (void)m;
    strncpy(b, g_dllpath, n);
    return (u32)strlen(g_dllpath);
}
static u32 MS sh_GetModuleFileNameW(void *m, u16 *b, u32 n) {
    (void)m;
    u32 i = 0;
    for (; g_dllpath[i] && i < n - 1; i++) b[i] = (u16)(u8)g_dllpath[i];
    b[i] = 0;
    return i;
}
static void *MS sh_LoadLibraryExW(void *n, void *h, u32 f) {
    (void)n;
    (void)h;
    (void)f;
    return (void *)0x1000;
}
static int MS sh_FreeLibrary(void *h) {
    (void)h;
    return 1;
}
static void *MS sh_GetProcAddress(void *h, const char *n) {
    (void)h;
    if ((u64)(uintptr_t)n >> 16 == 0) return 0; /* by ordinal */
    return find_shim(n);
}

// console/file → route engine logs to stderr so we SEE its [Engine] output
static void *MS sh_GetStdHandle(u32 n) { return (void *)(u64)(n ? n : 1); }
static int MS sh_SetStdHandle(u32 n, void *h) {
    (void)n;
    (void)h;
    return 1;
}
static u32 MS sh_GetFileType(void *h) {
    (void)h;
    return 2; /* FILE_TYPE_CHAR */
}
static int MS sh_GetConsoleMode(void *h, u32 *m) {
    (void)h;
    if (m) *m = 0;
    return 1;
}
static u32 MS sh_GetConsoleCP(void) { return 437; }
static int MS sh_WriteFile(void *h, const void *b, u32 n, u32 *wr, void *o) {
    (void)h;
    (void)o;
    gs_to_libc(); /* stdio も TLS を使う可能性があるためスワップ */
    fwrite(b, 1, n, stderr);
    gs_to_teb();
    if (wr) *wr = n;
    return 1;
}
static int MS sh_WriteConsoleW(void *h, const u16 *b, u32 n, u32 *wr, void *r) {
    (void)h;
    (void)r;
    gs_to_libc();
    for (u32 i = 0; i < n; i++) fputc(b[i] & 0xff, stderr);
    gs_to_teb();
    if (wr) *wr = n;
    return 1;
}
static void MS sh_OutputDebugStringA(const char *s) {
    gs_to_libc();
    fprintf(stderr, "%s", s ? s : "");
    gs_to_teb();
}
static void MS sh_OutputDebugStringW(const u16 *s) {
    gs_to_libc();
    if (s)
        for (; *s; s++) fputc(*s & 0xff, stderr);
    gs_to_teb();
}
static int MS sh_FlushFileBuffers(void *h) {
    (void)h;
    return 1;
}
static void *MS sh_CreateFileW(void *n, u32 a, u32 s, void *sa, u32 c, u32 fl, void *t) {
    (void)n;
    (void)a;
    (void)s;
    (void)sa;
    (void)c;
    (void)fl;
    (void)t;
    g_lasterr = 2;
    return (void *)-1; /* config absent → graceful */
}
static int MS sh_SetFilePointerEx(void *h, long long d, void *np, u32 m) {
    (void)h;
    (void)d;
    (void)np;
    (void)m;
    return 0;
}
static int MS sh_CloseHandle(void *h) {
    (void)h;
    return 1;
}
static void *MS sh_FindFirstFileExA(void *n, int i, void *d, int o, void *s, u32 f) {
    (void)n;
    (void)i;
    (void)d;
    (void)o;
    (void)s;
    (void)f;
    return (void *)-1;
}
static int MS sh_FindNextFileA(void *h, void *d) {
    (void)h;
    (void)d;
    return 0;
}
static int MS sh_FindClose(void *h) {
    (void)h;
    return 1;
}
static u32 MS sh_GetSystemDirectoryA(char *b, u32 n) {
    strncpy(b, "C:\\Windows\\System32", n);
    return 19;
}

// sync / events → the engine's global lock
static void *MS sh_CreateEventW(void *sa, int man, int init, void *name) {
    (void)sa;
    (void)man;
    (void)init;
    (void)name;
    return (void *)0x2000;
}
static int MS sh_SetEvent(void *h) {
    (void)h;
    return 1;
}
static int MS sh_ResetEvent(void *h) {
    (void)h;
    return 1;
}
static u32 MS sh_WaitForSingleObject(void *h, u32 ms) {
    (void)h;
    (void)ms;
    return 0; /* WAIT_OBJECT_0 */
}
static u32 MS sh_WaitForSingleObjectEx(void *h, u32 ms, int a) {
    (void)h;
    (void)ms;
    (void)a;
    return 0;
}
static void *MS sh_CreateThread(void *sa, u64 st, void *fn, void *arg, u32 fl, u32 *id) {
    (void)sa;
    (void)st;
    (void)fn;
    (void)arg;
    (void)fl;
    if (id) *id = 4321;
    return (void *)0x3000;
}
static int MS sh_DeviceIoControl(void *h, u32 c, void *ib, u32 il, void *ob, u32 ol, u32 *ret, void *ov) {
    (void)h;
    (void)c;
    (void)ib;
    (void)il;
    (void)ob;
    (void)ol;
    (void)ov;
    if (ret) *ret = 0;
    return 0;
}

// locale / codepage (ucrt init)
static u32 MS sh_GetACP(void) { return 1252; }
static u32 MS sh_GetOEMCP(void) { return 437; }
static int MS sh_GetCPInfo(u32 cp, void *info) {
    (void)cp;
    if (info) {
        u8 *p = info;
        *(u32 *)p = 1;
        p[4] = 0;
        p[5] = 0;
    }
    return 1;
}
static int MS sh_IsValidCodePage(u32 cp) {
    (void)cp;
    return 1;
}
static int MS sh_MultiByteToWideChar(u32 cp, u32 f, const char *mb, int mbc, u16 *wc, int wcc) {
    (void)cp;
    (void)f;
    if (mbc < 0) mbc = (int)strlen(mb) + 1;
    if (wcc == 0) return mbc;
    int n = mbc < wcc ? mbc : wcc;
    for (int i = 0; i < n; i++) wc[i] = (u8)mb[i];
    return n;
}
static int MS sh_WideCharToMultiByte(u32 cp, u32 f, const u16 *wc, int wcc, char *mb, int mbc, void *d, void *u) {
    (void)cp;
    (void)f;
    (void)d;
    (void)u;
    if (wcc < 0) {
        wcc = 0;
        while (wc[wcc]) wcc++;
        wcc++;
    }
    if (mbc == 0) return wcc;
    int n = wcc < mbc ? wcc : mbc;
    for (int i = 0; i < n; i++) mb[i] = (char)wc[i];
    return n;
}
static int MS sh_LCMapStringW(u32 l, u32 f, const u16 *s, int sc, u16 *d, int dc) {
    (void)l;
    (void)f;
    if (dc == 0) return sc;
    int n = sc < dc ? sc : dc;
    for (int i = 0; i < n; i++) d[i] = s[i];
    return n;
}
static int MS sh_GetStringTypeW(u32 t, const u16 *s, int c, u16 *out) {
    (void)t;
    (void)s;
    for (int i = 0; i < c; i++) out[i] = 0;
    return 1;
}

// misc / version / cpu
static void MS sh_GetSystemInfo(void *p) {
    memset(p, 0, 64);
    u8 *b = p;
    *(u32 *)(b + 0) = 9;
    *(u32 *)(b + 4) = 4096;
}
static void MS sh_GetSystemTimeAsFileTime(u64 *ft) {
    static u64 t = 0x01d0000000000000ULL;
    *ft = (t += 100000);
}
static u32 MS sh_GetTickCount(void) {
    static u32 t = 1000;
    return (t += 5);
}
static int MS sh_QueryPerformanceCounter(u64 *c) {
    static u64 t = 0;
    *c = (t += 1000);
    return 1;
}
static int MS sh_IsDebuggerPresent(void) { return 0; }
static int MS sh_IsProcessorFeaturePresent(u32 f) {
    (void)f;
    return 1;
}
static void *MS sh_EncodePointer(void *p) { return p; }
static void *MS sh_DecodePointer(void *p) { return p; }
static u64 MS sh_VerSetConditionMask(u64 m, u32 t, u8 c) {
    (void)t;
    (void)c;
    return m;
}
static int MS sh_VerifyVersionInfoA(void *vi, u32 tm, u64 cm) {
    (void)vi;
    (void)tm;
    (void)cm;
    return 1;
}
static char g_cmd[] = "loader";
static u16 g_cmdw[] = {'l', 'o', 'a', 'd', 'e', 'r', 0};
static char *MS sh_GetCommandLineA(void) { return g_cmd; }
static u16 *MS sh_GetCommandLineW(void) { return g_cmdw; }
static void MS sh_GetStartupInfoW(void *si) {
    memset(si, 0, 104);
    *(u32 *)si = 104;
}
static u16 g_env[2] = {0, 0};
static u16 *MS sh_GetEnvironmentStringsW(void) { return g_env; }
static int MS sh_FreeEnvironmentStringsW(void *p) {
    (void)p;
    return 1;
}

// exception/unwind (only hit if the engine throws — hope not)
static void MS sh_RtlCaptureContext(void *ctx) { memset(ctx, 0, 1232); }
static void *MS sh_RtlLookupFunctionEntry(u64 pc, u64 *base, void *hist) {
    (void)pc;
    (void)hist;
    if (base) *base = g_imagebase;
    return 0;
}
static void *MS sh_RtlVirtualUnwind(u32 t, u64 b, u64 pc, void *fe, void *ctx, void **hd, u64 *est, void *ctxp) {
    (void)t;
    (void)b;
    (void)pc;
    (void)fe;
    (void)ctx;
    (void)hd;
    (void)est;
    (void)ctxp;
    return 0;
}
static void MS sh_RtlUnwindEx(void *a, void *b, void *c, void *d, void *e, void *f) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
}
static void *MS sh_RtlPcToFileHeader(void *pc, void **base) {
    (void)pc;
    if (base) *base = (void *)g_image;
    return (void *)g_image;
}
static void *MS sh_SetUnhandledExceptionFilter(void *f) {
    (void)f;
    return 0;
}
static u32 MS sh_UnhandledExceptionFilter(void *p) {
    (void)p;
    return 1;
}
static void MS sh_RaiseException(u32 code, u32 fl, u32 n, void *a) {
    (void)fl;
    (void)n;
    (void)a;
    fprintf(stderr, "[!] RaiseException code=%#x\n", code);
}
static void MS sh_TerminateProcess(void *h, u32 c) {
    (void)h;
    fprintf(stderr, "[!] TerminateProcess(%u)\n", c);
}
static void MS sh_ExitProcess(u32 c) {
    fprintf(stderr, "[!] ExitProcess(%u)\n", c);
}

static void register_shims(void) {
#define R(n) reg(#n, (void *)sh_##n)
    R(SetLastError);
    R(GetLastError);
    R(GetProcessHeap);
    R(HeapAlloc);
    R(HeapFree);
    R(HeapReAlloc);
    R(HeapSize);
    R(HeapValidate);
    R(HeapQueryInformation);
    reg("InitializeCriticalSection", (void *)sh_InitCS);
    reg("InitializeCriticalSectionAndSpinCount", (void *)sh_InitCSSpin);
    reg("EnterCriticalSection", (void *)sh_EnterCS);
    reg("LeaveCriticalSection", (void *)sh_LeaveCS);
    reg("DeleteCriticalSection", (void *)sh_DeleteCS);
    reg("InitializeSListHead", (void *)sh_InitSList);
    reg("InterlockedFlushSList", (void *)sh_FlushSList);
    R(TlsAlloc);
    R(TlsFree);
    R(TlsGetValue);
    R(TlsSetValue);
    R(GetCurrentProcess);
    R(GetCurrentProcessId);
    R(GetCurrentThreadId);
    R(GetModuleHandleW);
    R(GetModuleHandleA);
    R(GetModuleHandleExW);
    R(GetModuleFileNameA);
    R(GetModuleFileNameW);
    R(LoadLibraryExW);
    R(FreeLibrary);
    R(GetProcAddress);
    R(GetStdHandle);
    R(SetStdHandle);
    R(GetFileType);
    R(GetConsoleMode);
    R(GetConsoleCP);
    R(WriteFile);
    R(WriteConsoleW);
    R(OutputDebugStringA);
    R(OutputDebugStringW);
    R(FlushFileBuffers);
    R(CreateFileW);
    R(SetFilePointerEx);
    R(CloseHandle);
    R(FindFirstFileExA);
    R(FindNextFileA);
    R(FindClose);
    R(GetSystemDirectoryA);
    R(CreateEventW);
    R(SetEvent);
    R(ResetEvent);
    R(WaitForSingleObject);
    R(WaitForSingleObjectEx);
    R(CreateThread);
    R(DeviceIoControl);
    R(GetACP);
    R(GetOEMCP);
    R(GetCPInfo);
    R(IsValidCodePage);
    R(MultiByteToWideChar);
    R(WideCharToMultiByte);
    R(LCMapStringW);
    R(GetStringTypeW);
    R(GetSystemInfo);
    R(GetSystemTimeAsFileTime);
    R(GetTickCount);
    R(QueryPerformanceCounter);
    R(IsDebuggerPresent);
    R(IsProcessorFeaturePresent);
    R(EncodePointer);
    R(DecodePointer);
    R(VerSetConditionMask);
    R(VerifyVersionInfoA);
    R(GetCommandLineA);
    R(GetCommandLineW);
    R(GetStartupInfoW);
    R(GetEnvironmentStringsW);
    R(FreeEnvironmentStringsW);
    R(RtlCaptureContext);
    R(RtlLookupFunctionEntry);
    R(RtlVirtualUnwind);
    R(RtlUnwindEx);
    R(RtlPcToFileHeader);
    R(SetUnhandledExceptionFilter);
    R(UnhandledExceptionFilter);
    R(RaiseException);
    R(TerminateProcess);
    R(ExitProcess);
#undef R
}

// ================= PE LOADER =================
static int load_pe(void) {
    int fd = open(g_dllpath, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    g_filelen = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    g_file = malloc((size_t)g_filelen);
    if (!g_file) return -1;
    if (read(fd, g_file, (size_t)g_filelen) != (ssize_t)g_filelen) {
        perror("read");
        return -1;
    }
    close(fd);

    u32 e = f32(0x3c);
    g_imagebase = f64(e + 24 + 24);
    u32 sizeofimage = f32(e + 24 + 56);
    u32 entry = f32(e + 24 + 16);
    u16 nsec = f16(e + 6);
    u16 optsize = f16(e + 20);
    u64 sectbl = e + 24 + optsize;

    u32 hdrsize = f32(e + 24 + 60); /* SizeOfHeaders */

    /* 完全解決済みイメージをスクラッチバッファ (書込可・実行不可) に作り、
     * 無名でない一時ファイルに書いてセクションごとに mmap し直す。
     * コードは R+X、データは R+W で、W^X を同時に満たすページは存在しない。
     * DLL は推奨ベース 0x180000000 にロードするためリロケーション不要。 */
    u8 *img = calloc(1, sizeofimage);
    if (!img) {
        perror("calloc");
        return -1;
    }
    memcpy(img, g_file, hdrsize);
    for (int i = 0; i < nsec; i++) {
        u64 s = sectbl + (u64)i * 40;
        u32 vaddr = f32(s + 12), rawsize = f32(s + 16), rawptr = f32(s + 20);
        memcpy(img + vaddr, g_file + rawptr, rawsize);
    }

    /* kernel32 import を IAT (スクラッチバッファ内) に解決 */
    u32 imprva = f32(e + 24 + 112 + 8 * 1);
    if (imprva) {
        for (u64 d = imprva;; d += 20) {
            u32 orig = *(u32 *)(img + d), namer = *(u32 *)(img + d + 12),
                 fthunk = *(u32 *)(img + d + 16);
            if (namer == 0) break;
            u64 rt = orig ? orig : fthunk;
            for (int j = 0;; j++) {
                u64 ent = *(u64 *)(img + rt + (u64)j * 8);
                if (!ent) break;
                u64 *slot = (u64 *)(img + fthunk + (u64)j * 8);
                if (ent >> 63) {
                    *slot = 0;
                    continue;
                }
                const char *fn = (char *)(img + (ent & 0x7fffffff) + 2);
                void *sh = find_shim(fn);
                if (!sh) fprintf(stderr, "[loader] MISSING shim: %s\n", fn);
                *slot = (u64)(uintptr_t)sh;
            }
        }
    }

    /* 解決済みイメージを一時ファイルへ (macOS: memfd の代わり) */
    char tmpname[] = "/tmp/ftengineXXXXXX";
    int mfd = mkstemp(tmpname);
    if (mfd < 0) {
        perror("mkstemp");
        free(img);
        return -1;
    }
    if (write(mfd, img, sizeofimage) != (ssize_t)sizeofimage) {
        perror("write tmpfile");
        close(mfd);
        free(img);
        return -1;
    }
    free(img);

    /* 推奨ベースを予約 (MAP_FIXED_NOREPLACE の代わりに MAP_FIXED) */
    void *m = mmap((void *)(uintptr_t)g_imagebase, sizeofimage, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED || m != (void *)(uintptr_t)g_imagebase) {
        fprintf(stderr, "[loader] reserve @%#" PRIx64 " failed (%p)\n", g_imagebase, m);
        close(mfd);
        return -1;
    }
    g_image = m;
    u32 hdrmap = (hdrsize + 0xfff) & ~0xfffu;
    if (mmap(g_image, hdrmap, PROT_READ, MAP_PRIVATE | MAP_FIXED, mfd, 0) == MAP_FAILED) {
        perror("[loader] map headers");
        close(mfd);
        return -1;
    }
    for (int i = 0; i < nsec; i++) {
        u64 s = sectbl + (u64)i * 40;
        u32 vaddr = f32(s + 12), vsize = f32(s + 8), rawsize = f32(s + 16), chars = f32(s + 36);
        u32 seglen = (vsize > rawsize ? vsize : rawsize);
        seglen = (seglen + 0xfff) & ~0xfffu;
        if (vaddr + seglen > sizeofimage) seglen = sizeofimage - vaddr;
        if (!seglen) continue;
        int prot = PROT_READ;
        if (chars & 0x20000000) prot |= PROT_EXEC;  /* IMAGE_SCN_MEM_EXECUTE */
        if (chars & 0x80000000) prot |= PROT_WRITE; /* IMAGE_SCN_MEM_WRITE */
        if (mmap(g_image + vaddr, seglen, prot, MAP_PRIVATE | MAP_FIXED, mfd, vaddr) ==
            MAP_FAILED) {
            perror("[loader] map section");
            close(mfd);
            return -1;
        }
    }
    close(mfd);
    unlink(tmpname); /* mapping は有効のまま */
    fprintf(stderr, "[loader] file-backed image at %#" PRIx64 " (no W+X), entry RVA %#x\n",
            g_imagebase, entry);

    /* TLS (dir 9): テンプレート複製、teb->tls 配列を接続 */
    u32 tlsrva = f32(e + 24 + 112 + 8 * 9);
    if (tlsrva) {
        u64 start = *(u64 *)(g_image + tlsrva);
        u64 end = *(u64 *)(g_image + tlsrva + 8);
        u64 idxaddr = *(u64 *)(g_image + tlsrva + 16);
        u64 cbaddr = *(u64 *)(g_image + tlsrva + 24);
        u64 tlen = end - start;
        memset(g_tls_block, 0, sizeof g_tls_block);
        if (tlen) memcpy(g_tls_block, (void *)(uintptr_t)start, tlen);
        g_tls_array[0] = g_tls_block;
        if (idxaddr) *(u32 *)(uintptr_t)idxaddr = 0;
        fprintf(stderr, "[loader] TLS template %llu bytes, callbacks@%#" PRIx64 "\n",
                (unsigned long long)tlen, cbaddr);
        if (cbaddr) {
            for (u64 *cb = (u64 *)(uintptr_t)cbaddr; *cb; cb++) {
                void (MS * f)(void *, u32, void *) = (void *)(uintptr_t)*cb;
                f((void *)(uintptr_t)g_imagebase, DLL_PROCESS_ATTACH, 0);
            }
        }
    }

    /* 偽 TEB を %gs へ (mach trap 0x3000003)。
     * 以後、このプロセスでは %gs は復元しない (Stage 0 実測: libc は
     * ゼロ埋め偽TEBで問題なく動作する)。他スレッドには影響しない。 */
    memset(g_teb, 0, sizeof g_teb);
    memset(g_peb, 0, sizeof g_peb);
    *(u64 *)(g_teb + 0x30) = (u64)(uintptr_t)g_teb;        /* NtTib.Self */
    *(u64 *)(g_teb + 0x08) = (u64)(uintptr_t)(g_teb + 0x2000); /* StackBase (dummy) */
    *(u64 *)(g_teb + 0x10) = (u64)(uintptr_t)g_teb;        /* StackLimit */
    *(u64 *)(g_teb + 0x58) = (u64)(uintptr_t)g_tls_array;  /* ThreadLocalStoragePointer */
    *(u64 *)(g_teb + 0x60) = (u64)(uintptr_t)g_peb;       /* PEB */
    if (set_gs(g_teb) != 0) {
        fprintf(stderr, "[loader] mach trap SET_GS 失敗\n");
        return -1;
    }
    fprintf(stderr, "[loader] TEB installed in %%gs\n");

    /* entry point (DllMain via CRT startup) — TEB コンテキストで実行 */
    int (MS * DllMain)(void *, u32, void *) = (void *)(g_image + entry);
    fprintf(stderr, "[loader] calling entry (DllMain) ...\n");
    int r = DllMain((void *)(uintptr_t)g_imagebase, DLL_PROCESS_ATTACH, 0);
    fprintf(stderr, "[loader] DllMain returned %d\n", r);
    gs_to_libc(); /* エンジン呼び出しの外では常に libc の %gs に戻す */
    return r ? 0 : -2;
}

/* find an export by name */
static void *get_export(const char *want) {
    u32 e = f32(0x3c);
    u32 exprva = f32(e + 24 + 112 + 8 * 0);
    u32 nnames = *(u32 *)(g_image + exprva + 24);
    u32 fns = *(u32 *)(g_image + exprva + 28);
    u32 names = *(u32 *)(g_image + exprva + 32);
    u32 ords = *(u32 *)(g_image + exprva + 36);
    for (u32 i = 0; i < nnames; i++) {
        u32 nrva = *(u32 *)(g_image + names + i * 4);
        if (!strcmp((char *)(g_image + nrva), want)) {
            u16 ord = *(u16 *)(g_image + ords + i * 2);
            u32 frva = *(u32 *)(g_image + fns + ord * 4);
            return g_image + frva;
        }
    }
    return 0;
}

// ================= engine driving (mirrors the Linux harness) =================
static u8 pipeline[1024];
static u8 birbuf[262144];

static u8 small[262144];
/* Prepare an ENG_W x ENG_H frame in `small`. If the source is at least as large,
 * crop the CENTERED window (preserves true ridge geometry); otherwise scale. */
#define FRAME_W 64
#define FRAME_H 80
static void resize(u8 *src, int sw, int sh, int dw, int dh) {
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            small[y * dw + x] = src[(y * sh / dh) * sw + (x * sw / dw)];
}
static void prepare_frame(const u8 *src, int sw, int sh) {
    if (sw >= FRAME_W && sh >= FRAME_H) {
        int ox = (sw - FRAME_W) / 2, oy = (sh - FRAME_H) / 2;
        for (int y = 0; y < FRAME_H; y++)
            for (int x = 0; x < FRAME_W; x++)
                small[y * FRAME_W + x] = src[(oy + y) * sw + (ox + x)];
    } else {
        resize((u8 *)src, sw, sh, FRAME_W, FRAME_H);
    }
}
static int build_bir(u8 *pix, int w, int h) {
    int total = 0x48 + 0x38 + w * h;
    memset(birbuf, 0, (size_t)total);
    const int HDR = 0x18, STD = 0x48;
    *(u32 *)(birbuf + 0) = 0x30;
    *(u32 *)(birbuf + 4) = HDR;
    *(u32 *)(birbuf + 8) = 0x38 + w * h;
    *(u32 *)(birbuf + 0xc) = STD;
    *(u16 *)(birbuf + HDR + 0x28) = 0x001B;
    *(u16 *)(birbuf + HDR + 0x2a) = 0x0401;
    *(u16 *)(birbuf + STD + 0x1c) = 500;
    *(u16 *)(birbuf + STD + 0x1e) = 500;
    birbuf[STD + 0x22] = 8;
    birbuf[STD + 0x23] = 0;
    *(u16 *)(birbuf + STD + 0x2c) = (u16)w;
    *(u16 *)(birbuf + STD + 0x2e) = (u16)h;
    memcpy(birbuf + STD + 0x38, pix, (size_t)(w * h));
    return total;
}

// ================= STORAGE ADAPTER STUB (pipeline+0x28 vtable) =================
/* Offsets the engine calls (Linux版と同様、逆アセンブルで確認されたもの):
 *   +0x68 AddRecord  +0x78 QueryBySubject  +0x90 FirstRecord
 *   +0x98 NextRecord +0xa0 GetCurrentRecord
 * WINBIO_STORAGE_RECORD: +0x00 Identity*, +0x20 TemplateBlob, +0x28 size */
static u8 g_rec_identity[76];
static u8 g_rec_template[1 << 20]; /* enrollment template ~200KB (up to 18 sub-tpls) */
static u64 g_rec_tsize = 0;
static int g_have_record = 0;

static u64 MS st_default(void *a, void *b, void *c, void *d, void *e, void *f) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    return 0;
}

static u32 MS st_QueryBySubject(void *pipe, void *identity, u8 sub) {
    (void)pipe;
    gs_to_libc();
    fprintf(stderr, "[storage] QueryBySubject(sub=%u) -> %s\n", sub,
            g_have_record ? "S_OK" : "NO_RESULTS");
    if (identity) memcpy(g_rec_identity, identity, sizeof g_rec_identity);
    gs_to_teb();
    return g_have_record ? 0 : 0x8009801f; /* WINBIO_E_DATABASE_NO_RESULTS */
}
static u32 MS st_FirstRecord(void *pipe) {
    (void)pipe;
    gs_to_libc();
    fprintf(stderr, "[storage] FirstRecord -> %s\n", g_have_record ? "S_OK" : "NO_RESULTS");
    gs_to_teb();
    return g_have_record ? 0 : 0x8009801f;
}
static u32 MS st_NextRecord(void *pipe) {
    (void)pipe;
    gs_to_libc();
    fprintf(stderr, "[storage] NextRecord -> NO_MORE_RECORDS\n");
    gs_to_teb();
    return 0x80098020; /* single-record store: always end after the first */
}
static u32 MS st_GetCurrentRecord(void *pipe, u8 *rec) {
    (void)pipe;
    gs_to_libc();
    fprintf(stderr, "[storage] GetCurrentRecord (tsize=%llu)\n", (unsigned long long)g_rec_tsize);
    gs_to_teb();
    memset(rec, 0, 0x40);
    *(u64 *)(rec + 0x00) = (u64)(uintptr_t)g_rec_identity;
    *(u64 *)(rec + 0x20) = (u64)(uintptr_t)g_rec_template;
    *(u64 *)(rec + 0x28) = g_rec_tsize;
    return 0;
}
static u32 MS st_AddRecord(void *pipe, u8 *rec, u8 sub) {
    (void)pipe;
    gs_to_libc();
    u64 tp = *(u64 *)(rec + 0x20), ts = *(u64 *)(rec + 0x28);
    u64 idp = *(u64 *)(rec + 0x00);
    fprintf(stderr, "[storage] AddRecord(sub=%u) template=%#" PRIx64 " size=%llu\n", sub, tp,
            (unsigned long long)ts);
    if (idp) memcpy(g_rec_identity, (void *)(uintptr_t)idp, sizeof g_rec_identity);
    if (tp && ts && ts <= sizeof g_rec_template) {
        memcpy(g_rec_template, (void *)(uintptr_t)tp, (size_t)ts);
        g_rec_tsize = ts;
        g_have_record = 1;
    }
    gs_to_teb();
    return 0;
}

static u64 g_storage_vt[0x100 / 8];
static u64 g_sensor_vt[0x100 / 8];
static void build_storage_vtable(void) {
    for (int i = 0; i < 0x100 / 8; i++) {
        g_storage_vt[i] = (u64)(uintptr_t)st_default;
        g_sensor_vt[i] = (u64)(uintptr_t)st_default;
    }
    g_storage_vt[0x68 / 8] = (u64)(uintptr_t)st_AddRecord;
    g_storage_vt[0x78 / 8] = (u64)(uintptr_t)st_QueryBySubject;
    g_storage_vt[0x90 / 8] = (u64)(uintptr_t)st_FirstRecord;
    g_storage_vt[0x98 / 8] = (u64)(uintptr_t)st_NextRecord;
    g_storage_vt[0xa0 / 8] = (u64)(uintptr_t)st_GetCurrentRecord;
}

/* ==================== public API (ft_engine.h) ==================== */
#include "ft_engine.h"

#define ENG_W 64
#define ENG_H 80
static void *g_iface;

static int g_loaded = 0;

void ft_engine_geometry(int *w, int *h) {
    if (w) *w = ENG_W;
    if (h) *h = ENG_H;
}

int ft_engine_open(const char *dll_path) {
    if (g_loaded) return 0;
    /* libc の TSD 基底を先に特定 (native %gs のうちに) —
     * shims 内で libc を呼ぶ際の gs切替え (規律) に必要 */
    if (discover_libc_base() != 0) {
        fprintf(stderr, "[loader] !! libc TSD 基底を特定できなかった\n");
        return -5;
    }
    fprintf(stderr, "[loader] libc TSD base = %#llx\n", (unsigned long long)g_libc_base);

    g_dllpath = dll_path ? dll_path : "ftWbioEngineAdapter.dll";
    register_shims();
    if (load_pe() != 0) return -1;
    void *q = get_export("WbioQueryEngineInterface");
    if (!q) return -2;
    u64 (MS * Query)(void **) = (void *)q;
    void *iface = 0;
    gs_to_teb();
    u32 qr = (u32)(uintptr_t)Query(&iface);
    gs_to_libc();
    if (qr != 0 || !iface) return -3;
    g_iface = iface;
    *(u64 *)(pipeline + 0x08) = ~0ULL;
    build_storage_vtable();
    *(u64 *)(pipeline + 0x20) = (u64)(uintptr_t)g_sensor_vt;
    *(u64 *)(pipeline + 0x28) = (u64)(uintptr_t)g_storage_vt;
    u64 (MS * Attach)(void *) = *(void **)((u8 *)iface + 32 + 0 * 8);
    gs_to_teb();
    u32 ar = (u32)(uintptr_t)Attach(pipeline);
    gs_to_libc();
    if (ar != 0) return -4;
    u64 ctx = *(u64 *)(pipeline + 0x38);
    *(u32 *)(uintptr_t)(ctx + 0x24) = ENG_W;
    *(u32 *)(uintptr_t)(ctx + 0x28) = ENG_H;
    g_loaded = 1;
    return 0;
}

void ft_engine_close(void) {
    /* Linux版の意図を踏襲: 固定ベースに1回ロードしたら破棄しない (no-op) */
}

uint32_t ft_engine_accept(const uint8_t *img, int sw, int sh, uint8_t purpose) {
    prepare_frame((const u8 *)img, sw, sh);
    int total = build_bir(small, ENG_W, ENG_H);
    u32 rej = 0;
    u64 (MS * Accept)(void *, void *, u64, u64, void *) = *(void **)((u8 *)g_iface + 32 + 8 * 8);
    gs_to_teb(); /* エンジン呼び出しは TEB コンテキストで */
    u32 hr = (u32)(uintptr_t)Accept(pipeline, birbuf, (u64)total, (u64)purpose, &rej);
    gs_to_libc();
    return hr;
}

void ft_engine_enroll_begin(void) {
    g_have_record = 0;
    g_rec_tsize = 0;
    u64 (MS * Create)(void *) = *(void **)((u8 *)g_iface + 32 + 12 * 8);
    gs_to_teb();
    Create(pipeline);
    gs_to_libc();
}

int ft_engine_enroll_update(void) {
    u64 (MS * Update)(void *, void *) = *(void **)((u8 *)g_iface + 32 + 13 * 8);
    u64 (MS * Status)(void *, void *) = *(void **)((u8 *)g_iface + 32 + 14 * 8);
    u32 urej = 0;
    gs_to_teb();
    u64 uhr = Update(pipeline, &urej);
    if ((u32)(uintptr_t)uhr == 0x80098008 || urej) {
        gs_to_libc();
        return 2; /* frame rejected */
    }
    u32 srej = 0;
    u64 shr = Status(pipeline, &srej);
    gs_to_libc();
    return (u32)(uintptr_t)shr == 0 ? 0 : 1; /* 0 complete, 1 need more */
}

static void fill_identity(u8 *id) {
    memset(id, 0, 76);
    *(u32 *)id = 3;
    for (int i = 0; i < 16; i++) id[8 + i] = 0xA0 + i;
}

int ft_engine_enroll_commit(uint8_t **out, size_t *outlen) {
    u8 id[76];
    fill_identity(id);
    g_have_record = 0;
    u64 (MS * Commit)(void *, void *, u8) = *(void **)((u8 *)g_iface + 32 + 17 * 8);
    gs_to_teb();
    u32 cr = (u32)(uintptr_t)Commit(pipeline, id, 1);
    gs_to_libc();
    if (cr != 0 || !g_have_record) return -1;
    *out = malloc((size_t)g_rec_tsize);
    if (!*out) return -2;
    memcpy(*out, g_rec_template, (size_t)g_rec_tsize);
    *outlen = (size_t)g_rec_tsize;
    return 0;
}

int ft_engine_verify(const uint8_t *tmpl, size_t tmpllen) {
    if (tmpllen == 0 || tmpllen > sizeof g_rec_template) return 0;
    memcpy(g_rec_template, tmpl, tmpllen);
    g_rec_tsize = tmpllen;
    g_have_record = 1;
    u8 id[76];
    fill_identity(id);
    u8 m = 0;
    u32 ps = 0, hs = 0, vr = 0;
    void *plp = 0, *hp = 0;
    u64 (MS * Verify)(void *, void *, u8, void *, void *, void *, void *, void *, void *) =
        *(void **)((u8 *)g_iface + 32 + 10 * 8);
    gs_to_teb();
    Verify(pipeline, id, 1, &m, &ps, &plp, &hs, &hp, &vr);
    gs_to_libc();
    return m ? 1 : 0;
}
