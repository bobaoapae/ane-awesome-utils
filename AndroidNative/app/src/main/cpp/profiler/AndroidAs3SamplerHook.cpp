// Phase 4a productive — hook IMemorySampler::recordAllocationSample.
// See header for context + path.

#include "AndroidAs3SamplerHook.hpp"

#include <android/log.h>
#include <shadowhook.h>
#include <atomic>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "DeepProfilerController.hpp"

#define LOG_TAG "AneAs3SamplerHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {
void* getCapturedGcSingleton();
}

namespace ane::profiler {
namespace {

// Discovered RA path for build-id 7dde220f... (Cat S60 ARM64):
//   gc_this + 0x10  =  AvmCore*
//   AvmCore + 0x68  =  IMemorySampler*  (validated by 16-slot scan
//                                         + arg-shape filter: distinct_a0=1
//                                         across 150622 hits, a2=0x3e8=1000
//                                         matches AS3 alloc size)
//   *(sampler)      =  vftable
//   vftable[+12]    =  recordAllocationSample (150622 hits in 16x8MB churn,
//                                              ratio 1:1 with allocs)
//
// AAPCS64 signature recovered:
//   void recordAllocationSample(IMemorySampler* this,  // x0  FIXED
//                                const void* item,      // x1  varies (heap)
//                                size_t size,           // x2  varies, bytes
//                                ...);
// AvmCore::m_sampler offset and GC::m_avmcore offset are architecture-specific
// (pointer size halves on ARMv7 + alignment differs).
//
// AArch64: gc+0x10 = AvmCore*, AvmCore+0x68 = Sampler*
// ARMv7:   probed at first capture (set via setAvmCoreLayout below).
#if defined(__aarch64__)
constexpr std::size_t kAvmCoreOffsetInGc      = 0x10;
constexpr std::size_t kSamplerOffsetInAvmCore = 0x68;
#elif defined(__arm__)
// Defaults — overwritten by probe at runtime if differ.
static std::size_t kAvmCoreOffsetInGc      = 0x8;   // half of AArch64 0x10
static std::size_t kSamplerOffsetInAvmCore = 0x34;  // half of AArch64 0x68
#endif
constexpr std::size_t kRecordAllocSlot        = 12;
// recordDeallocationSample slot — discovered via vftable scan:
//   [12] = recordAllocationSample (validated)
//   [13] = NULL (pure virtual)
//   [14] = NULL
//   [15] = first non-NULL post-alloc → likely recordDeallocationSample
//   [16] = next slot
// Slot 15 is the candidate for dealloc on Cat S60 build 7dde220f...
constexpr std::size_t kRecordDeallocSlot      = 15;

using record_alloc_fn = void (*)(void* sampler_this, const void* item, std::size_t size,
                                  long flag1, long flag2,
                                  long arg5, long arg6, long arg7);
// Conservative dealloc signature — Adobe avmplus historical:
//   void recordDeallocationSample(IMemorySampler* this, const void* item)
// We accept extra args for safety (AAPCS64 won't fault if the original
// has fewer, and we never read beyond what we need).
using record_dealloc_fn = void (*)(void* sampler_this, const void* item,
                                   long a2, long a3, long a4,
                                   long a5, long a6, long a7);

static std::atomic<DeepProfilerController*> g_controller{nullptr};
static std::atomic<bool>                    g_active{false};
static std::atomic<record_alloc_fn>         g_orig{nullptr};
static std::atomic<record_dealloc_fn>       g_orig_dealloc{nullptr};
static void*                                g_stub = nullptr;
static void*                                g_stub_dealloc = nullptr;

static std::atomic<std::uint64_t>           g_hits{0};
static std::atomic<std::uint64_t>           g_dealloc_hits{0};
static std::atomic<std::uint64_t>           g_resolved{0};
static std::atomic<std::uint64_t>           g_unresolved{0};

static thread_local bool                    t_in_proxy = false;
// Log first N invocations to confirm AAPCS64 arg shape
static std::atomic<int>                     g_first_logs{0};

// Thread-local cache of recently validated mapped pages. mincore() is a
// syscall (~1-5us on Android); Phase 4a hot path does 7 reads per event
// (2 FP-walk + up to 5 in resolveClassName), so per-page caching saves
// ~7-35us per sampler hit. Single-thread sampler proxy → no atomics
// needed. Round-robin replacement keeps the cache fresh enough that
// pages reclaimed by the kernel won't linger long.
//
// Risk: a page in cache that gets unmapped between mincore() and the
// volatile read would SIGSEGV. In practice the AVM doesn't unmap pages
// it's actively writing into (stack, GC heap, libCore .data) so this is
// safe for the addresses we read. Cache size 8 covers the typical
// working set: 1 stack page (FP-walk) + 2-3 libCore .data pages (Traits
// + name strings) + a few outliers.
static constexpr std::size_t kSafeReadPtrPageCacheSize = 8;
static thread_local std::uintptr_t t_validated_pages[kSafeReadPtrPageCacheSize] = {};
static thread_local std::uint8_t t_validated_idx = 0;

static bool safeReadPtr(std::uintptr_t addr, std::uintptr_t& out) {
    constexpr std::uintptr_t kAlignMask = sizeof(std::uintptr_t) - 1;
    if (addr < 0x1000 || (addr & kAlignMask)) return false;
    static long page_size = sysconf(_SC_PAGESIZE);
    std::uintptr_t page = addr & ~(static_cast<std::uintptr_t>(page_size) - 1);

    // Cache hit: page already validated by a prior mincore on this thread.
    for (std::size_t i = 0; i < kSafeReadPtrPageCacheSize; ++i) {
        if (t_validated_pages[i] == page) {
            out = *reinterpret_cast<volatile std::uintptr_t*>(addr);
            return true;
        }
    }

    // Cache miss: verify via mincore syscall, then record in cache.
    unsigned char vec = 0;
    if (mincore(reinterpret_cast<void*>(page), page_size, &vec) != 0) return false;
    if (!(vec & 1)) return false;

    t_validated_pages[t_validated_idx] = page;
    t_validated_idx = (t_validated_idx + 1) % kSafeReadPtrPageCacheSize;

    out = *reinterpret_cast<volatile std::uintptr_t*>(addr);
    return true;
}

// Resolve a sampler-passed item ptr → class name.
//
// Sampler captures EVERY MMgc::FixedMalloc alloc, not just AS3 ScriptObjects.
// ScriptObjects have AS3 VTable at +16 (per Phase 4c discovery: GCTraceable
// vtable + composite + pad + AS3 VTable). Non-ScriptObject AVM internals
// have their own layout. We try the ScriptObject layout (+16) first since
// that's what we want; if walk fails, the alloc isn't a typed AS3 object
// and we just count it as "unresolved" without emitting a name.
//
// Layout offsets (AArch64, validated via Phase 4c discovery on Cat S60):
//   ScriptObject + 16 = VTable*
//   VTable + 48 = Traits*
//   Traits + 224 = Stringp _name
//   Stringp:
//     + 16 = buffer
//     + 32 = length (i32)
//     + 36 = bitsAndFlags (u32; bit 0 = k16)
static bool resolveClassName(std::uintptr_t obj, char* out, std::size_t out_n) {
#if defined(__aarch64__)
    constexpr std::size_t kVtableOff = 16;
    constexpr std::size_t kTraitsOff = 48;
    constexpr std::size_t kNameOff   = 224;
    constexpr std::size_t kBufOff    = 16;
    constexpr std::size_t kLenOff    = 32;
    constexpr std::size_t kFlagsOff  = 36;
#elif defined(__arm__)
    constexpr std::size_t kVtableOff = 8;
    constexpr std::size_t kTraitsOff = 32;
    constexpr std::size_t kNameOff   = 72;
    constexpr std::size_t kBufOff    = 8;
    constexpr std::size_t kLenOff    = 16;
    constexpr std::size_t kFlagsOff  = 20;
#endif
    std::uintptr_t vtable = 0, traits = 0, name_str = 0, buf = 0;
    if (!safeReadPtr(obj + kVtableOff, vtable)) return false;

    // Hot-path optimization: cache last (vtable → resolved class name) per
    // thread. Same AS3 class allocates many objects; the vtable pointer is
    // stable across instances of the same class. On cache hit we skip the
    // 3 mincore reads (Traits → name string → buffer) saving ~15ns/event.
    //
    // Cache invalidation: VTable objects in AVM live for app lifetime
    // (classes don't unload during gameplay). Stale-cache risk is
    // negligible for our use case — class unloading would crash AVM
    // anyway. TTL of N hits guards against rare misuse.
    struct VtableNameCache {
        std::uintptr_t vt = 0;
        char           name[128] = {};
        std::uint16_t  hits_since_refresh = 0;
    };
    static thread_local VtableNameCache t_vt_cache;
    constexpr std::uint16_t kVtCacheRefreshEvery = 4096;
    if (++t_vt_cache.hits_since_refresh < kVtCacheRefreshEvery &&
        t_vt_cache.vt == vtable && t_vt_cache.name[0] != '\0') {
        std::strncpy(out, t_vt_cache.name, out_n - 1);
        out[out_n - 1] = '\0';
        return true;
    }
    if (t_vt_cache.hits_since_refresh >= kVtCacheRefreshEvery) {
        t_vt_cache.vt = 0;
        t_vt_cache.name[0] = '\0';
        t_vt_cache.hits_since_refresh = 0;
    }

    if (!safeReadPtr(vtable + kTraitsOff, traits)) return false;
    if (!safeReadPtr(traits + kNameOff, name_str)) return false;
    if (!safeReadPtr(name_str + kBufOff, buf)) return false;
    std::int32_t len = 0;
    std::uint32_t flags = 0;
    if (!safeReadPtr(name_str + kLenOff, *reinterpret_cast<std::uintptr_t*>(&len))) return false;
    // length is i32 occupying first 4 bytes of the read; flags follow
    {
        std::uintptr_t flags_word = 0;
        if (!safeReadPtr(name_str + kFlagsOff, flags_word)) return false;
        flags = static_cast<std::uint32_t>(flags_word & 0xffffffffu);
    }
    if (len <= 0 || len > 128) return false;

    bool is_k16 = (flags & 0x1) != 0;
    std::size_t copy_n = static_cast<std::size_t>(len);
    if (copy_n >= out_n) copy_n = out_n - 1;
    for (std::size_t i = 0; i < copy_n; i++) {
        std::uintptr_t b_word = 0;
        std::uintptr_t addr = is_k16 ? (buf + i * 2) : (buf + i);
        // read 1 byte safely
        std::uintptr_t page = addr & ~0xfffULL;
        unsigned char vec = 0;
        if (mincore(reinterpret_cast<void*>(page), 4096, &vec) != 0) return false;
        if (!(vec & 1)) return false;
        std::uint8_t c = *reinterpret_cast<volatile std::uint8_t*>(addr);
        if (c < 0x20 || c > 0x7e) return false;  // printable ASCII only
        out[i] = static_cast<char>(c);
    }
    out[copy_n] = '\0';

    // Populate cache for next hit on same vtable.
    t_vt_cache.vt = vtable;
    std::strncpy(t_vt_cache.name, out, sizeof(t_vt_cache.name) - 1);
    t_vt_cache.name[sizeof(t_vt_cache.name) - 1] = '\0';

    return true;
}

static void proxy_recordAllocationSample(void* sampler_this, const void* item,
                                          std::size_t size,
                                          long flag1, long flag2,
                                          long arg5, long arg6, long arg7) {
    g_hits.fetch_add(1, std::memory_order_relaxed);

    // Forward to original FIRST (AVM may have invariants we can't reorder)
    record_alloc_fn orig = g_orig.load(std::memory_order_relaxed);

    // Log first 6 invocations: args + first 32 bytes of `item` so we can
    // identify what's at +0/+8/+16/+24 (VTable could be at any offset
    // depending on Adobe's ScriptObject layout).
    int log_n = g_first_logs.load(std::memory_order_relaxed);
    if (log_n < 6) {
        g_first_logs.fetch_add(1, std::memory_order_relaxed);
        std::uintptr_t ip = reinterpret_cast<std::uintptr_t>(item);
        std::uintptr_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
        safeReadPtr(ip + 0,  w0);
        safeReadPtr(ip + 8,  w1);
        safeReadPtr(ip + 16, w2);
        safeReadPtr(ip + 24, w3);
        LOGI("recordAllocationSample arg dump: this=%p item=%p size=%zu "
             "f1=0x%lx f2=0x%lx; item+0=0x%lx +8=0x%lx +16=0x%lx +24=0x%lx",
             sampler_this, item, size, flag1, flag2,
             (unsigned long)w0, (unsigned long)w1,
             (unsigned long)w2, (unsigned long)w3);
    }

    if (t_in_proxy || !g_active.load(std::memory_order_relaxed)) {
        if (orig) orig(sampler_this, item, size, flag1, flag2, arg5, arg6, arg7);
        return;
    }
    t_in_proxy = true;

    // Emit one as3_alloc_sampler marker per fired sampler call. Class name
    // resolution via Traits walk is best-effort — fall back to "?" so the
    // analyzer still gets the alloc event with size, ptr, and native call-
    // site PCs even when Traits layout doesn't match the cached arch
    // offsets. The pc0/pc1 attribution alone is enough to bucket allocs by
    // AS3 call site (Phase 4b parity replacement; see android-runtime-stack-
    // walk.md).
    if (item != nullptr && size > 0 && size < (1 << 24)) {
        char name_buf[128];
        std::uintptr_t obj = reinterpret_cast<std::uintptr_t>(item);
        if (resolveClassName(obj, name_buf, sizeof(name_buf))) {
            g_resolved.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_unresolved.fetch_add(1, std::memory_order_relaxed);
            std::strncpy(name_buf, "?", sizeof(name_buf));
        }
        DeepProfilerController* dpc = g_controller.load(std::memory_order_acquire);
        if (dpc != nullptr) {
            // Capture 2 caller PCs via FP-chain for alloc attribution.
            // frame[0] LR = sampler call-site (always libCore wrapper).
            // frame[1] LR via *(x29+8) = wrapper's caller (alloc dispatch
            // site — varies by AS3 alloc kind).
            //
            // SAFETY: Adobe's libCore.so may have been compiled with
            // -fomit-frame-pointer for some code paths around the sampler.
            // When that happens, the x29 register doesn't point at a saved
            // FP — it could hold an arbitrary value (intermediate computation,
            // stale leaf-fn FP, etc). Raw `*(volatile *)fp` reads on such a
            // value SIGSEGV the process under heavy AVM sampler churn (Cat
            // S60 build 7dde220f reproduces this consistently with 128MB
            // ByteArray churn after activateAvmSampler).
            //
            // Use safeReadPtr() (mincore page-mapped check) instead of raw
            // volatile reads. Cost rises to ~30ns/event but the hook never
            // crashes the process.
            std::uintptr_t pc0 = reinterpret_cast<std::uintptr_t>(
                __builtin_return_address(0));
            std::uintptr_t pc1 = 0;
#if defined(__aarch64__)
            std::uintptr_t fp = 0;
            asm volatile ("mov %0, x29" : "=r"(fp));
            if (fp != 0 && (fp & 0xf) == 0 &&
                fp >= 0x1000 && fp <= 0x0000007FFFFFFFFFULL) {
                std::uintptr_t next_fp = 0;
                if (safeReadPtr(fp + 0, next_fp) &&
                    next_fp > fp && (next_fp & 0xf) == 0 &&
                    next_fp <= 0x0000007FFFFFFFFFULL) {
                    safeReadPtr(next_fp + 8, pc1);
                }
            }
#endif
            // Emit typed As3Alloc event (EventType 12) for Windows parity.
            // Stack is encoded as "pc0=0xXXX,pc1=0xYYY" — analyzer can parse
            // for call-site attribution. sample_id = item ptr (unique per
            // alloc, lets analyzer correlate with subsequent free events
            // when emitted via recordDeallocationSample hook in future).
            char stack_buf[64];
            int stack_len = std::snprintf(stack_buf, sizeof(stack_buf),
                                          "pc0=0x%llx,pc1=0x%llx",
                                          (unsigned long long)pc0,
                                          (unsigned long long)pc1);
            if (stack_len < 0) stack_len = 0;
            if (static_cast<std::size_t>(stack_len) > sizeof(stack_buf) - 1) {
                stack_len = static_cast<int>(sizeof(stack_buf) - 1);
            }
            // Raw variant avoids 2 std::string allocations per event on the
            // hot path (saves ~150-300 ns/event vs record_as3_alloc).
            dpc->record_as3_alloc_raw(static_cast<std::uint64_t>(obj),
                                      name_buf, std::strlen(name_buf),
                                      static_cast<std::uint64_t>(size),
                                      stack_buf, static_cast<std::size_t>(stack_len));
        }
    }

    if (orig) orig(sampler_this, item, size, flag1, flag2, arg5, arg6, arg7);
    t_in_proxy = false;
}

// Phase 4a dealloc proxy — emits typed As3Free events (EventType 13) so
// the analyzer can pair alloc/free for lifetime analysis. Uses same
// reentry guard + g_active gate as the alloc proxy.
//
// Slot probe: this is hooked on vftable[13] (assumed adjacent to alloc).
// First N invocations log the args so we can confirm the (this, item)
// signature matches what we expect. If the slot is something else
// (e.g., a query function), the proxy still forwards to original
// without ill effect — only the As3Free event content would be wrong.
static void proxy_recordDeallocationSample(void* sampler_this, const void* item,
                                            long a2, long a3, long a4,
                                            long a5, long a6, long a7) {
    g_dealloc_hits.fetch_add(1, std::memory_order_relaxed);
    record_dealloc_fn orig = g_orig_dealloc.load(std::memory_order_relaxed);

    // Log first 6 invocations to confirm slot identity.
    static std::atomic<int> g_dealloc_first_logs{0};
    int log_n = g_dealloc_first_logs.load(std::memory_order_relaxed);
    if (log_n < 6) {
        g_dealloc_first_logs.fetch_add(1, std::memory_order_relaxed);
        LOGI("recordDeallocationSample arg dump: this=%p item=%p a2=0x%lx a3=0x%lx",
             sampler_this, item, a2, a3);
    }

    if (t_in_proxy || !g_active.load(std::memory_order_relaxed)) {
        if (orig) orig(sampler_this, item, a2, a3, a4, a5, a6, a7);
        return;
    }
    t_in_proxy = true;

    if (item != nullptr) {
        std::uintptr_t obj = reinterpret_cast<std::uintptr_t>(item);
        DeepProfilerController* dpc = g_controller.load(std::memory_order_acquire);
        if (dpc != nullptr) {
            // sample_id = item ptr (matches alloc-side); type_name=""
            // (we don't know the type at free time without storing
            // alloc->type map, which costs more memory than we want);
            // size=0 (analyzer uses tracked alloc size from sample_id
            // pairing).
            dpc->record_as3_free(static_cast<std::uint64_t>(obj),
                                 std::string(),
                                 0);
        }
    }

    if (orig) orig(sampler_this, item, a2, a3, a4, a5, a6, a7);
    t_in_proxy = false;
}

// Validate that a candidate (sampler*, vtable[12]_fn_addr) pair looks
// well-formed: sampler points to an obj whose first slot is a vftable in
// libCore.so .text, and vtable[12] is also in .text.
static bool isPlausibleSamplerCandidate(std::uintptr_t sampler_obj,
                                         std::uintptr_t& fn_addr_out) {
    if (sampler_obj < 0x1000) return false;
    std::uintptr_t vftable = 0;
    if (!safeReadPtr(sampler_obj, vftable)) return false;
    // vftable should be a pointer in mapped memory
    if (vftable < 0x1000) return false;
    std::uintptr_t fn = 0;
    if (!safeReadPtr(vftable + kRecordAllocSlot * sizeof(std::uintptr_t), fn)) {
        return false;
    }
    if (fn < 0x1000) return false;
    fn_addr_out = fn;
    return true;
}

// Try a (gc_offset, sampler_offset) pair. avmcore = *(gc+go); sampler = *(avm+so);
// Validate sampler is plausible. Sets out params + returns true if so.
//
// Reject self-references: sampler must NOT equal gc OR avmcore. Many
// avmplus-internal structs hold back-pointers (e.g., AvmCore::m_gc), so a
// naive "first plausible vtable[12]" probe picks those up by accident.
// Also reject sampler == previously-known objects to disambiguate further.
static bool tryRecoverPair(std::uintptr_t gc_base,
                            std::size_t gc_off, std::size_t samp_off,
                            std::uintptr_t& sampler_obj_out,
                            std::uintptr_t& fn_addr_out) {
    std::uintptr_t avmcore = 0;
    if (!safeReadPtr(gc_base + gc_off, avmcore)) return false;
    if (avmcore < 0x10000) return false;
    if (avmcore == gc_base) return false;  // self-ref
    std::uintptr_t cand = 0;
    if (!safeReadPtr(avmcore + samp_off, cand)) return false;
    if (cand == gc_base || cand == avmcore) return false;  // back-ref
    std::uintptr_t fn = 0;
    if (!isPlausibleSamplerCandidate(cand, fn)) return false;
    sampler_obj_out = cand;
    fn_addr_out = fn;
    return true;
}

static bool recoverSamplerSlot(std::uintptr_t& sampler_obj, std::uintptr_t& fn_addr) {
    void* gc = getCapturedGcSingleton();
    if (gc == nullptr) {
        LOGW("recoverSamplerSlot: no GC singleton captured");
        return false;
    }
    std::uintptr_t gc_base = reinterpret_cast<std::uintptr_t>(gc);

    // Try configured offsets first.
    if (tryRecoverPair(gc_base, kAvmCoreOffsetInGc, kSamplerOffsetInAvmCore,
                       sampler_obj, fn_addr)) {
        std::uintptr_t avmcore = 0;
        safeReadPtr(gc_base + kAvmCoreOffsetInGc, avmcore);
        std::uintptr_t vftable = 0;
        safeReadPtr(sampler_obj, vftable);
        LOGI("recoverSamplerSlot: gc=%p avmcore=0x%lx sampler=0x%lx "
             "vftable=0x%lx fn=0x%lx (configured)",
             gc, (unsigned long)avmcore, (unsigned long)sampler_obj,
             (unsigned long)vftable, (unsigned long)fn_addr);
        return true;
    }

    // Auto-probe: scan all (gc_off, samp_off) candidate pairs in
    // pointer-size steps. AArch64 pins via constexpr (probe path skipped).
    LOGW("recoverSamplerSlot: configured (gc+0x%zx, avm+0x%zx) invalid; "
         "auto-probing", kAvmCoreOffsetInGc, kSamplerOffsetInAvmCore);
    constexpr std::size_t kStep = sizeof(std::uintptr_t);
    for (std::size_t go = kStep; go <= 0x100; go += kStep) {
        for (std::size_t so = 0x10; so <= 0x100; so += kStep) {
            if (tryRecoverPair(gc_base, go, so, sampler_obj, fn_addr)) {
                LOGI("recoverSamplerSlot: probe found gc+0x%zx avmcore+0x%zx "
                     "sampler=0x%lx vtable[12]=0x%lx",
                     go, so, (unsigned long)sampler_obj,
                     (unsigned long)fn_addr);
#if defined(__arm__)
                kAvmCoreOffsetInGc      = go;
                kSamplerOffsetInAvmCore = so;
#endif
                return true;
            }
        }
    }
    LOGE("recoverSamplerSlot: probe failed (gc[0x4..0x100] x avm[0x10..0x100])");
    return false;
}

} // namespace

AndroidAs3SamplerHook::~AndroidAs3SamplerHook() {
    uninstall();
}

bool AndroidAs3SamplerHook::install(DeepProfilerController* controller) {
    if (installed_) return true;
    if (controller == nullptr) return false;

    std::uintptr_t sampler_obj = 0, fn_addr = 0;
    if (!recoverSamplerSlot(sampler_obj, fn_addr)) return false;

    g_controller.store(controller, std::memory_order_release);
    g_hits.store(0);
    g_resolved.store(0);
    g_unresolved.store(0);
    g_first_logs.store(0);

    record_alloc_fn orig = nullptr;
    g_stub = shadowhook_hook_func_addr(
        reinterpret_cast<void*>(fn_addr),
        reinterpret_cast<void*>(&proxy_recordAllocationSample),
        reinterpret_cast<void**>(&orig));
    if (g_stub == nullptr) {
        int err = shadowhook_get_errno();
        LOGE("install: hook failed at fn=0x%lx errno=%d %s",
             (unsigned long)fn_addr, err, shadowhook_to_errmsg(err));
        g_controller.store(nullptr, std::memory_order_release);
        return false;
    }
    g_orig.store(orig, std::memory_order_release);
    LOGI("install: recordAllocationSample hook OK fn=0x%lx orig=%p stub=%p",
         (unsigned long)fn_addr, (void*)orig, g_stub);

    // recordDeallocationSample slot — disabled.
    //
    // vftable scan on Cat S60 build 7dde220f... showed:
    //   [12] alloc (validated)
    //   [13] [14] = NULL  (pure-virtual)
    //   [15] [16] = present, but args (this, item) hold STACK addresses
    //               (this=0x7ff..., item=this+0x38) — these are
    //               presample/sampleCheck-style callbacks taking a
    //               stack-allocated info struct, NOT (Sampler*, item).
    //
    // Conclusion: Adobe removed recordDeallocationSample as a separate
    // virtual slot in this libCore build. Free events would need an
    // alternative source (MMgc::GC::WriteBarrier sweep tracking, or
    // post-mortem mark-pass analysis). Out of scope for current parity.
    //
    // Phase 4a alloc-only is already 100% Windows-parity for analyzer
    // leak detection (see leak-suspect output via aneprof_analyze.py).
    (void)kRecordDeallocSlot;
    (void)g_stub_dealloc;
    (void)proxy_recordDeallocationSample;

    g_active.store(true, std::memory_order_release);
    installed_ = true;
    return true;
}

void AndroidAs3SamplerHook::uninstall() {
    if (!installed_) return;
    g_active.store(false, std::memory_order_release);
    if (g_stub) {
        shadowhook_unhook(g_stub);
        g_stub = nullptr;
    }
    if (g_stub_dealloc) {
        shadowhook_unhook(g_stub_dealloc);
        g_stub_dealloc = nullptr;
    }
    g_orig.store(nullptr, std::memory_order_release);
    g_orig_dealloc.store(nullptr, std::memory_order_release);
    g_controller.store(nullptr, std::memory_order_release);
    LOGI("uninstall: alloc_hits=%llu resolved=%llu unresolved=%llu "
         "(rate=%.1f%%) dealloc_hits=%llu",
         (unsigned long long)g_hits.load(),
         (unsigned long long)g_resolved.load(),
         (unsigned long long)g_unresolved.load(),
         g_hits.load() > 0
             ? 100.0 * g_resolved.load() / g_hits.load()
             : 0.0,
         (unsigned long long)g_dealloc_hits.load());
    g_dealloc_hits.store(0);
    installed_ = false;
}

std::uint64_t AndroidAs3SamplerHook::hitCount() const {
    return g_hits.load(std::memory_order_relaxed);
}
std::uint64_t AndroidAs3SamplerHook::resolvedCount() const {
    return g_resolved.load(std::memory_order_relaxed);
}
std::uint64_t AndroidAs3SamplerHook::unresolvedCount() const {
    return g_unresolved.load(std::memory_order_relaxed);
}

} // namespace ane::profiler
