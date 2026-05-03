// Native allocation tracer — uses ByteDance shadowhook (inline hooks) to
// instrument libc's malloc/calloc/realloc/free/mmap/munmap. Each allocation
// >= 64 KB whose CALLER lies inside libCore.so (Adobe AIR runtime) is
// recorded with full unwound stack into a live-allocation table; freed
// allocs remove their entry. dumpAllocs() returns the surviving allocations
// after a workload, ranked by size, with symbolized stack traces.
//
// Why shadowhook (not bytehook): bytehook is no longer published on Maven
// Central — only shadowhook 2.x is. shadowhook does inline patching, which
// hooks the symbol globally (no caller-filtering at hook level). We compensate
// by checking the immediate caller PC against the libCore.so address range
// in the proxy: only allocations originating from libCore.so are recorded.

#include <jni.h>
#include <android/log.h>
#include <shadowhook.h>
#include <unwind.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <string>

// Include the DeepProfilerController API so alloc_tracer can route alloc/
// free events into the .aneprof event stream when the deep profiler is
// recording. Defined in shared/profiler/include/DeepProfilerController.hpp;
// we keep a weak global pointer that AndroidProfilerBridge wires/unwires
// on profilerStart/Stop.
#include "DeepProfilerController.hpp"
#include <mutex>
#include <atomic>
#include <algorithm>
#include <sys/mman.h>
#include <sys/time.h>
#include <link.h>

#define LOG_TAG "AneAllocTracer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// ----- Configuration -----

// Minimum allocation size to track. Below this we skip the unwind/hashmap
// to keep hot paths fast. 64 KB is the boundary that shows up as
// "[anon:libc_malloc]" entries on Android scudo (one mmap per alloc).
static constexpr size_t kMinTrackSize = 64 * 1024;

// Maximum stack frames captured per alloc.
static constexpr size_t kMaxStackFrames = 16;

// Hard cap on the live-alloc table.
static constexpr size_t kMaxLiveAllocs = 50000;

// ----- Function pointer typedefs -----

typedef void* (*malloc_t)(size_t);
typedef void* (*calloc_t)(size_t, size_t);
typedef void* (*realloc_t)(void*, size_t);
typedef void  (*free_t)(void*);
typedef void* (*mmap_t)(void*, size_t, int, int, int, off_t);
typedef int   (*munmap_t)(void*, size_t);

// ----- State -----

struct AllocRecord {
    uintptr_t pc[kMaxStackFrames];
    size_t    nframes;
    size_t    size;
    int64_t   tsMs;
    int       kind;     // 0=malloc, 1=calloc, 2=realloc, 3=mmap
    int       phaseId;  // index into g_phases (0 = "<none>")
};

static std::mutex g_mutex;
static std::unordered_map<uintptr_t, AllocRecord> g_live;
static std::atomic<bool>   g_active{false};
static std::atomic<size_t> g_total_tracked{0};
static std::atomic<size_t> g_total_filtered{0};   // excluded by caller filter
static std::atomic<size_t> g_total_undersize{0};  // excluded by size filter
static std::atomic<size_t> g_total_dropped{0};    // excluded by table cap

// Phase attribution: AS3-side `markPhase("battle_start")` appends a string here
// and updates g_current_phase. Each AllocRecord captures phaseId at alloc time.
// Dump renders the resolved phase name per record.
static std::mutex                g_phases_mu;
static std::vector<std::string>  g_phases{"<none>"};  // index 0 = no phase set
static std::atomic<int>          g_current_phase{0};

static void* g_stub_malloc  = nullptr;
static void* g_stub_calloc  = nullptr;
static void* g_stub_realloc = nullptr;
static void* g_stub_free    = nullptr;
static void* g_stub_mmap    = nullptr;
static void* g_stub_munmap  = nullptr;

// Pointers to the original libc symbols, populated by shadowhook on hook.
static malloc_t  g_orig_malloc  = nullptr;
static calloc_t  g_orig_calloc  = nullptr;
static realloc_t g_orig_realloc = nullptr;
static free_t    g_orig_free    = nullptr;
static mmap_t    g_orig_mmap    = nullptr;
static munmap_t  g_orig_munmap  = nullptr;

// Re-entry guard owned by proxy entry. Phase 0 invariant: t_in_tracer is set
// the first time control enters any proxy (proxy_malloc/calloc/realloc/free/
// mmap/munmap) and cleared on exit. All inner tracer machinery (recordAlloc,
// captureStack, unordered_map insert, DeepProfilerController writes) runs WITH
// the guard set, so any internal allocation re-entering our proxies takes the
// early-return fast path.
//
// `tls_model("initial-exec")` forces the linker to allocate this TLS slot in
// the .tbss section at .so load time (as an ELF TLS via TPIDR_EL0 on ARM64 /
// TPIDRURW on ARMv7), NOT via __emutls_get_address. Without this attribute,
// the NDK toolchain falls back to emulated TLS for some reason (visible via
// llvm-readelf showing __emutls_v.* symbols in the .so), and the very first
// access from a fresh thread would call malloc to allocate the __emutls block
// — which itself goes through proxy_malloc, which itself accesses t_in_tracer,
// triggering infinite recursion → stack overflow → SIGSEGV.
static thread_local __attribute__((tls_model("initial-exec"))) bool t_in_tracer = false;

struct ScopedTracerGuard {
    ScopedTracerGuard() { t_in_tracer = true; }
    ~ScopedTracerGuard() { t_in_tracer = false; }
};

// libCore.so address range — populated on start() via dl_iterate_phdr.
static uintptr_t g_libcore_start = 0;
static uintptr_t g_libcore_end   = 0;

// DeepProfilerController integration — when the .aneprof deep profiler is
// recording, route alloc/free events through it so they appear in the
// .aneprof event stream alongside snapshots, markers, and (eventually) AS3
// stack annotations. Set/cleared by AndroidProfilerBridge during
// profilerStart/Stop. NULL when only the standalone JSONL alloc tracer is
// active (no overhead in that case).
static std::atomic<ane::profiler::DeepProfilerController*> g_dpc{nullptr};

// ----- libCore.so range discovery -----

static int phdrCallback(struct dl_phdr_info* info, size_t, void* data) {
    const char* name = info->dlpi_name;
    if (name == nullptr) name = "";
    LOGI("phdr: %s (addr=0x%lx)", name, (unsigned long)info->dlpi_addr);
    const char* slash = strrchr(name, '/');
    const char* base = slash ? slash + 1 : name;
    if (strcmp(base, "libCore.so") != 0) return 0;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const auto& p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD) continue;
        uintptr_t a = info->dlpi_addr + p.p_vaddr;
        uintptr_t b = a + p.p_memsz;
        if (a < lo) lo = a;
        if (b > hi) hi = b;
    }
    if (lo < hi) {
        auto* range = static_cast<std::pair<uintptr_t, uintptr_t>*>(data);
        range->first = lo;
        range->second = hi;
        return 1;  // stop iteration
    }
    return 0;
}

static bool discoverLibCoreRange() {
    std::pair<uintptr_t, uintptr_t> range{0, 0};
    dl_iterate_phdr(phdrCallback, &range);
    if (range.first == 0 || range.second == 0) {
        LOGE("libCore.so not loaded — cannot install caller filter");
        return false;
    }
    g_libcore_start = range.first;
    g_libcore_end   = range.second;
    LOGI("libCore.so range: 0x%lx-0x%lx (%lu bytes)",
         (unsigned long)g_libcore_start, (unsigned long)g_libcore_end,
         (unsigned long)(g_libcore_end - g_libcore_start));
    return true;
}

static inline bool callerInLibCore(uintptr_t pc) {
    return pc >= g_libcore_start && pc < g_libcore_end;
}

// ----- Stack unwind -----

struct UnwindCtx {
    uintptr_t* frames;
    size_t     max;
    size_t     count;
};

static _Unwind_Reason_Code unwindCallback(struct _Unwind_Context* ctx, void* arg) {
    UnwindCtx* uc = static_cast<UnwindCtx*>(arg);
    if (uc->count >= uc->max) return _URC_END_OF_STACK;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc == 0) return _URC_END_OF_STACK;
    uc->frames[uc->count++] = pc;
    return _URC_NO_REASON;
}

static size_t captureStack(uintptr_t* frames, size_t max) {
    UnwindCtx uc{frames, max, 0};
    _Unwind_Backtrace(unwindCallback, &uc);
    return uc.count;
}

static int64_t nowMs() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ----- Recording helpers -----

// Returns true if this allocation should be traced. Captures the stack as
// a side effect and writes it into outFrames.
//
// PRECONDITION: t_in_tracer is set (caller is the proxy entry guard via
// ScopedTracerGuard). Proxy already filtered by size+active+!in_tracer.
static bool shouldTrace(size_t size, uintptr_t* outFrames, size_t* outNFrames) {
    if (size < kMinTrackSize) {
        g_total_undersize.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    *outNFrames = captureStack(outFrames, kMaxStackFrames);

    // Caller filter: skip stacks where no frame is inside libCore.so. The
    // immediate caller is frame[1] (frame[0] is our proxy itself), but a
    // libc internal helper may push extra frames before reaching libCore.so
    // — so scan all captured frames.
    bool fromLibCore = false;
    for (size_t i = 0; i < *outNFrames; i++) {
        if (callerInLibCore(outFrames[i])) {
            fromLibCore = true;
            break;
        }
    }
    if (!fromLibCore) {
        g_total_filtered.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

static void recordAlloc(uintptr_t addr, size_t size, int kind) {
    if (addr == 0) return;
    uintptr_t pc[kMaxStackFrames];
    size_t nframes = 0;
    if (!shouldTrace(size, pc, &nframes)) return;

    AllocRecord rec;
    rec.size = size;
    rec.tsMs = nowMs();
    rec.kind = kind;
    rec.phaseId = g_current_phase.load(std::memory_order_relaxed);
    rec.nframes = nframes;
    memcpy(rec.pc, pc, nframes * sizeof(uintptr_t));

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_live.size() >= kMaxLiveAllocs) {
            g_total_dropped.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_live[addr] = rec;
            g_total_tracked.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Mirror the alloc into the .aneprof event stream when the deep profiler
    // is recording. The proxy entry guard already prevents re-entry, so any
    // allocations the controller's writer thread/queue makes recursively
    // through libc take the proxy's early-return path (small alloc) or
    // t_in_tracer-true path (large alloc).
    auto* dpc = g_dpc.load(std::memory_order_acquire);
    if (dpc != nullptr) {
        dpc->record_alloc_if_untracked(reinterpret_cast<void*>(addr),
                                        static_cast<std::uint64_t>(size));
    }
}

// PRECONDITION: t_in_tracer is set (caller is the proxy entry guard).
static void recordFree(uintptr_t addr) {
    if (addr == 0) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_live.erase(addr);
    }
    auto* dpc = g_dpc.load(std::memory_order_acquire);
    if (dpc != nullptr) {
        dpc->record_free_if_tracked(reinterpret_cast<void*>(addr));
    }
}

// ----- Hook proxies -----
//
// Phase 0 fix: each proxy filters by kMinTrackSize (or unconditional pass for
// free/munmap whose ptr was never tracked) BEFORE accessing t_in_tracer. The
// thread_local read on first access by a fresh thread can otherwise trigger
// __emutls_get_address → libc malloc → proxy_malloc → infinite recursion →
// stack overflow → SIGSEGV (NDK r28 + clang 19 emulated-TLS path on ARM64).
//
// Once we know the alloc is large enough to potentially track, we apply a
// ScopedTracerGuard that holds t_in_tracer=true for the entire recording
// lifecycle (captureStack, unordered_map insert, DPC mirror), so any nested
// libc call from inside takes the early-return fast path at proxy_malloc.
//
// For free/munmap, t_in_tracer is read only when g_active is true, since the
// vast majority of free() calls in production are for small allocations
// never recorded. This is a heuristic — recordFree will still erase from
// g_live whether it's tracked or not (linear hash, O(1) miss is fine).

static void* proxy_malloc(size_t size) {
    void* p = (g_orig_malloc != nullptr) ? g_orig_malloc(size) : nullptr;
    if (size < kMinTrackSize) return p;  // skip TLS access for small allocs
    if (t_in_tracer || !g_active.load(std::memory_order_relaxed) || !p) return p;
    ScopedTracerGuard guard;
    recordAlloc((uintptr_t)p, size, 0);
    return p;
}

static void* proxy_calloc(size_t nmemb, size_t size) {
    void* p = (g_orig_calloc != nullptr) ? g_orig_calloc(nmemb, size) : nullptr;
    size_t total = nmemb * size;
    if (total < kMinTrackSize) return p;
    if (t_in_tracer || !g_active.load(std::memory_order_relaxed) || !p) return p;
    ScopedTracerGuard guard;
    recordAlloc((uintptr_t)p, total, 1);
    return p;
}

static void* proxy_realloc(void* ptr, size_t size) {
    void* p = (g_orig_realloc != nullptr) ? g_orig_realloc(ptr, size) : nullptr;
    // Always need to drop the old ptr from g_live if it was tracked, even if
    // new size is small. Use the same TLS gate logic as malloc.
    if (size < kMinTrackSize && ptr == nullptr) return p;
    if (t_in_tracer || !g_active.load(std::memory_order_relaxed)) return p;
    ScopedTracerGuard guard;
    if (ptr)                         recordFree((uintptr_t)ptr);
    if (p && size >= kMinTrackSize)  recordAlloc((uintptr_t)p, size, 2);
    return p;
}

static void proxy_free(void* ptr) {
    if (ptr == nullptr) return;
    if (t_in_tracer || !g_active.load(std::memory_order_relaxed)) {
        if (g_orig_free) g_orig_free(ptr);
        return;
    }
    ScopedTracerGuard guard;
    recordFree((uintptr_t)ptr);
    if (g_orig_free) g_orig_free(ptr);
}

static void* proxy_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    void* p = (g_orig_mmap != nullptr) ? g_orig_mmap(addr, length, prot, flags, fd, offset)
                                       : MAP_FAILED;
    if (length < kMinTrackSize) return p;
    if (t_in_tracer || !g_active.load(std::memory_order_relaxed) || p == MAP_FAILED) return p;
    ScopedTracerGuard guard;
    recordAlloc((uintptr_t)p, length, 3);
    return p;
}

static int proxy_munmap(void* addr, size_t length) {
    if (length < kMinTrackSize || addr == nullptr) {
        return (g_orig_munmap != nullptr) ? g_orig_munmap(addr, length) : -1;
    }
    if (t_in_tracer || !g_active.load(std::memory_order_relaxed)) {
        return (g_orig_munmap != nullptr) ? g_orig_munmap(addr, length) : -1;
    }
    ScopedTracerGuard guard;
    recordFree((uintptr_t)addr);
    return (g_orig_munmap != nullptr) ? g_orig_munmap(addr, length) : -1;
}

// ----- Hook installation -----

// shadowhook 2.x rejects lib_name=NULL with SHADOWHOOK_ERRNO_INVALID_ARG (3).
// All allocator symbols we hook (malloc/calloc/realloc/free/mmap/munmap) live
// in bionic libc.so on every Android ABI we ship.
static void* hookOne(const char* sym, void* proxy, void** orig) {
    void* stub = shadowhook_hook_sym_name("libc.so", sym, proxy, orig);
    if (stub == nullptr) {
        LOGE("shadowhook_hook_sym_name(libc.so, %s) failed: errno=%d %s",
             sym, shadowhook_get_errno(), shadowhook_to_errmsg(shadowhook_get_errno()));
    }
    return stub;
}

static bool installHooks() {
    g_stub_malloc  = hookOne("malloc",  (void*)proxy_malloc,  reinterpret_cast<void**>(&g_orig_malloc));
    g_stub_calloc  = hookOne("calloc",  (void*)proxy_calloc,  reinterpret_cast<void**>(&g_orig_calloc));
    g_stub_realloc = hookOne("realloc", (void*)proxy_realloc, reinterpret_cast<void**>(&g_orig_realloc));
    g_stub_free    = hookOne("free",    (void*)proxy_free,    reinterpret_cast<void**>(&g_orig_free));
    g_stub_mmap    = hookOne("mmap",    (void*)proxy_mmap,    reinterpret_cast<void**>(&g_orig_mmap));
    g_stub_munmap  = hookOne("munmap",  (void*)proxy_munmap,  reinterpret_cast<void**>(&g_orig_munmap));
    LOGI("hooks: malloc=%p calloc=%p realloc=%p free=%p mmap=%p munmap=%p",
         g_stub_malloc, g_stub_calloc, g_stub_realloc, g_stub_free, g_stub_mmap, g_stub_munmap);
    return g_stub_malloc != nullptr;
}

static void uninstallHooks() {
    if (g_stub_malloc)  { shadowhook_unhook(g_stub_malloc);  g_stub_malloc  = nullptr; }
    if (g_stub_calloc)  { shadowhook_unhook(g_stub_calloc);  g_stub_calloc  = nullptr; }
    if (g_stub_realloc) { shadowhook_unhook(g_stub_realloc); g_stub_realloc = nullptr; }
    if (g_stub_free)    { shadowhook_unhook(g_stub_free);    g_stub_free    = nullptr; }
    if (g_stub_mmap)    { shadowhook_unhook(g_stub_mmap);    g_stub_mmap    = nullptr; }
    if (g_stub_munmap)  { shadowhook_unhook(g_stub_munmap);  g_stub_munmap  = nullptr; }
}

// ----- Symbolization -----

static bool symbolize(uintptr_t pc, char* out, size_t outSize) {
    Dl_info info;
    if (dladdr((void*)pc, &info) && info.dli_fbase) {
        uintptr_t off = pc - (uintptr_t)info.dli_fbase;
        const char* fname = info.dli_fname ? info.dli_fname : "?";
        const char* slash = strrchr(fname, '/');
        const char* base = slash ? slash + 1 : fname;
        if (info.dli_sname) {
            snprintf(out, outSize, "%s+0x%lx %s", base, (unsigned long)off, info.dli_sname);
        } else {
            snprintf(out, outSize, "%s+0x%lx", base, (unsigned long)off);
        }
        return true;
    }
    snprintf(out, outSize, "0x%lx", (unsigned long)pc);
    return false;
}

} // namespace

// Public C++ API (called from AndroidProfilerBridge.cpp during deep profiler
// start/stop) — wire/unwire the DeepProfilerController so alloc/free events
// flow into the .aneprof stream alongside the standalone JSONL ring.
namespace ane::alloc_tracer {

void setDeepProfilerController(ane::profiler::DeepProfilerController* dpc) {
    g_dpc.store(dpc, std::memory_order_release);
}

ane::profiler::DeepProfilerController* getDeepProfilerController() {
    return g_dpc.load(std::memory_order_acquire);
}

bool isActive() {
    return g_active.load(std::memory_order_acquire);
}

} // namespace ane::alloc_tracer

// ----- JNI entry points -----

extern "C" {

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_AllocTracer_nativeStart(JNIEnv* env, jclass) {
    LOGI("nativeStart: entering");
    if (g_active.exchange(true)) {
        LOGW("nativeStart: already active");
        return 0;
    }
    LOGI("nativeStart: calling discoverLibCoreRange");
    if (!discoverLibCoreRange()) {
        LOGE("nativeStart: discoverLibCoreRange returned false");
        g_active.store(false);
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_live.clear();
        g_total_tracked.store(0);
        g_total_filtered.store(0);
        g_total_undersize.store(0);
        g_total_dropped.store(0);
    }
    {
        std::lock_guard<std::mutex> lk(g_phases_mu);
        g_phases.clear();
        g_phases.emplace_back("<none>");
        g_current_phase.store(0);
    }
    LOGI("nativeStart: calling installHooks");
    if (!installHooks()) {
        LOGE("nativeStart: installHooks returned false");
        g_active.store(false);
        uninstallHooks();
        return -1;
    }
    LOGI("nativeStart: success");
    return 1;
}

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_AllocTracer_nativeStop(JNIEnv* env, jclass) {
    if (!g_active.exchange(false)) return 0;
    uninstallHooks();
    return 1;
}

// purgeStalePhase: walk g_live, free entries whose phase name contains the given
// substring AND whose tsMs is older than (nowMs - minAgeMs). Returns JSON with
// counts/bytes freed. Caller MUST guarantee:
//   - AS3 GC + scudo M_PURGE_ALL already ran (so AS3 references are gone)
//   - The phase substring corresponds to a logical scope that has fully ended
//     (e.g. battle exited and back in hall before purging "matchroom")
//   - minAgeMs > 0 (so we never free pointers from the live phase)
//
// Calling free() directly via libc bypasses any libCore.so XOR-canary verify
// path, so any subsequent libCore.so read of [ptr ^ canary] would mismatch and
// abort. That's why we only purge entries whose owning scope is dead.
JNIEXPORT jstring JNICALL
Java_br_com_redesurftank_aneawesomeutils_AllocTracer_nativePurgeStalePhase(
        JNIEnv* env, jclass, jstring jSubstr, jint jMinAgeMs, jint jMaxFree) {
    const char* c = jSubstr ? env->GetStringUTFChars(jSubstr, nullptr) : nullptr;
    std::string substr = c ? c : "";
    if (c) env->ReleaseStringUTFChars(jSubstr, c);
    int minAgeMs = jMinAgeMs > 0 ? jMinAgeMs : 1000;
    int maxFree = jMaxFree > 0 ? jMaxFree : 100000;

    std::vector<std::string> phases_copy;
    {
        std::lock_guard<std::mutex> lk(g_phases_mu);
        phases_copy = g_phases;
    }
    auto phaseMatches = [&](int id) -> bool {
        if (id < 0 || (size_t)id >= phases_copy.size()) return false;
        if (substr.empty()) return true;
        return phases_copy[id].find(substr) != std::string::npos;
    };

    int64_t now = nowMs();
    int64_t cutoff = now - minAgeMs;

    // Collect candidates first under lock, then free outside lock to keep
    // contention small. free() reentry into our hook is guarded by t_in_tracer.
    std::vector<std::pair<uintptr_t, size_t>> candidates;
    int matched = 0, scanned = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        scanned = (int)g_live.size();
        candidates.reserve(std::min<size_t>(g_live.size(), (size_t)maxFree));
        for (auto it = g_live.begin(); it != g_live.end();) {
            const AllocRecord& r = it->second;
            if (r.tsMs <= cutoff && phaseMatches(r.phaseId)) {
                candidates.emplace_back(it->first, r.size);
                it = g_live.erase(it);  // remove from live before free, else
                                        // proxy_free re-entry would try to erase
                                        // (no-op but extra mutex round-trip).
                matched++;
                if (matched >= maxFree) break;
            } else {
                ++it;
            }
        }
    }

    // Free outside the live-table lock. ScopedTracerGuard makes proxy_free
    // pass through without re-acquiring g_mutex (proxy_free reads t_in_tracer
    // before any g_active or recordFree work).
    int freed = 0;
    uint64_t freedBytes = 0;
    {
        ScopedTracerGuard guard;
        for (const auto& kv : candidates) {
            if (g_orig_free) {
                g_orig_free(reinterpret_cast<void*>(kv.first));
            } else {
                ::free(reinterpret_cast<void*>(kv.first));
            }
            freed++;
            freedBytes += kv.second;
        }
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"scanned\":%d,\"matched\":%d,\"freed\":%d,\"freedBytes\":%llu,"
             "\"substr\":\"%s\",\"minAgeMs\":%d,\"now\":%lld}",
             scanned, matched, freed, (unsigned long long)freedBytes,
             substr.c_str(), minAgeMs, (long long)now);
    LOGI("purgeStalePhase: scanned=%d matched=%d freed=%d bytes=%llu",
         scanned, matched, freed, (unsigned long long)freedBytes);
    return env->NewStringUTF(buf);
}

JNIEXPORT jstring JNICALL
Java_br_com_redesurftank_aneawesomeutils_AllocTracer_nativeDump(JNIEnv* env, jclass, jint topN) {
    std::vector<std::pair<uintptr_t, AllocRecord>> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        snapshot.reserve(g_live.size());
        for (const auto& kv : g_live) snapshot.emplace_back(kv.first, kv.second);
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const auto& a, const auto& b) { return a.second.size > b.second.size; });
    if (topN > 0 && (size_t)topN < snapshot.size()) snapshot.resize(topN);

    // Take a copy of phase names so we resolve under our own lock without
    // holding g_phases_mu across the per-record loop.
    std::vector<std::string> phases_copy;
    {
        std::lock_guard<std::mutex> lk(g_phases_mu);
        phases_copy = g_phases;
    }
    auto resolvePhase = [&](int id) -> const char* {
        if (id < 0 || (size_t)id >= phases_copy.size()) return "<oob>";
        return phases_copy[id].c_str();
    };

    std::string out;
    out.reserve(snapshot.size() * 256 + 256);
    char buf[64];

    out += "{\"active\":";
    out += g_active.load() ? "true" : "false";
    out += ",\"tracked\":";   snprintf(buf, sizeof(buf), "%zu", g_total_tracked.load());   out += buf;
    out += ",\"filtered\":";  snprintf(buf, sizeof(buf), "%zu", g_total_filtered.load());  out += buf;
    out += ",\"undersize\":"; snprintf(buf, sizeof(buf), "%zu", g_total_undersize.load()); out += buf;
    out += ",\"dropped\":";   snprintf(buf, sizeof(buf), "%zu", g_total_dropped.load());   out += buf;
    out += ",\"liveCount\":"; snprintf(buf, sizeof(buf), "%zu", snapshot.size());          out += buf;
    out += ",\"libCore\":\"";
    snprintf(buf, sizeof(buf), "0x%lx-0x%lx", (unsigned long)g_libcore_start, (unsigned long)g_libcore_end);
    out += buf;
    out += "\",\"phases\":[";
    for (size_t i = 0; i < phases_copy.size(); i++) {
        if (i > 0) out += ",";
        out += "\"";
        for (const char* p = phases_copy[i].c_str(); *p; p++) {
            if (*p == '"' || *p == '\\') out += '\\';
            out += *p;
        }
        out += "\"";
    }
    out += "],\"allocs\":[";

    bool first = true;
    char symBuf[256];
    for (const auto& kv : snapshot) {
        if (!first) out += ",";
        first = false;
        const auto& r = kv.second;
        out += "{\"addr\":\""; snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)kv.first); out += buf;
        out += "\",\"size\":"; snprintf(buf, sizeof(buf), "%zu", r.size); out += buf;
        out += ",\"kind\":";   snprintf(buf, sizeof(buf), "%d", r.kind); out += buf;
        out += ",\"tsMs\":";   snprintf(buf, sizeof(buf), "%lld", (long long)r.tsMs); out += buf;
        out += ",\"phase\":\"";
        for (const char* p = resolvePhase(r.phaseId); *p; p++) {
            if (*p == '"' || *p == '\\') out += '\\';
            out += *p;
        }
        out += "\",\"stack\":[";
        for (size_t i = 0; i < r.nframes; i++) {
            if (i > 0) out += ",";
            symbolize(r.pc[i], symBuf, sizeof(symBuf));
            out += "\"";
            for (const char* p = symBuf; *p; p++) {
                if (*p == '"' || *p == '\\') out += '\\';
                out += *p;
            }
            out += "\"";
        }
        out += "]}";
    }
    out += "]}";

    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_AllocTracer_nativeMarkPhase(
        JNIEnv* env, jclass, jstring jName) {
    const char* c = jName ? env->GetStringUTFChars(jName, nullptr) : nullptr;
    std::string name = c ? c : "";
    if (c) env->ReleaseStringUTFChars(jName, c);
    if (name.empty()) name = "<empty>";
    int id;
    {
        std::lock_guard<std::mutex> lk(g_phases_mu);
        g_phases.emplace_back(std::move(name));
        id = static_cast<int>(g_phases.size() - 1);
    }
    g_current_phase.store(id, std::memory_order_release);
    return id;
}

} // extern "C"
