// Phase 5 — Android deep memory hook implementation. See header.

#include "AndroidDeepMemoryHook.hpp"

#include <android/log.h>
#include <shadowhook.h>
#include <link.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "DeepProfilerController.hpp"

#define LOG_TAG "AneDeepMemHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ane::profiler {
namespace {

// Build-id pinned offsets for `GCHeap::Alloc` (alloc-with-retry slow path).
// Located via RA on libCore.so 51.1.3.10 (Iter N+10 in PROGRESS.md).
//
// To extend to a new build-id:
//   1. Extract libCore.so for both archs from AIRSDK_<ver>/runtimes/air/android/device/
//   2. Find function calling `b malloc@plt` (or a wrapper that does) with retry-loop pattern
//   3. Add entry below.

struct BuildIdMatch {
    const char*   build_id_hex;     // hex string, no separators
    std::uintptr_t alloc_offset;    // GCHeap::Alloc entry from libCore.so base
};

// AArch64 build-id 7dde220f62c90358cfc2cb082f5ce63ab0cd3966 → 0x89c42c
// ARMv7  build-id 582a8f65b8dcb741e5eb869ccf9526137270d99e → TBD (next iter)
static constexpr BuildIdMatch kKnownBuilds[] = {
    {"7dde220f62c90358cfc2cb082f5ce63ab0cd3966", 0x89c42c},
};

// Function signature inferred from disassembly of libCore.so+0x89c42c (ARM64):
//   void* GCHeap::Alloc(size_t size, int flags);
// AAPCS64: x0 = size, x1 = flags, return in x0.
typedef void* (*GCHeapAlloc_t)(std::size_t size, int flags);

// ----- State -----

static std::atomic<DeepProfilerController*> g_controller{nullptr};
static std::atomic<bool>                    g_active{false};
static std::atomic<std::uint64_t>           g_diag_alloc_calls{0};
static std::atomic<std::uint64_t>           g_diag_alloc_bytes{0};
static std::atomic<std::uint64_t>           g_diag_alloc_failures{0};

// Re-entry guard — record_alloc into DPC may itself trigger inner libc allocs
// that go through alloc_tracer's libc shadowhook; if they re-enter our proxy
// (only possible if alloc_tracer is wired to also call us), we'd loop. The
// guard is the same pattern as alloc_tracer's t_in_tracer.
static thread_local bool t_in_deep_hook = false;

static void* g_stub_alloc                = nullptr;
static GCHeapAlloc_t g_orig_alloc        = nullptr;

// ----- libCore.so + build-id discovery -----

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
            if (type == 3 /* NT_GNU_BUILD_ID */ && namesz >= 4 &&
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

static void buildIdToHex(const std::uint8_t* bytes, std::size_t len,
                         char* out_hex /* must be 2*len+1 chars */) {
    static const char* digits = "0123456789abcdef";
    for (std::size_t i = 0; i < len; ++i) {
        out_hex[i * 2 + 0] = digits[bytes[i] >> 4];
        out_hex[i * 2 + 1] = digits[bytes[i] & 0xf];
    }
    out_hex[len * 2] = '\0';
}

// Returns matching offset (non-zero) or 0 if no match.
static std::uintptr_t findOffsetForBuildId(const std::uint8_t* bytes, std::size_t len) {
    char hex[81];
    if (len * 2 + 1 > sizeof(hex)) return 0;
    buildIdToHex(bytes, len, hex);
    for (const auto& m : kKnownBuilds) {
        if (std::strcmp(hex, m.build_id_hex) == 0) {
            LOGI("build-id matched: %s → offset 0x%lx", hex, (unsigned long)m.alloc_offset);
            return m.alloc_offset;
        }
    }
    LOGW("build-id %s: NOT in known list (%zu entries) — deep memory hook disabled",
         hex, sizeof(kKnownBuilds) / sizeof(kKnownBuilds[0]));
    return 0;
}

// ----- Proxy -----

static void* proxy_GCHeapAlloc(std::size_t size, int flags) {
    if (t_in_deep_hook) {
        return g_orig_alloc ? g_orig_alloc(size, flags) : nullptr;
    }
    t_in_deep_hook = true;
    g_diag_alloc_calls.fetch_add(1, std::memory_order_relaxed);

    void* p = g_orig_alloc ? g_orig_alloc(size, flags) : nullptr;

    if (p == nullptr) {
        g_diag_alloc_failures.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_diag_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
        if (g_active.load(std::memory_order_acquire)) {
            DeepProfilerController* dpc = g_controller.load(std::memory_order_acquire);
            if (dpc != nullptr) {
                dpc->record_alloc_if_untracked(p, static_cast<std::uint64_t>(size));
            }
        }
    }
    t_in_deep_hook = false;
    return p;
}

} // namespace

AndroidDeepMemoryHook::~AndroidDeepMemoryHook() {
    uninstall();
}

bool AndroidDeepMemoryHook::install(DeepProfilerController* controller) {
    if (installed_) return true;
    if (controller == nullptr) {
        LOGE("install: controller is null");
        return false;
    }

    LibcoreInfo info{};
    dl_iterate_phdr(phdrCallback, &info);
    if (info.base == 0) {
        LOGE("install: libCore.so not loaded");
        return false;
    }
    if (info.buildIdLen == 0) {
        LOGE("install: libCore.so build-id not found in PT_NOTE");
        return false;
    }

    std::uintptr_t offset = findOffsetForBuildId(info.buildId, info.buildIdLen);
    if (offset == 0) {
        // Unknown build-id — refuse to install rather than blindly patch a
        // possibly-different function on a new SDK. RA must be re-done for
        // the new build-id (see header comment).
        return false;
    }

    std::uintptr_t target_addr = info.base + offset;
    LOGI("install: libCore.so base=0x%lx + offset=0x%lx → target=0x%lx",
         (unsigned long)info.base, (unsigned long)offset, (unsigned long)target_addr);

    g_controller.store(controller, std::memory_order_release);
    g_diag_alloc_calls.store(0);
    g_diag_alloc_bytes.store(0);
    g_diag_alloc_failures.store(0);

    g_stub_alloc = shadowhook_hook_func_addr(
        reinterpret_cast<void*>(target_addr),
        reinterpret_cast<void*>(&proxy_GCHeapAlloc),
        reinterpret_cast<void**>(&g_orig_alloc));

    if (g_stub_alloc == nullptr) {
        int err = shadowhook_get_errno();
        LOGE("install: hook failed at 0x%lx: errno=%d %s",
             (unsigned long)target_addr, err, shadowhook_to_errmsg(err));
        g_controller.store(nullptr, std::memory_order_release);
        return false;
    }

    g_active.store(true, std::memory_order_release);
    installed_ = true;
    LOGI("install: deep memory hook OK (stub=%p, orig=%p)", g_stub_alloc, g_orig_alloc);
    return true;
}

void AndroidDeepMemoryHook::uninstall() {
    g_active.store(false, std::memory_order_release);
    if (g_stub_alloc) {
        shadowhook_unhook(g_stub_alloc);
        g_stub_alloc = nullptr;
    }
    g_orig_alloc = nullptr;
    g_controller.store(nullptr, std::memory_order_release);
    installed_ = false;
}

std::uint64_t AndroidDeepMemoryHook::diagAllocCalls()    const { return g_diag_alloc_calls.load(std::memory_order_relaxed); }
std::uint64_t AndroidDeepMemoryHook::diagAllocBytes()    const { return g_diag_alloc_bytes.load(std::memory_order_relaxed); }
std::uint64_t AndroidDeepMemoryHook::diagAllocFailures() const { return g_diag_alloc_failures.load(std::memory_order_relaxed); }

} // namespace ane::profiler
