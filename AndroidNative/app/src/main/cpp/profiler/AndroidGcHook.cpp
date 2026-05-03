// Phase 7a — Android GC observer hook implementation. See header.

#include "AndroidGcHook.hpp"

#include <android/log.h>
#include <shadowhook.h>
#include <link.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "DeepProfilerController.hpp"
#include "AneprofFormat.hpp"

#define LOG_TAG "AneGcHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ane::profiler {
namespace {

// Build-id pinned offsets for `MMgc::GC::Collect` (or its Scout-instrumented
// wrapper that calls into the actual collector). Located via RA on libCore.so
// 51.1.3.10 (Iter N+11+ in PROGRESS.md) — xref to ".gc.CollectionWork"
// telemetry tag string at .rodata page 0xf42000 + 0x10e.

struct BuildIdMatch {
    const char*    build_id_hex;
    std::uintptr_t collect_offset;
};

// AArch64 build-id 7dde220f62c90358cfc2cb082f5ce63ab0cd3966 → +0x896824
//   Function signature: void GC::Collect(this) — single ptr arg
//   Body emits .gc.CollectionWork Scout marker, dispatches to specialized
//   collection paths via flags at this+0x3b1, this+0x3b2, this+0xc20
//
// ARMv7 build-id 582a8f65b8dcb741e5eb869ccf9526137270d99e → +0x5501e0 (+1 thumb bit)
//   Same logical structure verified via Thumb-2 disasm:
//   - push {r4-r7, lr} + push.w {r8-r11} + vpush {d8-d10} + sub sp, #0x98
//   - mov r4, r0 (saves this)
//   - ldrb r0, [r4, #0xa] → already-running flag (offset differs from ARM64
//     this+0x9 due to ARMv7 alignment but same semantic)
//   - if non-zero, sets r9=1 (already running) and branches around work
//   - Inline literal pool with .gc.CollectionWork at +0x5501c4 (Adobe puts
//     read-only data in .text on ARMv7 build)
static constexpr BuildIdMatch kKnownBuilds[] = {
    {"7dde220f62c90358cfc2cb082f5ce63ab0cd3966", 0x896824},
    {"582a8f65b8dcb741e5eb869ccf9526137270d99e", 0x5501e1},  // +1 Thumb bit
};

// Function signature inferred from disassembly:
//   void GC::Collect(GC* this);
// AAPCS64: x0 = this, no return.
typedef void (*GCCollect_t)(void* gc_this);

static std::atomic<DeepProfilerController*> g_controller{nullptr};
static std::atomic<bool>                    g_active{false};
static std::atomic<std::uint64_t>           g_diag_collect_calls{0};
static thread_local bool                    t_in_gc_hook = false;

static void* g_stub_collect = nullptr;
static GCCollect_t g_orig_collect = nullptr;

struct LibcoreInfo {
    std::uintptr_t base = 0;
    std::uint8_t   buildId[40] = {};
    std::size_t    buildIdLen = 0;
};

static void extractBuildIdFromPhdrs(struct dl_phdr_info* info, LibcoreInfo* out) {
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const std::uint8_t* note = reinterpret_cast<const std::uint8_t*>(
            info->dlpi_addr + ph.p_vaddr);
        const std::uint8_t* end  = note + ph.p_memsz;
        while (note + 12 <= end) {
            std::uint32_t namesz = *reinterpret_cast<const std::uint32_t*>(note + 0);
            std::uint32_t descsz = *reinterpret_cast<const std::uint32_t*>(note + 4);
            std::uint32_t type   = *reinterpret_cast<const std::uint32_t*>(note + 8);
            const std::uint8_t* name = note + 12;
            const std::uint8_t* desc = name + ((namesz + 3) & ~3u);
            const std::uint8_t* next = desc + ((descsz + 3) & ~3u);
            if (type == 3 && namesz >= 4 &&
                std::memcmp(name, "GNU\0", 4) == 0 && descsz <= sizeof(out->buildId)) {
                std::memcpy(out->buildId, desc, descsz);
                out->buildIdLen = descsz;
                return;
            }
            note = next;
        }
    }
}

static int phdrCallback(struct dl_phdr_info* info, size_t, void* data) {
    const char* name = info->dlpi_name;
    if (name == nullptr) return 0;
    const char* slash = strrchr(name, '/');
    const char* base  = slash ? slash + 1 : name;
    if (strcmp(base, "libCore.so") != 0) return 0;
    auto* out = reinterpret_cast<LibcoreInfo*>(data);
    out->base = static_cast<std::uintptr_t>(info->dlpi_addr);
    extractBuildIdFromPhdrs(info, out);
    return 1;
}

static void buildIdToHex(const std::uint8_t* bytes, std::size_t len, char* out_hex) {
    static const char* digits = "0123456789abcdef";
    for (std::size_t i = 0; i < len; ++i) {
        out_hex[i * 2 + 0] = digits[bytes[i] >> 4];
        out_hex[i * 2 + 1] = digits[bytes[i] & 0xf];
    }
    out_hex[len * 2] = '\0';
}

static std::uintptr_t findOffsetForBuildId(const std::uint8_t* bytes, std::size_t len) {
    char hex[81];
    if (len * 2 + 1 > sizeof(hex)) return 0;
    buildIdToHex(bytes, len, hex);
    for (const auto& m : kKnownBuilds) {
        if (std::strcmp(hex, m.build_id_hex) == 0) return m.collect_offset;
    }
    LOGW("build-id %s: NOT in known list — GC observer disabled", hex);
    return 0;
}

static inline std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
}

static void proxy_GCCollect(void* gc_this) {
    if (t_in_gc_hook || !g_active.load(std::memory_order_relaxed) || g_orig_collect == nullptr) {
        if (g_orig_collect) g_orig_collect(gc_this);
        return;
    }
    t_in_gc_hook = true;
    g_diag_collect_calls.fetch_add(1, std::memory_order_relaxed);

    const std::uint64_t t0 = nowNs();
    g_orig_collect(gc_this);
    const std::uint64_t t1 = nowNs();

    DeepProfilerController* dpc = g_controller.load(std::memory_order_acquire);
    if (dpc != nullptr) {
        // Heap stats unavailable without RA on `GC::GetTotalGCBytes` (TBD —
        // would let us populate before/after live counts/bytes properly).
        // For now: emit GcCycle with timing-only payload — analyzer infers
        // pressure from alloc/free event deltas around the cycle window.
        // gc_id = start timestamp (cheap monotonic id).
        dpc->record_gc_cycle(
            /* gc_id            */ t0,
            /* kind             */ aneprof::GcCycleKind::NativeObserved,
            /* before_live_count*/ 0,
            /* before_live_bytes*/ 0,
            /* after_live_count */ 0,
            /* after_live_bytes */ 0,
            /* label            */ std::string(),
            /* flags            */ 0);
    }
    t_in_gc_hook = false;
}

} // namespace

AndroidGcHook::~AndroidGcHook() {
    uninstall();
}

bool AndroidGcHook::install(DeepProfilerController* controller) {
    if (installed_) return true;
    if (controller == nullptr) return false;

    LibcoreInfo info{};
    dl_iterate_phdr(phdrCallback, &info);
    if (info.base == 0 || info.buildIdLen == 0) {
        LOGE("install: libCore.so not loaded or build-id missing");
        return false;
    }
    std::uintptr_t offset = findOffsetForBuildId(info.buildId, info.buildIdLen);
    if (offset == 0) return false;

    std::uintptr_t target_addr = info.base + offset;
    g_controller.store(controller, std::memory_order_release);
    g_diag_collect_calls.store(0);

    g_stub_collect = shadowhook_hook_func_addr(
        reinterpret_cast<void*>(target_addr),
        reinterpret_cast<void*>(&proxy_GCCollect),
        reinterpret_cast<void**>(&g_orig_collect));

    if (g_stub_collect == nullptr) {
        int err = shadowhook_get_errno();
        LOGE("install: hook failed at 0x%lx: errno=%d %s",
             (unsigned long)target_addr, err, shadowhook_to_errmsg(err));
        g_controller.store(nullptr, std::memory_order_release);
        return false;
    }

    g_active.store(true, std::memory_order_release);
    installed_ = true;
    LOGI("install: GC observer hook OK (offset=0x%lx, target=0x%lx, stub=%p)",
         (unsigned long)offset, (unsigned long)target_addr, g_stub_collect);
    return true;
}

void AndroidGcHook::uninstall() {
    g_active.store(false, std::memory_order_release);
    if (g_stub_collect) {
        shadowhook_unhook(g_stub_collect);
        g_stub_collect = nullptr;
    }
    g_orig_collect = nullptr;
    g_controller.store(nullptr, std::memory_order_release);
    installed_ = false;
}

std::uint64_t AndroidGcHook::diagCollectCalls() const {
    return g_diag_collect_calls.load(std::memory_order_relaxed);
}

} // namespace ane::profiler
