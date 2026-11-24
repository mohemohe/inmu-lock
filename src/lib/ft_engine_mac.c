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
//     4. macOS の libSystem は %gs を TLS (malloc/stdio 等) に使う。
//        → DLL のコードを実行する間だけ %gs を偽 TEB にし、shim が libc を
//          呼ぶ間と DLL 呼び出しの外では libc の基底へ戻す (下記「gs 切替え規律」)。
//          基底はスレッドごとなので、ft_engine_* は ft_engine_open を呼んだ
//          スレッドからのみ呼ぶこと。
//
//   その他の移植: memfd_create → mkstemp + mmap + unlink (マップ完了直後),
//                MAP_FIXED_NOREPLACE → MAP_FIXED (推奨ベース 0x180000000 は
//                競合しない前提。元実装と同じくリロケーションは処理しない)
//
// ライセンス: LGPL-2.1-or-later (ft9201-libfprint 由来の部分)

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ft_engine.h" /* FT_ENGINE_TEMPLATE_MAX (公開 API の宣言は下でも参照) */

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
static u32 g_sizeofimage; /* 検証済み SizeOfImage (load_pe が設定) */
/* f16/f32/f64 は境界検査をしない。呼ぶ前に必ず file_range_ok() で確認すること。 */
static u32 f32(u64 o) { return *(u32 *)(g_file + o); }
static u64 f64(u64 o) { return *(u64 *)(g_file + o); }
static u16 f16(u64 o) { return *(u16 *)(g_file + o); }
/* [off, off+len) がファイル内に収まるか (オーバーフローしない比較) */
static int file_range_ok(u64 off, u64 len) {
    return g_filelen > 0 && off <= (u64)g_filelen && len <= (u64)g_filelen - off;
}
/* [rva, rva+len) がイメージ内に収まるか */
static int img_range_ok(u64 rva, u64 len) {
    return rva <= g_sizeofimage && len <= (u64)g_sizeofimage - rva;
}
/* 実際に mmap した領域 (ヘッダ + 各セクション) と保護属性。
 * イメージ範囲内でも、セクションの隙間 (PROT_NONE) や書込不可ページがあるので、
 * マップ後のイメージを読む/書く/実行する前に seg_ok() で確認する。 */
#define FT_MAX_SECTIONS 32
static struct {
    u32 rva, len;
    int prot;
} g_segs[FT_MAX_SECTIONS + 1];
static int g_nsegs;
/* [rva, rva+len) が 1 つのマップ済み領域に収まり、need の保護属性を全て持つか */
static int seg_ok(u64 rva, u64 len, int need) {
    for (int i = 0; i < g_nsegs; i++) {
        if (rva >= g_segs[i].rva && rva - g_segs[i].rva <= g_segs[i].len &&
            len <= g_segs[i].len - (rva - g_segs[i].rva) && (g_segs[i].prot & need) == need)
            return 1;
    }
    return 0;
}
/* 仮想アドレス範囲版 */
static int va_seg_ok(u64 va, u64 len, int need) {
    return va >= g_imagebase && seg_ok(va - g_imagebase, len, need);
}

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

/* DLL の致命エラー (ExitProcess / TerminateProcess / RaiseException) 用。
 * %gs を libc 側へ戻してからログを出し、libc の teardown を経由せず終了する
 * (デーモンは launchd の KeepAlive で再起動される)。
 * シムから戻ると、DLL が「到達しない」前提のコード (throw の後、/GS 検査失敗の
 * 後など) を root のまま実行し続けてしまうため、決して戻らない。 */
#define FT_FATAL_EXIT_CODE 70 /* EX_SOFTWARE 相当 */
static void shim_fatal(const char *what, u32 code) __attribute__((noreturn));
static void shim_fatal(const char *what, u32 code) {
    gs_to_libc();
    fprintf(stderr, "[!] %s(%#x) — vendor DLL の致命エラー。プロセスを終了します\n", what, code);
    fflush(stderr);
    _exit(FT_FATAL_EXIT_CODE);
}

/* ヒープ: 各ブロックの先頭に 16 バイトのヘッダ (要求サイズ) を置く。
 * HeapSize が要求サイズを正確に返せるようにし、HeapReAlloc の
 * HEAP_ZERO_MEMORY (増加分のゼロ埋め) を Windows と同じ意味で実装するため。
 * ヘッダは 16 バイトなので、返すポインタの 16 バイト境界は保たれる。 */
#define HEAP_HDR 16
#define HEAP_ZERO_MEMORY 0x8
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10
static void *MS sh_GetProcessHeap(void) { return (void *)0x100; }
static void *MS sh_HeapAlloc(void *h, u32 flags, u64 size) {
    (void)h;
    if (size > (u64)SIZE_MAX - HEAP_HDR) return 0;
    gs_to_libc(); /* malloc は TLS (%gs) を使うため必須 */
    u64 *hdr = malloc((size_t)size + HEAP_HDR);
    if (hdr) {
        hdr[0] = size;
        hdr[1] = 0;
        if (flags & HEAP_ZERO_MEMORY) memset(hdr + 2, 0, (size_t)size);
    }
    gs_to_teb();
    return hdr ? (void *)(hdr + 2) : 0;
}
static int MS sh_HeapFree(void *h, u32 f, void *p) {
    (void)h;
    (void)f;
    if (!p) return 1;
    gs_to_libc();
    free((u64 *)p - 2);
    gs_to_teb();
    return 1;
}
static void *MS sh_HeapReAlloc(void *h, u32 f, void *p, u64 s) {
    if (!p) return sh_HeapAlloc(h, f, s);
    if (s > (u64)SIZE_MAX - HEAP_HDR) return 0;
    u64 *hdr = (u64 *)p - 2;
    u64 old = hdr[0];
    u64 *nh;
    gs_to_libc();
    if (f & HEAP_REALLOC_IN_PLACE_ONLY) {
        /* ブロックを移動してはならない: 実容量に収まる時だけ成功させる */
        size_t cap = malloc_size(hdr);
        nh = (cap >= HEAP_HDR && s <= cap - HEAP_HDR) ? hdr : 0;
    } else {
        nh = realloc(hdr, (size_t)s + HEAP_HDR);
    }
    if (nh) {
        nh[0] = s;
        if ((f & HEAP_ZERO_MEMORY) && s > old)
            memset((u8 *)(nh + 2) + old, 0, (size_t)(s - old));
    }
    gs_to_teb();
    return nh ? (void *)(nh + 2) : 0; /* 失敗時、元のブロックはそのまま有効 */
}
static u64 MS sh_HeapSize(void *h, u32 f, void *p) {
    (void)h;
    (void)f;
    if (!p) return ~0ULL; /* (SIZE_T)-1 = 失敗 */
    return ((u64 *)p - 2)[0];
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
static u32 MS sh_TlsAlloc(void) {
    if (g_tls_next >= 1088) return 0xffffffff; /* TLS_OUT_OF_INDEXES */
    return g_tls_next++;
}
static int MS sh_TlsFree(u32 i) {
    (void)i;
    return 1;
}
static void *MS sh_TlsGetValue(u32 i) {
    g_lasterr = 0;
    return i < 1088 ? g_tls_vals[i] : 0;
}
static int MS sh_TlsSetValue(u32 i, void *v) {
    if (i >= 1088) {
        g_lasterr = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    g_tls_vals[i] = v;
    return 1;
}

// process/thread/module
static void *MS sh_GetCurrentProcess(void) { return (void *)-1; }
static u32 MS sh_GetCurrentProcessId(void) {
    gs_to_libc();
    u32 pid = (u32)getpid();
    gs_to_teb();
    return pid;
}
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
/* Windows と同じ意味: 収まれば長さ (NUL 除く) を返す。収まらなければ
 * n-1 文字 + NUL に切り詰めて n を返し ERROR_INSUFFICIENT_BUFFER を設定する。
 * いずれの場合も必ず NUL 終端する。 */
static u32 MS sh_GetModuleFileNameA(void *m, char *b, u32 n) {
    (void)m;
    if (!b || n == 0) {
        g_lasterr = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    size_t len = strlen(g_dllpath);
    if (len < n) {
        memcpy(b, g_dllpath, len + 1);
        return (u32)len;
    }
    memcpy(b, g_dllpath, n - 1);
    b[n - 1] = 0;
    g_lasterr = 122;
    return n;
}
static u32 MS sh_GetModuleFileNameW(void *m, u16 *b, u32 n) {
    (void)m;
    if (!b || n == 0) {
        g_lasterr = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    u32 i = 0;
    for (; g_dllpath[i] && i < n - 1; i++) b[i] = (u16)(u8)g_dllpath[i];
    b[i] = 0;
    if (g_dllpath[i]) {
        g_lasterr = 122;
        return n;
    }
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
    static const char sysdir[] = "C:\\Windows\\System32";
    /* 収まらない時は必要サイズ (NUL 込み) を返し、バッファには書かない */
    if (!b || n < sizeof sysdir) return (u32)sizeof sysdir;
    memcpy(b, sysdir, sizeof sysdir);
    return (u32)(sizeof sysdir - 1);
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
        /* CPINFO: MaxCharSize(4) + DefaultChar[2] + LeadByte[12]。
         * LeadByte を未初期化のまま残さない (全て 0 = リードバイトなし)。 */
        u8 *p = info;
        memset(p, 0, 18);
        *(u32 *)p = 1;
    }
    return 1;
}
static int MS sh_IsValidCodePage(u32 cp) {
    (void)cp;
    return 1;
}
/* NUL 終端の UTF-16 文字列の長さ (NUL 込み)。int に収まらなければ -1。 */
static int wlen_nul(const u16 *s) {
    size_t n = 0;
    while (s[n]) {
        if (++n >= (size_t)INT_MAX) return -1;
    }
    return (int)n + 1;
}
/* 以下の変換系は Windows と同じ意味にする: 出力長 0 は必要長の問い合わせ、
 * 出力バッファが足りなければ収まる分だけ書いて 0 を返し
 * ERROR_INSUFFICIENT_BUFFER を設定する (切り詰めた結果を成功として返さない)。 */
static int MS sh_MultiByteToWideChar(u32 cp, u32 f, const char *mb, int mbc, u16 *wc, int wcc) {
    (void)cp;
    (void)f;
    if (!mb || mbc == 0 || wcc < 0 || (wcc > 0 && !wc)) {
        g_lasterr = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (mbc < 0) {
        size_t l = strlen(mb) + 1;
        if (l > (size_t)INT_MAX) {
            g_lasterr = 87;
            return 0;
        }
        mbc = (int)l;
    }
    if (wcc == 0) return mbc;
    int n = mbc < wcc ? mbc : wcc;
    for (int i = 0; i < n; i++) wc[i] = (u8)mb[i];
    if (mbc > wcc) {
        g_lasterr = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    return n;
}
static int MS sh_WideCharToMultiByte(u32 cp, u32 f, const u16 *wc, int wcc, char *mb, int mbc, void *d, void *u) {
    (void)cp;
    (void)f;
    (void)d;
    (void)u;
    if (!wc || wcc == 0 || mbc < 0 || (mbc > 0 && !mb)) {
        g_lasterr = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (wcc < 0) {
        wcc = wlen_nul(wc);
        if (wcc < 0) {
            g_lasterr = 87;
            return 0;
        }
    }
    if (mbc == 0) return wcc;
    int n = wcc < mbc ? wcc : mbc;
    for (int i = 0; i < n; i++) mb[i] = (char)wc[i];
    if (wcc > mbc) {
        g_lasterr = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    return n;
}
static int MS sh_LCMapStringW(u32 l, u32 f, const u16 *s, int sc, u16 *d, int dc) {
    (void)l;
    (void)f;
    if (!s || sc == 0 || dc < 0 || (dc > 0 && !d)) {
        g_lasterr = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (sc < 0) {
        sc = wlen_nul(s);
        if (sc < 0) {
            g_lasterr = 87;
            return 0;
        }
    }
    if (dc == 0) return sc;
    int n = sc < dc ? sc : dc;
    for (int i = 0; i < n; i++) d[i] = s[i];
    if (sc > dc) {
        g_lasterr = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    return n;
}
static int MS sh_GetStringTypeW(u32 t, const u16 *s, int c, u16 *out) {
    (void)t;
    if (!s || !out || c == 0) {
        g_lasterr = 87; /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (c < 0) {
        c = wlen_nul(s);
        if (c < 0) {
            g_lasterr = 87;
            return 0;
        }
    }
    for (int i = 0; i < c; i++) out[i] = 0;
    return 1;
}

// misc / version / cpu
static void MS sh_GetSystemInfo(void *p) {
    /* SYSTEM_INFO は x64 で 48 バイト (DLL 側も 0x30 バイトのスタック領域を
     * 渡している)。それを超えて書くと呼び出し元のスタックを壊す。 */
    memset(p, 0, 48);
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
/* RaiseException: このローダーには SEH ディスパッチャが無い
 * (RtlLookupFunctionEntry は NULL、RtlUnwindEx は no-op) ため、例外を捕捉する
 * ハンドラへ制御を渡せない。Windows でいう「未処理例外」と同じ扱いで
 * プロセスを終了する。戻ると C++ の throw の直後など、DLL が到達しない前提の
 * コードを実行し続けてしまう。 */
static void MS sh_RaiseException(u32 code, u32 fl, u32 n, void *a) {
    (void)fl;
    (void)n;
    (void)a;
    shim_fatal("RaiseException", code);
}
static void MS sh_TerminateProcess(void *h, u32 c) {
    (void)h;
    shim_fatal("TerminateProcess", c);
}
static void MS sh_ExitProcess(u32 c) {
    shim_fatal("ExitProcess", c);
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
/* ロードを許可する唯一の DLL (vendor/ftWbioEngineAdapter.dll) の SHA-256。
 * この DLL は root のデーモン内でそのまま実行されるため、内容が 1 ビットでも
 * 違うものは決してロードしない。DLL を更新する場合はここも更新すること。 */
static const u8 g_dll_sha256[CC_SHA256_DIGEST_LENGTH] = {
    0xca, 0x34, 0xba, 0x1e, 0x84, 0x94, 0xfc, 0x84, 0xa9, 0xac, 0x31, 0xdd, 0x5c, 0x00, 0x3e, 0x82,
    0x5d, 0x84, 0x74, 0x84, 0xe5, 0xe3, 0x50, 0x9f, 0x09, 0x9d, 0xe1, 0x23, 0xd7, 0x95, 0x3c, 0x6a,
};
#define FT_DLL_MAX_BYTES (16u << 20)   /* DLL ファイルサイズの上限 */
#define FT_IMAGE_MAX_BYTES (64u << 20) /* SizeOfImage の上限 */
/* リロケーションを処理しないので、推奨ベースはこの値でなければならない */
#define FT_IMAGE_BASE 0x180000000ULL

/* DLL ファイル全体を読み込み、SHA-256 がピンと一致した時だけ g_file/g_filelen に
 * 設定する。以後のロードはこのメモリ上のバイト列だけを使い、ファイルは再度
 * 読まない (検証と使用の間で差し替えられない)。 */
static int read_verified_dll(void) {
    /* O_NOFOLLOW: 最終要素のシンボリックリンクを辿らない。
     * O_NONBLOCK: FIFO 等を指定されても open でブロックしない。 */
    int fd = open(g_dllpath, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror("[loader] open");
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("[loader] fstat");
        close(fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "[loader] !! 通常ファイルではない: %s\n", g_dllpath);
        close(fd);
        return -1;
    }
    if (geteuid() == 0 && (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)))) {
        fprintf(stderr, "[loader] !! root 実行時は root 所有かつ group/other 書き込み不可の "
                        "DLL のみロードする: %s (uid=%u mode=%o)\n",
                g_dllpath, (unsigned)st.st_uid, (unsigned)(st.st_mode & 07777));
        close(fd);
        return -1;
    }
    if (st.st_size <= 0 || st.st_size > (off_t)FT_DLL_MAX_BYTES) {
        fprintf(stderr, "[loader] !! DLL のサイズが不正: %lld bytes\n", (long long)st.st_size);
        close(fd);
        return -1;
    }
    size_t len = (size_t)st.st_size;
    u8 *buf = malloc(len);
    if (!buf) {
        perror("[loader] malloc");
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[loader] read");
            break;
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != len) {
        fprintf(stderr, "[loader] !! DLL を全量読めなかった (%zu/%zu bytes)\n", got, len);
        free(buf);
        return -1;
    }

    u8 md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(buf, (CC_LONG)len, md);
    if (memcmp(md, g_dll_sha256, sizeof md) != 0) {
        char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
        for (size_t i = 0; i < sizeof md; i++) snprintf(hex + i * 2, 3, "%02x", md[i]);
        fprintf(stderr, "[loader] !! DLL の SHA-256 が一致しない — ロードを拒否: %s\n"
                        "[loader]    actual = %s\n",
                g_dllpath, hex);
        free(buf);
        return -1;
    }
    g_file = buf;
    g_filelen = (long)len;
    return 0;
}

static int load_pe(void) {
    u8 *img = NULL;  /* 解決済みイメージのスクラッチバッファ */
    int mfd = -1;    /* 一時ファイルの fd */
    int tmp_made = 0; /* 一時ファイルが /tmp に残っているか (未 unlink) */
    int mapped = 0;  /* 推奨ベースを予約済みか */
    char tmpname[] = "/tmp/ftengineXXXXXX";

    /* ハッシュ検証を通るまで、ファイルの中身は一切パースしない */
    if (read_verified_dll() != 0) return -1;

    /* ---- ヘッダの検証 (多層防御: すべてファイル長/イメージ長に対して境界検査) ---- */
    if (!file_range_ok(0, 0x40) || f16(0) != 0x5a4d /* MZ */) goto bad_pe;
    u32 e = f32(0x3c);
    /* PE シグネチャ(4) + FILE ヘッダ(20) + PE32+ オプショナルヘッダ(240) */
    if (!file_range_ok(e, 24 + 240)) goto bad_pe;
    u16 nsec = f16((u64)e + 6);
    u16 optsize = f16((u64)e + 20);
    if (f32(e) != 0x00004550 /* PE\0\0 */ || f16((u64)e + 4) != 0x8664 /* AMD64 */ ||
        f16((u64)e + 24) != 0x20b /* PE32+ */ || optsize < 240 ||
        f32((u64)e + 24 + 108) < 10 /* NumberOfRvaAndSizes: dir 0,1,9 を使う */)
        goto bad_pe;
    g_imagebase = f64((u64)e + 24 + 24);
    u32 sizeofimage = f32((u64)e + 24 + 56);
    u32 entry = f32((u64)e + 24 + 16);
    u64 sectbl = (u64)e + 24 + optsize;
    u32 hdrsize = f32((u64)e + 24 + 60); /* SizeOfHeaders */
    if (g_imagebase != FT_IMAGE_BASE || sizeofimage == 0 || sizeofimage > FT_IMAGE_MAX_BYTES ||
        (sizeofimage & 0xfff) || hdrsize > sizeofimage || !file_range_ok(0, hdrsize) ||
        entry == 0 || entry >= sizeofimage || nsec > FT_MAX_SECTIONS ||
        !file_range_ok(sectbl, (u64)nsec * 40))
        goto bad_pe;
    g_sizeofimage = sizeofimage;
    g_nsegs = 0;
    u64 prev_end = ((u64)hdrsize + 0xfff) & ~0xfffULL; /* ヘッダのマップ範囲の終端 */
    for (int i = 0; i < nsec; i++) {
        u64 s = sectbl + (u64)i * 40;
        u32 vsize = f32(s + 8), vaddr = f32(s + 12), rawsize = f32(s + 16), rawptr = f32(s + 20);
        if ((vaddr & 0xfff) || !img_range_ok(vaddr, vsize) || !img_range_ok(vaddr, rawsize) ||
            !file_range_ok(rawptr, rawsize))
            goto bad_pe;
        /* セクションは昇順で、ヘッダや他のセクションと重ならないこと
         * (重なると後からの MAP_FIXED が前の領域の保護属性を上書きする) */
        if (vaddr < prev_end) goto bad_pe;
        u64 span = vsize > rawsize ? vsize : rawsize;
        prev_end = (vaddr + span + 0xfff) & ~0xfffULL;
    }

    /* 完全解決済みイメージをスクラッチバッファ (書込可・実行不可) に作り、
     * 無名でない一時ファイルに書いてセクションごとに mmap し直す。
     * コードは R+X、データは R+W で、W^X を同時に満たすページは存在しない。
     * DLL は推奨ベース 0x180000000 にロードするためリロケーション不要。 */
    img = calloc(1, sizeofimage);
    if (!img) {
        perror("calloc");
        goto fail;
    }
    memcpy(img, g_file, hdrsize);
    for (int i = 0; i < nsec; i++) {
        u64 s = sectbl + (u64)i * 40;
        u32 vaddr = f32(s + 12), rawsize = f32(s + 16), rawptr = f32(s + 20);
        memcpy(img + vaddr, g_file + rawptr, rawsize);
    }

    /* kernel32 import を IAT (スクラッチバッファ内) に解決 */
    u32 imprva = f32((u64)e + 24 + 112 + 8 * 1);
    if (imprva) {
        for (u64 d = imprva;; d += 20) {
            if (!img_range_ok(d, 20)) goto bad_pe;
            u32 orig = *(u32 *)(img + d), namer = *(u32 *)(img + d + 12),
                 fthunk = *(u32 *)(img + d + 16);
            if (namer == 0) break;
            u64 rt = orig ? orig : fthunk;
            for (u64 j = 0;; j++) {
                if (!img_range_ok(rt + j * 8, 8) || !img_range_ok((u64)fthunk + j * 8, 8))
                    goto bad_pe;
                u64 ent = *(u64 *)(img + rt + j * 8);
                if (!ent) break;
                u64 *slot = (u64 *)(img + fthunk + j * 8);
                if (ent >> 63) {
                    /* 序数 import は解決できない。NULL のまま残すと呼ばれた時に
                     * 不定動作になるのでロード自体を拒否する。 */
                    fprintf(stderr, "[loader] !! 序数 import は未対応\n");
                    goto fail;
                }
                u64 nrva = (ent & 0x7fffffff) + 2;
                if (!img_range_ok(nrva, 1) || !memchr(img + nrva, 0, sizeofimage - nrva))
                    goto bad_pe; /* 名前がイメージ内で NUL 終端されていない */
                const char *fn = (char *)(img + nrva);
                void *sh = find_shim(fn);
                if (!sh) {
                    /* 未解決の import を NULL のまま残さない (fail-closed) */
                    fprintf(stderr, "[loader] MISSING shim: %s\n", fn);
                    goto fail;
                }
                *slot = (u64)(uintptr_t)sh;
            }
        }
    }

    /* 解決済みイメージを一時ファイルへ (macOS: memfd の代わり)。
     * mkstemp は O_EXCL・モード 0600 で作る。macOS は unlink 済みファイルの
     * PROT_EXEC mmap を EPERM で拒否する (実測) ため、作成直後には unlink できない。
     * mapping 完了直後に unlink し、それ以前のエラーパスでは fail: で必ず unlink する。 */
    mfd = mkstemp(tmpname);
    if (mfd < 0) {
        perror("mkstemp");
        goto fail;
    }
    tmp_made = 1;
    for (size_t done = 0; done < sizeofimage;) {
        ssize_t n = write(mfd, img + done, sizeofimage - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("write tmpfile");
            goto fail;
        }
        if (n == 0) {
            fprintf(stderr, "[loader] write tmpfile: 書き込みが進まない\n");
            goto fail;
        }
        done += (size_t)n;
    }
    free(img);
    img = NULL;

    /* 推奨ベースを予約 (MAP_FIXED_NOREPLACE の代わりに MAP_FIXED) */
    void *m = mmap((void *)(uintptr_t)g_imagebase, sizeofimage, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED || m != (void *)(uintptr_t)g_imagebase) {
        fprintf(stderr, "[loader] reserve @%#" PRIx64 " failed (%p)\n", g_imagebase, m);
        if (m != MAP_FAILED) munmap(m, sizeofimage);
        goto fail;
    }
    g_image = m;
    mapped = 1;
    u32 hdrmap = (hdrsize + 0xfff) & ~0xfffu;
    if (hdrmap) {
        if (mmap(g_image, hdrmap, PROT_READ, MAP_PRIVATE | MAP_FIXED, mfd, 0) == MAP_FAILED) {
            perror("[loader] map headers");
            goto fail;
        }
        g_segs[g_nsegs].rva = 0;
        g_segs[g_nsegs].len = hdrmap;
        g_segs[g_nsegs].prot = PROT_READ;
        g_nsegs++;
    }
    for (int i = 0; i < nsec; i++) {
        u64 s = sectbl + (u64)i * 40;
        u32 vaddr = f32(s + 12), vsize = f32(s + 8), rawsize = f32(s + 16), chars = f32(s + 36);
        u64 seglen = (vsize > rawsize ? vsize : rawsize);
        seglen = (seglen + 0xfff) & ~0xfffULL;
        if (vaddr + seglen > sizeofimage) seglen = sizeofimage - vaddr;
        if (!seglen) continue;
        int prot = PROT_READ;
        if (chars & 0x20000000) prot |= PROT_EXEC;  /* IMAGE_SCN_MEM_EXECUTE */
        if (chars & 0x80000000) prot |= PROT_WRITE; /* IMAGE_SCN_MEM_WRITE */
        if ((prot & PROT_EXEC) && (prot & PROT_WRITE)) {
            fprintf(stderr, "[loader] !! W+X セクションは拒否\n");
            goto fail;
        }
        if (mmap(g_image + vaddr, (size_t)seglen, prot, MAP_PRIVATE | MAP_FIXED, mfd, vaddr) ==
            MAP_FAILED) {
            perror("[loader] map section");
            goto fail;
        }
        g_segs[g_nsegs].rva = vaddr;
        g_segs[g_nsegs].len = (u32)seglen;
        g_segs[g_nsegs].prot = prot;
        g_nsegs++;
    }
    /* entry point は実行可能なマップ済み領域になければならない */
    if (!seg_ok(entry, 1, PROT_READ | PROT_EXEC)) goto bad_pe;
    close(mfd);
    mfd = -1;
    unlink(tmpname); /* mapping は有効のまま */
    tmp_made = 0;
    fprintf(stderr, "[loader] file-backed image at %#" PRIx64 " (no W+X), entry RVA %#x\n",
            g_imagebase, entry);

    /* 偽 TEB / PEB を用意する (%gs への設定は DLL のコードを呼ぶ間だけ)。 */
    memset(g_teb, 0, sizeof g_teb);
    memset(g_peb, 0, sizeof g_peb);
    *(u64 *)(g_teb + 0x30) = (u64)(uintptr_t)g_teb;        /* NtTib.Self */
    *(u64 *)(g_teb + 0x08) = (u64)(uintptr_t)(g_teb + 0x2000); /* StackBase (dummy) */
    *(u64 *)(g_teb + 0x10) = (u64)(uintptr_t)g_teb;        /* StackLimit */
    *(u64 *)(g_teb + 0x58) = (u64)(uintptr_t)g_tls_array;  /* ThreadLocalStoragePointer */
    *(u64 *)(g_teb + 0x60) = (u64)(uintptr_t)g_peb;       /* PEB */
    /* mach trap 0x3000003 で %gs を偽 TEB へ切替えられることを確認し、
     * すぐ libc 側へ戻す (gs 切替え規律: libc を呼ぶ間は常に libc の %gs)。 */
    if (set_gs(g_teb) != 0) {
        gs_to_libc();
        fprintf(stderr, "[loader] mach trap SET_GS 失敗\n");
        goto fail;
    }
    gs_to_libc();
    fprintf(stderr, "[loader] TEB installed in %%gs\n");

    /* TLS (dir 9): テンプレート複製、teb->tls 配列を接続 */
    u32 tlsrva = f32((u64)e + 24 + 112 + 8 * 9);
    if (tlsrva) {
        if (!seg_ok(tlsrva, 40, PROT_READ)) goto bad_pe;
        u64 start = *(u64 *)(g_image + tlsrva);
        u64 end = *(u64 *)(g_image + tlsrva + 8);
        u64 idxaddr = *(u64 *)(g_image + tlsrva + 16);
        u64 cbaddr = *(u64 *)(g_image + tlsrva + 24);
        if (end < start) goto bad_pe;
        u64 tlen = end - start;
        /* テンプレートはイメージ内にあり、g_tls_block に収まらなければならない */
        if (tlen > sizeof g_tls_block || (tlen && !va_seg_ok(start, tlen, PROT_READ))) {
            fprintf(stderr, "[loader] !! TLS テンプレートが不正 (%llu bytes)\n",
                    (unsigned long long)tlen);
            goto fail;
        }
        if (idxaddr && !va_seg_ok(idxaddr, 4, PROT_READ | PROT_WRITE)) goto bad_pe;
        memset(g_tls_block, 0, sizeof g_tls_block);
        if (tlen) memcpy(g_tls_block, (void *)(uintptr_t)start, (size_t)tlen);
        g_tls_array[0] = g_tls_block;
        if (idxaddr) *(u32 *)(uintptr_t)idxaddr = 0;
        fprintf(stderr, "[loader] TLS template %llu bytes, callbacks@%#" PRIx64 "\n",
                (unsigned long long)tlen, cbaddr);
        if (cbaddr) {
            for (u64 a = cbaddr;; a += 8) {
                if (!va_seg_ok(a, 8, PROT_READ)) goto bad_pe;
                u64 cb = *(u64 *)(uintptr_t)a;
                if (!cb) break;
                if (!va_seg_ok(cb, 1, PROT_READ | PROT_EXEC)) goto bad_pe;
                void (MS * f)(void *, u32, void *) = (void *)(uintptr_t)cb;
                gs_to_teb(); /* DLL のコードは TEB コンテキストで実行 */
                f((void *)(uintptr_t)g_imagebase, DLL_PROCESS_ATTACH, 0);
                gs_to_libc();
            }
        }
    }

    /* entry point (DllMain via CRT startup) — TEB コンテキストで実行 */
    int (MS * DllMain)(void *, u32, void *) = (void *)(g_image + entry);
    fprintf(stderr, "[loader] calling entry (DllMain) ...\n");
    gs_to_teb();
    int r = DllMain((void *)(uintptr_t)g_imagebase, DLL_PROCESS_ATTACH, 0);
    gs_to_libc(); /* エンジン呼び出しの外では常に libc の %gs に戻す */
    fprintf(stderr, "[loader] DllMain returned %d\n", r);
    return r ? 0 : -2;

bad_pe:
    fprintf(stderr, "[loader] !! PE の構造が不正 — ロードを拒否\n");
fail:
    if (mfd >= 0) close(mfd);
    if (tmp_made) unlink(tmpname);
    free(img);
    if (mapped) {
        munmap(g_image, g_sizeofimage);
        g_image = NULL;
    }
    free(g_file);
    g_file = NULL;
    g_filelen = 0;
    g_sizeofimage = 0;
    g_nsegs = 0;
    return -1;
}

/* find an export by name */
static void *get_export(const char *want) {
    u32 e = f32(0x3c);
    u32 exprva = f32((u64)e + 24 + 112 + 8 * 0);
    if (!exprva || !seg_ok(exprva, 40, PROT_READ)) return 0;
    u32 nfuncs = *(u32 *)(g_image + exprva + 20);
    u32 nnames = *(u32 *)(g_image + exprva + 24);
    u32 fns = *(u32 *)(g_image + exprva + 28);
    u32 names = *(u32 *)(g_image + exprva + 32);
    u32 ords = *(u32 *)(g_image + exprva + 36);
    if (!seg_ok(fns, (u64)nfuncs * 4, PROT_READ) || !seg_ok(names, (u64)nnames * 4, PROT_READ) ||
        !seg_ok(ords, (u64)nnames * 2, PROT_READ))
        return 0;
    size_t wantlen = strlen(want) + 1;
    for (u32 i = 0; i < nnames; i++) {
        u32 nrva = *(u32 *)(g_image + names + (u64)i * 4);
        if (!seg_ok(nrva, wantlen, PROT_READ)) continue;
        if (!memcmp(g_image + nrva, want, wantlen)) {
            u16 ord = *(u16 *)(g_image + ords + (u64)i * 2);
            if (ord >= nfuncs) return 0;
            u32 frva = *(u32 *)(g_image + fns + (u64)ord * 4);
            if (!seg_ok(frva, 1, PROT_READ | PROT_EXEC)) return 0;
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
static u8 g_rec_template[FT_ENGINE_TEMPLATE_MAX]; /* enrollment template ~200KB (up to 18 sub-tpls) */
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

/* HRESULT (32bit)。関数ポインタの戻り値型を u32 にして eax だけを見る
 * (rax の上位 32bit は不定)。 */
#define FT_S_OK 0u
#define FT_E_FAIL 0x80004005u
#define FT_E_INVALIDARG 0x80070057u
#define FT_FAILED(hr) (((hr) & 0x80000000u) != 0)

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
    if (nshims == 0) register_shims(); /* 失敗後の再試行で二重登録しない */
    int lr = load_pe();
    if (lr != 0) return lr == -2 ? -2 : -1;
    void *q = get_export("WbioQueryEngineInterface");
    if (!q) return -2;
    /* HRESULT WbioQueryEngineInterface(PWINBIO_ENGINE_INTERFACE *EngineInterface) */
    u32 (MS * Query)(void **) = (void *)q;
    void *iface = 0;
    gs_to_teb();
    u32 qr = Query(&iface);
    gs_to_libc();
    if (qr != FT_S_OK || !iface) return -3;
    g_iface = iface;
    *(u64 *)(pipeline + 0x08) = ~0ULL;
    build_storage_vtable();
    *(u64 *)(pipeline + 0x20) = (u64)(uintptr_t)g_sensor_vt;
    *(u64 *)(pipeline + 0x28) = (u64)(uintptr_t)g_storage_vt;
    /* HRESULT EngineAdapterAttach(PWINBIO_PIPELINE Pipeline) */
    u32 (MS * Attach)(void *) = *(void **)((u8 *)iface + 32 + 0 * 8);
    gs_to_teb();
    u32 ar = Attach(pipeline);
    gs_to_libc();
    if (ar != FT_S_OK) return -4;
    u64 ctx = *(u64 *)(pipeline + 0x38);
    if (!ctx) return -4; /* EngineContext が設定されていない */
    *(u32 *)(uintptr_t)(ctx + 0x24) = ENG_W;
    *(u32 *)(uintptr_t)(ctx + 0x28) = ENG_H;
    g_loaded = 1;
    return 0;
}

void ft_engine_close(void) {
    /* Linux版の意図を踏襲: 固定ベースに1回ロードしたら破棄しない (no-op) */
}

/* 以下のエンジン呼び出しの引数型は winbio_adapter.h の公式シグネチャ
 * (Microsoft Learn: WINBIO_ENGINE_INTERFACE / PIBIO_ENGINE_*_FN) に合わせる。
 *   WINBIO_BIR_PURPOSE / WINBIO_BIOMETRIC_SUBTYPE / BOOLEAN = UCHAR (u8)
 *   WINBIO_REJECT_DETAIL = ULONG (u32)、SIZE_T = 8 バイト (u64)、HRESULT = u32
 * out 引数が指す変数のサイズを DLL が書き込むサイズと必ず一致させること。 */

uint32_t ft_engine_accept(const uint8_t *img, int sw, int sh, uint8_t purpose) {
    if (!g_loaded) return FT_E_FAIL;
    if (!img || sw <= 0 || sh <= 0) return FT_E_INVALIDARG;
    prepare_frame((const u8 *)img, sw, sh);
    int total = build_bir(small, ENG_W, ENG_H);
    u32 rej = 0; /* WINBIO_REJECT_DETAIL */
    /* HRESULT EngineAdapterAcceptSampleData(PWINBIO_PIPELINE, PWINBIO_BIR SampleBuffer,
     *     SIZE_T SampleSize, WINBIO_BIR_PURPOSE Purpose, PWINBIO_REJECT_DETAIL) */
    u32 (MS * Accept)(void *, void *, u64, u8, u32 *) = *(void **)((u8 *)g_iface + 32 + 8 * 8);
    gs_to_teb(); /* エンジン呼び出しは TEB コンテキストで */
    u32 hr = Accept(pipeline, birbuf, (u64)total, purpose, &rej);
    gs_to_libc();
    return hr;
}

void ft_engine_enroll_begin(void) {
    if (!g_loaded) return;
    g_have_record = 0;
    g_rec_tsize = 0;
    /* HRESULT EngineAdapterCreateEnrollment(PWINBIO_PIPELINE) */
    u32 (MS * Create)(void *) = *(void **)((u8 *)g_iface + 32 + 12 * 8);
    gs_to_teb();
    Create(pipeline);
    gs_to_libc();
}

int ft_engine_enroll_update(void) {
    if (!g_loaded) return 2;
    /* HRESULT EngineAdapterUpdateEnrollment(PWINBIO_PIPELINE, PWINBIO_REJECT_DETAIL)
     * HRESULT EngineAdapterGetEnrollmentStatus(PWINBIO_PIPELINE, PWINBIO_REJECT_DETAIL) */
    u32 (MS * Update)(void *, u32 *) = *(void **)((u8 *)g_iface + 32 + 13 * 8);
    u32 (MS * Status)(void *, u32 *) = *(void **)((u8 *)g_iface + 32 + 14 * 8);
    u32 urej = 0;
    gs_to_teb();
    u32 uhr = Update(pipeline, &urej);
    /* 成功は S_OK (完成) か WINBIO_I_MORE_DATA (継続) だけ。失敗 HRESULT
     * (WINBIO_E_BAD_CAPTURE 以外も含む) は受理として数えない。 */
    if (FT_FAILED(uhr) || urej) {
        gs_to_libc();
        return 2; /* frame rejected */
    }
    u32 srej = 0;
    u32 shr = Status(pipeline, &srej);
    gs_to_libc();
    return shr == FT_S_OK ? 0 : 1; /* 0 complete, 1 need more */
}

static void fill_identity(u8 *id) {
    memset(id, 0, 76);
    *(u32 *)id = 3;
    for (int i = 0; i < 16; i++) id[8 + i] = 0xA0 + i;
}

int ft_engine_enroll_commit(uint8_t **out, size_t *outlen) {
    if (!g_loaded || !out || !outlen) return -1;
    u8 id[76];
    fill_identity(id);
    g_have_record = 0;
    /* HRESULT EngineAdapterCommitEnrollment(PWINBIO_PIPELINE, PWINBIO_IDENTITY,
     *     WINBIO_BIOMETRIC_SUBTYPE SubFactor, PUCHAR PayloadBlob, SIZE_T PayloadBlobSize)
     * ペイロードは使わないので NULL / 0 を明示的に渡す (省略すると不定値が渡る)。 */
    u32 (MS * Commit)(void *, void *, u8, u8 *, u64) = *(void **)((u8 *)g_iface + 32 + 17 * 8);
    gs_to_teb();
    u32 cr = Commit(pipeline, id, 1, NULL, 0);
    gs_to_libc();
    if (cr != FT_S_OK || !g_have_record || g_rec_tsize == 0) return -1;
    *out = malloc((size_t)g_rec_tsize);
    if (!*out) return -2;
    memcpy(*out, g_rec_template, (size_t)g_rec_tsize);
    *outlen = (size_t)g_rec_tsize;
    return 0;
}

int ft_engine_verify(const uint8_t *tmpl, size_t tmpllen) {
    if (!g_loaded || !tmpl) return 0;
    if (tmpllen == 0 || tmpllen > sizeof g_rec_template) return 0;
    memcpy(g_rec_template, tmpl, tmpllen);
    g_rec_tsize = tmpllen;
    g_have_record = 1;
    u8 id[76];
    fill_identity(id);
    u8 m = 0;                             /* BOOLEAN Match */
    u8 *payload = 0, *hash = 0;           /* PUCHAR */
    u64 payload_size = 0, hash_size = 0;  /* SIZE_T (DLL は 8 バイト書き込む) */
    u32 rej = 0;                          /* WINBIO_REJECT_DETAIL */
    /* HRESULT EngineAdapterVerifyFeatureSet(PWINBIO_PIPELINE, PWINBIO_IDENTITY,
     *     WINBIO_BIOMETRIC_SUBTYPE SubFactor, PBOOLEAN Match,
     *     PUCHAR *PayloadBlob, PSIZE_T PayloadBlobSize,
     *     PUCHAR *HashValue, PSIZE_T HashSize, PWINBIO_REJECT_DETAIL RejectDetail) */
    u32 (MS * Verify)(void *, void *, u8, u8 *, u8 **, u64 *, u8 **, u64 *, u32 *) =
        *(void **)((u8 *)g_iface + 32 + 10 * 8);
    gs_to_teb();
    u32 hr = Verify(pipeline, id, 1, &m, &payload, &payload_size, &hash, &hash_size, &rej);
    gs_to_libc();
    /* fail-closed: HRESULT が S_OK で、かつ Match が TRUE (1) の時だけ一致とする */
    return (hr == FT_S_OK && m == 1) ? 1 : 0;
}
