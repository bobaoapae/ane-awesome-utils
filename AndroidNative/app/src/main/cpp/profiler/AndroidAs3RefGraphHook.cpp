// Phase 4b — Android AS3 reference graph hook implementation.
// See header for context + path discovery.

#include "AndroidAs3RefGraphHook.hpp"

#include <android/log.h>
#include <shadowhook.h>
#include <link.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "DeepProfilerController.hpp"

#define LOG_TAG "AneAs3RefGraph"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ane::profiler {
namespace {

// Build-id pinned offsets. addEventListener is HIGH confidence
// (cross-arch matched via callee-frequency); the others are TBD —
// install() skips uninstalled offsets gracefully so partial coverage
// can ship while RA finishes the rest.
struct BuildIdOffsets {
    const char* build_id_hex;
    std::uintptr_t addEventListener;
    std::uintptr_t removeEventListener;
    std::uintptr_t addChild;
    std::uintptr_t removeChild;
};

static constexpr BuildIdOffsets kKnownBuilds[] = {
    {
        "7dde220f62c90358cfc2cb082f5ce63ab0cd3966",
        // First guess (0x00c98060) was based on partial freq match; got
        // 0 hits on validation. Re-matched with full Windows freq pattern
        // (5-5-4-3-3-3-1-1-1-1) and 0x00c973b0 emerged as the candidate
        // matching the 3×freq3 inner-loop signature exactly.
        0x00c973b0,  // addEventListener — re-matched, awaiting validation
        0,           // removeEventListener — TBD
        0,           // addChild — TBD
        0,           // removeChild — TBD
    },
};

// AAPCS64 signature for EventDispatcher::addEventListener:
//   void addEventListener(this, type, listener, useCapture, priority, useWeakRef)
// x0 = this (EventDispatcher), x1 = type (Stringp), x2 = listener (Function)
typedef void (*addEventListener_t)(void* self, void* type, void* listener,
                                    long useCapture, long priority, long useWeakRef);

typedef void (*generic1arg_t)(void* self);
typedef void (*generic2arg_t)(void* self, void* arg);

static std::atomic<DeepProfilerController*> g_controller{nullptr};
static std::atomic<bool>                    g_active{false};

static std::atomic<std::uint64_t>           g_hits_addEventListener{0};
static std::atomic<std::uint64_t>           g_hits_removeEventListener{0};
static std::atomic<std::uint64_t>           g_hits_addChild{0};
static std::atomic<std::uint64_t>           g_hits_removeChild{0};

static thread_local bool                    t_in_hook = false;

static void* g_stub_addEventListener = nullptr;
static addEventListener_t g_orig_addEventListener = nullptr;

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
    const char* base = slash ? slash + 1 : name;
    if (strcmp(base, "libCore.so") != 0) return 0;
    auto* out = reinterpret_cast<LibcoreInfo*>(data);
    out->base = static_cast<std::uintptr_t>(info->dlpi_addr);
    extractBuildIdFromPhdrs(info, out);
    return 1;
}

static const BuildIdOffsets* findOffsetsForBuildId(const std::uint8_t* bytes,
                                                    std::size_t len) {
    char hex[81];
    if (len * 2 + 1 > sizeof(hex)) return nullptr;
    static const char* digits = "0123456789abcdef";
    for (std::size_t i = 0; i < len; i++) {
        hex[i*2 + 0] = digits[bytes[i] >> 4];
        hex[i*2 + 1] = digits[bytes[i] & 0xf];
    }
    hex[len*2] = '\0';
    for (const auto& b : kKnownBuilds) {
        if (strcmp(hex, b.build_id_hex) == 0) return &b;
    }
    LOGW("findOffsetsForBuildId: build-id %s not in known list", hex);
    return nullptr;
}

// Proxy for addEventListener — emit reference edge then forward.
//
// WARNING: AAPCS64 dictates that integer args 0..7 use x0..x7. Since
// addEventListener has 6 args, we capture x0..x5. Float-args we don't
// preserve (no float args in this API).
static void proxy_addEventListener(void* self, void* type, void* listener,
                                    long useCapture, long priority, long useWeakRef) {
    g_hits_addEventListener.fetch_add(1, std::memory_order_relaxed);

    if (t_in_hook || !g_active.load(std::memory_order_relaxed)) {
        if (g_orig_addEventListener)
            g_orig_addEventListener(self, type, listener, useCapture, priority, useWeakRef);
        return;
    }
    t_in_hook = true;

    DeepProfilerController* dpc = g_controller.load(std::memory_order_acquire);
    if (dpc != nullptr && self != nullptr && listener != nullptr) {
        char marker_value[256];
        std::snprintf(marker_value, sizeof(marker_value),
                      "{\"owner\":\"0x%llx\",\"dependent\":\"0x%llx\","
                      "\"kind\":\"EventListener\",\"useCapture\":%ld,"
                      "\"useWeakRef\":%ld}",
                      (unsigned long long)reinterpret_cast<std::uintptr_t>(self),
                      (unsigned long long)reinterpret_cast<std::uintptr_t>(listener),
                      useCapture & 0xff, useWeakRef & 0xff);
        dpc->marker("as3_ref_add", marker_value);
    }

    if (g_orig_addEventListener)
        g_orig_addEventListener(self, type, listener, useCapture, priority, useWeakRef);
    t_in_hook = false;
}

} // namespace

AndroidAs3RefGraphHook::~AndroidAs3RefGraphHook() {
    uninstall();
}

bool AndroidAs3RefGraphHook::install(DeepProfilerController* controller) {
    if (installed_) return true;
    if (controller == nullptr) return false;

    LibcoreInfo info{};
    dl_iterate_phdr(phdrCallback, &info);
    if (info.base == 0 || info.buildIdLen == 0) {
        LOGE("install: libCore.so not loaded or build-id missing");
        return false;
    }
    const BuildIdOffsets* offsets = findOffsetsForBuildId(info.buildId, info.buildIdLen);
    if (offsets == nullptr) return false;

    g_controller.store(controller, std::memory_order_release);
    g_hits_addEventListener.store(0);
    g_hits_removeEventListener.store(0);
    g_hits_addChild.store(0);
    g_hits_removeChild.store(0);

    // Hook addEventListener (HIGH confidence offset)
    if (offsets->addEventListener) {
        std::uintptr_t addr = info.base + offsets->addEventListener;
        g_stub_addEventListener = shadowhook_hook_func_addr(
            reinterpret_cast<void*>(addr),
            reinterpret_cast<void*>(&proxy_addEventListener),
            reinterpret_cast<void**>(&g_orig_addEventListener));
        if (g_stub_addEventListener == nullptr) {
            int err = shadowhook_get_errno();
            LOGE("install: addEventListener hook failed at 0x%lx errno=%d %s",
                 (unsigned long)addr, err, shadowhook_to_errmsg(err));
            g_controller.store(nullptr, std::memory_order_release);
            return false;
        }
        LOGI("install: addEventListener hooked (offset=0x%lx, addr=0x%lx, stub=%p)",
             (unsigned long)offsets->addEventListener, (unsigned long)addr,
             g_stub_addEventListener);
    } else {
        LOGW("install: addEventListener offset=0 (build-id not fully RA'd)");
    }

    // TBD: removeEventListener, addChild, removeChild — install when offsets known

    g_active.store(true, std::memory_order_release);
    installed_ = true;
    LOGI("install: AS3 ref graph hook OK (partial — only addEventListener)");
    return true;
}

void AndroidAs3RefGraphHook::uninstall() {
    if (!installed_) return;
    g_active.store(false, std::memory_order_release);
    if (g_stub_addEventListener) {
        shadowhook_unhook(g_stub_addEventListener);
        g_stub_addEventListener = nullptr;
    }
    g_orig_addEventListener = nullptr;
    g_controller.store(nullptr, std::memory_order_release);
    LOGI("uninstall: stats addEventListener=%llu removeEventListener=%llu "
         "addChild=%llu removeChild=%llu",
         (unsigned long long)g_hits_addEventListener.load(),
         (unsigned long long)g_hits_removeEventListener.load(),
         (unsigned long long)g_hits_addChild.load(),
         (unsigned long long)g_hits_removeChild.load());
    installed_ = false;
}

std::uint64_t AndroidAs3RefGraphHook::addEventListenerHits() const {
    return g_hits_addEventListener.load(std::memory_order_relaxed);
}
std::uint64_t AndroidAs3RefGraphHook::removeEventListenerHits() const {
    return g_hits_removeEventListener.load(std::memory_order_relaxed);
}
std::uint64_t AndroidAs3RefGraphHook::addChildHits() const {
    return g_hits_addChild.load(std::memory_order_relaxed);
}
std::uint64_t AndroidAs3RefGraphHook::removeChildHits() const {
    return g_hits_removeChild.load(std::memory_order_relaxed);
}

} // namespace ane::profiler
