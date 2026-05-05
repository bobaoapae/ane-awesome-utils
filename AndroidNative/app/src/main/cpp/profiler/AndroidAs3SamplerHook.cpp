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
//   AvmCore + 0xa0  =  IMemorySampler*
//   *(sampler)      =  vftable
//   vftable[+7]     =  recordAllocationSample (288 hits/40k allocs in churn)
constexpr std::size_t kSamplerOffsetInAvmCore = 0xa0;
constexpr std::size_t kRecordAllocSlot        = 7;

using record_alloc_fn = void (*)(void* sampler_this, const void* item, std::size_t size,
                                  long flag1, long flag2,
                                  long arg5, long arg6, long arg7);

static std::atomic<DeepProfilerController*> g_controller{nullptr};
static std::atomic<bool>                    g_active{false};
static std::atomic<record_alloc_fn>         g_orig{nullptr};
static void*                                g_stub = nullptr;

static std::atomic<std::uint64_t>           g_hits{0};
static std::atomic<std::uint64_t>           g_resolved{0};
static std::atomic<std::uint64_t>           g_unresolved{0};

static thread_local bool                    t_in_proxy = false;
// Log first N invocations to confirm AAPCS64 arg shape
static std::atomic<int>                     g_first_logs{0};

static bool safeReadPtr(std::uintptr_t addr, std::uintptr_t& out) {
    if (addr < 0x1000 || (addr & 7)) return false;
    static long page_size = sysconf(_SC_PAGESIZE);
    std::uintptr_t page = addr & ~(static_cast<std::uintptr_t>(page_size) - 1);
    unsigned char vec = 0;
    if (mincore(reinterpret_cast<void*>(page), page_size, &vec) != 0) return false;
    if (!(vec & 1)) return false;
    out = *reinterpret_cast<volatile std::uintptr_t*>(addr);
    return true;
}

// Resolve a ScriptObject ptr → class name. Reuses the same VTable→Traits→
// _name walk as AndroidAs3ObjectHook (Phase 4c), but on a verified
// AS3-typed alloc passed by the sampler (not a guess from FixedMalloc).
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
    return true;
}

static void proxy_recordAllocationSample(void* sampler_this, const void* item,
                                          std::size_t size,
                                          long flag1, long flag2,
                                          long arg5, long arg6, long arg7) {
    g_hits.fetch_add(1, std::memory_order_relaxed);

    // Forward to original FIRST (AVM may have invariants we can't reorder)
    record_alloc_fn orig = g_orig.load(std::memory_order_relaxed);

    // Log first 3 invocations so we can verify the AAPCS64 arg shape
    int log_n = g_first_logs.load(std::memory_order_relaxed);
    if (log_n < 3) {
        g_first_logs.fetch_add(1, std::memory_order_relaxed);
        LOGI("recordAllocationSample arg dump: this=%p item=%p size=%zu "
             "f1=0x%lx f2=0x%lx a5=0x%lx",
             sampler_this, item, size, flag1, flag2, arg5);
    }

    if (t_in_proxy || !g_active.load(std::memory_order_relaxed)) {
        if (orig) orig(sampler_this, item, size, flag1, flag2, arg5, arg6, arg7);
        return;
    }
    t_in_proxy = true;

    // Resolve class name via Traits walk
    if (item != nullptr && size > 0 && size < (1 << 24)) {
        char name_buf[128];
        std::uintptr_t obj = reinterpret_cast<std::uintptr_t>(item);
        bool ok = resolveClassName(obj, name_buf, sizeof(name_buf));
        if (ok) {
            g_resolved.fetch_add(1, std::memory_order_relaxed);
            DeepProfilerController* dpc = g_controller.load(std::memory_order_acquire);
            if (dpc != nullptr) {
                char marker_value[256];
                std::snprintf(marker_value, sizeof(marker_value),
                              "{\"class\":\"%s\",\"size\":%zu,\"ptr\":\"0x%llx\"}",
                              name_buf, size,
                              (unsigned long long)obj);
                dpc->marker("as3_alloc_sampler", marker_value);
            }
        } else {
            g_unresolved.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (orig) orig(sampler_this, item, size, flag1, flag2, arg5, arg6, arg7);
    t_in_proxy = false;
}

static bool recoverSamplerSlot(std::uintptr_t& sampler_obj, std::uintptr_t& fn_addr) {
    void* gc = getCapturedGcSingleton();
    if (gc == nullptr) {
        LOGW("recoverSamplerSlot: no GC singleton captured");
        return false;
    }
    std::uintptr_t avmcore = 0;
    if (!safeReadPtr(reinterpret_cast<std::uintptr_t>(gc) + 0x10, avmcore)) {
        LOGE("recoverSamplerSlot: gc+0x10 not readable");
        return false;
    }
    if (!safeReadPtr(avmcore + kSamplerOffsetInAvmCore, sampler_obj)) {
        LOGE("recoverSamplerSlot: avmcore+0x%zx not readable",
             kSamplerOffsetInAvmCore);
        return false;
    }
    std::uintptr_t vftable = 0;
    if (!safeReadPtr(sampler_obj, vftable)) {
        LOGE("recoverSamplerSlot: sampler vftable not readable");
        return false;
    }
    if (!safeReadPtr(vftable + kRecordAllocSlot * 8, fn_addr)) {
        LOGE("recoverSamplerSlot: vftable[%zu] not readable",
             kRecordAllocSlot);
        return false;
    }
    LOGI("recoverSamplerSlot: gc=%p avmcore=0x%lx sampler=0x%lx vftable=0x%lx fn=0x%lx",
         gc, (unsigned long)avmcore, (unsigned long)sampler_obj,
         (unsigned long)vftable, (unsigned long)fn_addr);
    return true;
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
    g_active.store(true, std::memory_order_release);
    installed_ = true;
    LOGI("install: recordAllocationSample hook OK fn=0x%lx orig=%p stub=%p",
         (unsigned long)fn_addr, (void*)orig, g_stub);
    return true;
}

void AndroidAs3SamplerHook::uninstall() {
    if (!installed_) return;
    g_active.store(false, std::memory_order_release);
    if (g_stub) {
        shadowhook_unhook(g_stub);
        g_stub = nullptr;
    }
    g_orig.store(nullptr, std::memory_order_release);
    g_controller.store(nullptr, std::memory_order_release);
    LOGI("uninstall: stats hits=%llu resolved=%llu unresolved=%llu (rate=%.1f%%)",
         (unsigned long long)g_hits.load(),
         (unsigned long long)g_resolved.load(),
         (unsigned long long)g_unresolved.load(),
         g_hits.load() > 0
             ? 100.0 * g_resolved.load() / g_hits.load()
             : 0.0);
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
