// Phase 4b RA tooling — generic experiment hook.

#include "AndroidExperimentHook.hpp"

#include <android/log.h>
#include <shadowhook.h>
#include <link.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define LOG_TAG "AneExperimentHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ane::profiler {
namespace {

constexpr std::size_t kMaxHooks = 32;

using generic_fn = long (*)(long, long, long, long, long, long, long, long);

struct ArgSnap {
    long a0, a1, a2, a3, a4, a5;
};

struct HookSlot {
    std::atomic<bool>          taken{false};
    std::uintptr_t             libcore_offset = 0;
    char                       label[64] = {};
    void*                      stub = nullptr;
    std::atomic<generic_fn>    orig{nullptr};
    std::atomic<std::uint64_t> hits{0};
    ArgSnap                    first_args[5]{};
    std::atomic<int>           args_captured{0};
};

static HookSlot g_slots[kMaxHooks];
static thread_local bool t_in_proxy = false;

// 32 distinct proxy functions — one per slot. Each has its own static
// `slot_idx` so it can route to the right slot without lookup.
#define DEFINE_PROXY(N)                                                       \
    static long proxy_##N(long a0, long a1, long a2, long a3,                 \
                           long a4, long a5, long a6, long a7) {              \
        HookSlot& slot = g_slots[N];                                          \
        std::uint64_t cnt = slot.hits.fetch_add(1, std::memory_order_relaxed); \
        if (cnt < 5) {                                                        \
            int idx = slot.args_captured.fetch_add(1, std::memory_order_relaxed); \
            if (idx < 5) {                                                    \
                slot.first_args[idx] = {a0, a1, a2, a3, a4, a5};              \
            }                                                                 \
        }                                                                     \
        if (t_in_proxy) {                                                     \
            generic_fn fn = slot.orig.load(std::memory_order_relaxed);        \
            return fn ? fn(a0,a1,a2,a3,a4,a5,a6,a7) : 0;                      \
        }                                                                     \
        t_in_proxy = true;                                                    \
        generic_fn fn = slot.orig.load(std::memory_order_relaxed);            \
        long rv = fn ? fn(a0,a1,a2,a3,a4,a5,a6,a7) : 0;                       \
        t_in_proxy = false;                                                   \
        return rv;                                                            \
    }

DEFINE_PROXY(0)  DEFINE_PROXY(1)  DEFINE_PROXY(2)  DEFINE_PROXY(3)
DEFINE_PROXY(4)  DEFINE_PROXY(5)  DEFINE_PROXY(6)  DEFINE_PROXY(7)
DEFINE_PROXY(8)  DEFINE_PROXY(9)  DEFINE_PROXY(10) DEFINE_PROXY(11)
DEFINE_PROXY(12) DEFINE_PROXY(13) DEFINE_PROXY(14) DEFINE_PROXY(15)
DEFINE_PROXY(16) DEFINE_PROXY(17) DEFINE_PROXY(18) DEFINE_PROXY(19)
DEFINE_PROXY(20) DEFINE_PROXY(21) DEFINE_PROXY(22) DEFINE_PROXY(23)
DEFINE_PROXY(24) DEFINE_PROXY(25) DEFINE_PROXY(26) DEFINE_PROXY(27)
DEFINE_PROXY(28) DEFINE_PROXY(29) DEFINE_PROXY(30) DEFINE_PROXY(31)
#undef DEFINE_PROXY

#define PE(N) (const void*)&proxy_##N
static const void* kProxyTable[kMaxHooks] = {
    PE(0),  PE(1),  PE(2),  PE(3),  PE(4),  PE(5),  PE(6),  PE(7),
    PE(8),  PE(9),  PE(10), PE(11), PE(12), PE(13), PE(14), PE(15),
    PE(16), PE(17), PE(18), PE(19), PE(20), PE(21), PE(22), PE(23),
    PE(24), PE(25), PE(26), PE(27), PE(28), PE(29), PE(30), PE(31),
};
#undef PE

static int phdr_cb(struct dl_phdr_info* info, size_t, void* data) {
    const char* name = info->dlpi_name;
    if (name == nullptr) return 0;
    const char* slash = strrchr(name, '/');
    const char* base = slash ? slash + 1 : name;
    if (strcmp(base, "libCore.so") != 0) return 0;
    *reinterpret_cast<std::uintptr_t*>(data) = static_cast<std::uintptr_t>(info->dlpi_addr);
    return 1;
}

static std::uintptr_t getLibCoreBase() {
    std::uintptr_t base = 0;
    dl_iterate_phdr(phdr_cb, &base);
    return base;
}

} // namespace

int AndroidExperimentHook::install(std::uintptr_t libcore_offset, const char* label) {
    std::uintptr_t base = getLibCoreBase();
    if (base == 0) {
        LOGE("install[%s, off=0x%lx]: libCore.so not loaded",
             label ? label : "?", (unsigned long)libcore_offset);
        return -1;
    }

    // Find free slot AND check no duplicate offset
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        if (g_slots[i].taken.load(std::memory_order_acquire) &&
            g_slots[i].libcore_offset == libcore_offset) {
            LOGW("install[%s, off=0x%lx]: already hooked in slot %zu",
                 label ? label : "?", (unsigned long)libcore_offset, i);
            return -1;
        }
    }
    int slot_idx = -1;
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        bool expected = false;
        if (g_slots[i].taken.compare_exchange_strong(expected, true,
                                                      std::memory_order_acq_rel)) {
            slot_idx = static_cast<int>(i);
            break;
        }
    }
    if (slot_idx < 0) {
        LOGW("install: no free slot (%zu max)", kMaxHooks);
        return -1;
    }

    HookSlot& slot = g_slots[slot_idx];
    slot.libcore_offset = libcore_offset;
    if (label) std::strncpy(slot.label, label, sizeof(slot.label) - 1);
    slot.label[sizeof(slot.label) - 1] = '\0';
    slot.hits.store(0);
    slot.args_captured.store(0);

    std::uintptr_t target = base + libcore_offset;
    generic_fn orig = nullptr;
    slot.stub = shadowhook_hook_func_addr(
        reinterpret_cast<void*>(target),
        const_cast<void*>(kProxyTable[slot_idx]),
        reinterpret_cast<void**>(&orig));
    if (slot.stub == nullptr) {
        int err = shadowhook_get_errno();
        LOGE("install[%s, off=0x%lx]: shadowhook failed errno=%d %s",
             label ? label : "?", (unsigned long)libcore_offset,
             err, shadowhook_to_errmsg(err));
        slot.taken.store(false, std::memory_order_release);
        return -1;
    }
    slot.orig.store(orig, std::memory_order_release);
    LOGI("install[%s, slot=%d]: offset=0x%lx target=0x%lx orig=%p stub=%p",
         label ? label : "?", slot_idx,
         (unsigned long)libcore_offset, (unsigned long)target,
         (void*)orig, slot.stub);
    return slot_idx;
}

long AndroidExperimentHook::hitsForOffset(std::uintptr_t libcore_offset) {
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        if (g_slots[i].taken.load(std::memory_order_acquire) &&
            g_slots[i].libcore_offset == libcore_offset) {
            return static_cast<long>(g_slots[i].hits.load(std::memory_order_relaxed));
        }
    }
    return -1;
}

void AndroidExperimentHook::uninstallAll() {
    LOGI("uninstallAll: hit counts + arg snapshots:");
    int active = 0;
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        if (!g_slots[i].taken.load(std::memory_order_acquire)) continue;
        active++;
        HookSlot& slot = g_slots[i];
        std::uint64_t hits = slot.hits.load(std::memory_order_relaxed);
        LOGI("  slot[%zu] '%s' off=0x%lx  hits=%llu",
             i, slot.label, (unsigned long)slot.libcore_offset,
             (unsigned long long)hits);
        int n_args = slot.args_captured.load(std::memory_order_relaxed);
        if (n_args > 5) n_args = 5;
        for (int a = 0; a < n_args; a++) {
            const ArgSnap& s = slot.first_args[a];
            LOGI("    call[%d]  a0=0x%lx  a1=0x%lx  a2=0x%lx  a3=0x%lx  a4=0x%lx  a5=0x%lx",
                 a,
                 (unsigned long)s.a0, (unsigned long)s.a1, (unsigned long)s.a2,
                 (unsigned long)s.a3, (unsigned long)s.a4, (unsigned long)s.a5);
        }
        if (slot.stub) {
            shadowhook_unhook(slot.stub);
            slot.stub = nullptr;
        }
        slot.orig.store(nullptr, std::memory_order_release);
        slot.taken.store(false, std::memory_order_release);
    }
    LOGI("uninstallAll: cleared %d active hooks", active);
}

int AndroidExperimentHook::activeSlots() {
    int n = 0;
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        if (g_slots[i].taken.load(std::memory_order_relaxed)) n++;
    }
    return n;
}

} // namespace ane::profiler
