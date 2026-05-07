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

// Caller-PC histogram per slot. Small fixed bucket — collisions OK
// (we only need to identify HOT callers, not enumerate all). 32 buckets
// per slot × 32 slots = 1024 atomic counters total.
//
// We capture up to 4 stack frames per hit: the immediate caller (frame 0)
// is usually a GC/alloc helper; the deeper frames (1-3) reach the AS3
// native method or AVM2 dispatcher.
constexpr std::size_t kCallerBuckets = 32;
constexpr int kStackDepth = 12;

struct HookSlot {
    std::atomic<bool>          taken{false};
    std::uintptr_t             libcore_offset = 0;
    char                       label[64] = {};
    void*                      stub = nullptr;
    std::atomic<generic_fn>    orig{nullptr};
    std::atomic<std::uint64_t> hits{0};
    ArgSnap                    first_args[5]{};
    std::atomic<int>           args_captured{0};

    // Per-frame caller-PC histograms. frame 0 = immediate caller (LR),
    // frame 1+ = walked via FP chain. AS3 native impls usually appear
    // at frame 2-3 (after MMgc helpers).
    std::atomic<std::uintptr_t> caller_pc[kStackDepth][kCallerBuckets]{};
    std::atomic<std::uint64_t>  caller_count[kStackDepth][kCallerBuckets]{};
};

static HookSlot g_slots[kMaxHooks];
static thread_local bool t_in_proxy = false;

// Record the caller-PC at a specific stack frame into the slot's
// histogram (lossy hash table).
static inline void recordCallerPcAtDepth(HookSlot& slot, int depth, std::uintptr_t pc) {
    if (pc == 0 || depth < 0 || depth >= kStackDepth) return;
    // Mix bits then mask. Caller PCs are aligned (4-byte) so rotate
    // before hashing to avoid clustering.
    std::uintptr_t h = pc ^ (pc >> 13);
    std::size_t idx = static_cast<std::size_t>(h & (kCallerBuckets - 1));
    auto& pc_table  = slot.caller_pc[depth];
    auto& cnt_table = slot.caller_count[depth];
    for (int probe = 0; probe < 4; probe++) {
        std::size_t b = (idx + probe) & (kCallerBuckets - 1);
        std::uintptr_t cur = pc_table[b].load(std::memory_order_acquire);
        if (cur == pc) {
            cnt_table[b].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (cur == 0) {
            std::uintptr_t expected = 0;
            if (pc_table[b].compare_exchange_strong(
                    expected, pc, std::memory_order_acq_rel)) {
                cnt_table[b].fetch_add(1, std::memory_order_relaxed);
                return;
            }
            cur = pc_table[b].load(std::memory_order_acquire);
            if (cur == pc) {
                cnt_table[b].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
}

// Walk the AArch64 frame-pointer chain starting at the saved FP for
// THIS function (= our proxy). Captures up to kStackDepth frame's LR.
//
// Convention: x29 is FP. [x29 + 0] = saved FP (next-up frame), [x29 + 8]
// = saved LR (return address into caller). We start ONE level up so
// frame 0 == our proxy's caller.
//
// Bounded loop with sanity check on each FP — abort if FP looks
// invalid (not aligned, not in heap-stack range).
static inline void recordStackTrace(HookSlot& slot) {
    // __builtin_return_address(0) is reliable across compilers — gives
    // proxy's immediate caller (LR at proxy entry).
    std::uintptr_t pc0 = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    recordCallerPcAtDepth(slot, 0, pc0);

#if defined(__aarch64__)
    // Frame-pointer walk for deeper frames. Read x29 (current FP).
    std::uintptr_t fp;
    asm volatile ("mov %0, x29" : "=r"(fp));
    for (int depth = 1; depth < kStackDepth; depth++) {
        // Validate FP: pointer-aligned, within reasonable range
        if (fp == 0 || (fp & 0xf) != 0) break;
        if (fp < 0x1000 || fp > 0x0000007FFFFFFFFFULL) break;
        // [fp + 0] = saved FP, [fp + 8] = saved LR
        std::uintptr_t next_fp = *reinterpret_cast<volatile std::uintptr_t*>(fp + 0);
        std::uintptr_t lr      = *reinterpret_cast<volatile std::uintptr_t*>(fp + 8);
        recordCallerPcAtDepth(slot, depth, lr);
        if (next_fp <= fp) break;  // FP must grow upward (stack); abort if not
        fp = next_fp;
    }
#endif
}

// 32 distinct proxy functions — one per slot. Each has its own static
// `slot_idx` so it can route to the right slot without lookup.
#define DEFINE_PROXY(N)                                                       \
    static long proxy_##N(long a0, long a1, long a2, long a3,                 \
                           long a4, long a5, long a6, long a7) {              \
        HookSlot& slot = g_slots[N];                                          \
        std::uint64_t cnt = slot.hits.fetch_add(1, std::memory_order_relaxed); \
        recordStackTrace(slot);                                               \
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
        // Caller-PC histogram per stack frame depth. Frame 0 = LR
        // (immediate caller), Frame N = walked via FP chain.
        struct CallerEntry { std::uintptr_t pc; std::uint64_t cnt; };
        for (int depth = 0; depth < kStackDepth; depth++) {
            CallerEntry top[kCallerBuckets];
            std::size_t n = 0;
            for (std::size_t b = 0; b < kCallerBuckets; b++) {
                std::uintptr_t pc = slot.caller_pc[depth][b].load(std::memory_order_relaxed);
                std::uint64_t c  = slot.caller_count[depth][b].load(std::memory_order_relaxed);
                if (pc != 0 && c > 0) top[n++] = {pc, c};
            }
            for (std::size_t a = 1; a < n; a++) {
                CallerEntry e = top[a];
                std::size_t b = a;
                while (b > 0 && top[b-1].cnt < e.cnt) { top[b] = top[b-1]; b--; }
                top[b] = e;
            }
            if (n > 0) {
                LOGI("    frame[%d] (top %zu):", depth, n > 6 ? (std::size_t)6 : n);
                for (std::size_t k = 0; k < n && k < 6; k++) {
                    // Resolve PC to library name + offset via dl_iterate_phdr.
                    struct PcResolve {
                        std::uintptr_t target;
                        char           name[128];
                        std::uintptr_t base;
                        bool           found;
                        static int cb(struct dl_phdr_info* info, size_t, void* d) {
                            auto* r = reinterpret_cast<PcResolve*>(d);
                            std::uintptr_t base = info->dlpi_addr;
                            // Walk segments to find one containing target.
                            for (int i = 0; i < info->dlpi_phnum; i++) {
                                const auto& ph = info->dlpi_phdr[i];
                                if (ph.p_type != PT_LOAD) continue;
                                std::uintptr_t lo = base + ph.p_vaddr;
                                std::uintptr_t hi = lo + ph.p_memsz;
                                if (r->target >= lo && r->target < hi) {
                                    const char* nm = info->dlpi_name;
                                    if (!nm || !*nm) nm = "(self)";
                                    const char* slash = strrchr(nm, '/');
                                    const char* base_nm = slash ? slash + 1 : nm;
                                    std::strncpy(r->name, base_nm, sizeof(r->name) - 1);
                                    r->name[sizeof(r->name) - 1] = '\0';
                                    r->base = base;
                                    r->found = true;
                                    return 1;
                                }
                            }
                            return 0;
                        }
                    } resolve{};
                    resolve.target = top[k].pc;
                    dl_iterate_phdr(PcResolve::cb, &resolve);
                    if (resolve.found) {
                        LOGI("      pc=0x%lx  count=%llu  [%s+0x%lx]",
                             (unsigned long)top[k].pc,
                             (unsigned long long)top[k].cnt,
                             resolve.name,
                             (unsigned long)(top[k].pc - resolve.base));
                    } else {
                        LOGI("      pc=0x%lx  count=%llu  [unmapped/JIT]",
                             (unsigned long)top[k].pc,
                             (unsigned long long)top[k].cnt);
                    }
                }
            }
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

// =============================================================================
// LIGHT variant — counter only, no stack walk / no arg snapshot.
// Separate slot pool so it doesn't compete with the heavy hook.
// Designed for hot paths where the heavy hook would freeze the runtime.
// =============================================================================

namespace {

struct LightHookSlot {
    std::atomic<bool>          taken{false};
    std::uintptr_t             libcore_offset = 0;
    char                       label[64] = {};
    void*                      stub = nullptr;
    std::atomic<generic_fn>    orig{nullptr};
    std::atomic<std::uint64_t> hits{0};
};

static LightHookSlot g_light_slots[kMaxHooks];

// 32 distinct light proxies — one per slot. Each is just an atomic
// counter increment + tail-call to original. NO stack walk, NO
// thread-local guard, NO arg capture. Runs in ~10 ns per call.
#define DEFINE_LIGHT_PROXY(N)                                                  \
    static long light_proxy_##N(long a0, long a1, long a2, long a3,            \
                                 long a4, long a5, long a6, long a7) {         \
        g_light_slots[N].hits.fetch_add(1, std::memory_order_relaxed);         \
        generic_fn fn = g_light_slots[N].orig.load(std::memory_order_relaxed); \
        return fn ? fn(a0,a1,a2,a3,a4,a5,a6,a7) : 0;                           \
    }

DEFINE_LIGHT_PROXY(0)  DEFINE_LIGHT_PROXY(1)  DEFINE_LIGHT_PROXY(2)  DEFINE_LIGHT_PROXY(3)
DEFINE_LIGHT_PROXY(4)  DEFINE_LIGHT_PROXY(5)  DEFINE_LIGHT_PROXY(6)  DEFINE_LIGHT_PROXY(7)
DEFINE_LIGHT_PROXY(8)  DEFINE_LIGHT_PROXY(9)  DEFINE_LIGHT_PROXY(10) DEFINE_LIGHT_PROXY(11)
DEFINE_LIGHT_PROXY(12) DEFINE_LIGHT_PROXY(13) DEFINE_LIGHT_PROXY(14) DEFINE_LIGHT_PROXY(15)
DEFINE_LIGHT_PROXY(16) DEFINE_LIGHT_PROXY(17) DEFINE_LIGHT_PROXY(18) DEFINE_LIGHT_PROXY(19)
DEFINE_LIGHT_PROXY(20) DEFINE_LIGHT_PROXY(21) DEFINE_LIGHT_PROXY(22) DEFINE_LIGHT_PROXY(23)
DEFINE_LIGHT_PROXY(24) DEFINE_LIGHT_PROXY(25) DEFINE_LIGHT_PROXY(26) DEFINE_LIGHT_PROXY(27)
DEFINE_LIGHT_PROXY(28) DEFINE_LIGHT_PROXY(29) DEFINE_LIGHT_PROXY(30) DEFINE_LIGHT_PROXY(31)
#undef DEFINE_LIGHT_PROXY

#define LPE(N) (const void*)&light_proxy_##N
static const void* kLightProxyTable[kMaxHooks] = {
    LPE(0),  LPE(1),  LPE(2),  LPE(3),  LPE(4),  LPE(5),  LPE(6),  LPE(7),
    LPE(8),  LPE(9),  LPE(10), LPE(11), LPE(12), LPE(13), LPE(14), LPE(15),
    LPE(16), LPE(17), LPE(18), LPE(19), LPE(20), LPE(21), LPE(22), LPE(23),
    LPE(24), LPE(25), LPE(26), LPE(27), LPE(28), LPE(29), LPE(30), LPE(31),
};
#undef LPE

} // namespace

int AndroidExperimentHook::lightInstall(std::uintptr_t libcore_offset, const char* label) {
    std::uintptr_t base = getLibCoreBase();
    if (base == 0) {
        LOGE("light_install[%s, off=0x%lx]: libCore.so not loaded",
             label ? label : "?", (unsigned long)libcore_offset);
        return -1;
    }

    for (std::size_t i = 0; i < kMaxHooks; i++) {
        if (g_light_slots[i].taken.load(std::memory_order_acquire) &&
            g_light_slots[i].libcore_offset == libcore_offset) {
            LOGW("light_install[%s, off=0x%lx]: already hooked in light slot %zu",
                 label ? label : "?", (unsigned long)libcore_offset, i);
            return -1;
        }
    }
    int slot_idx = -1;
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        bool expected = false;
        if (g_light_slots[i].taken.compare_exchange_strong(expected, true,
                                                            std::memory_order_acq_rel)) {
            slot_idx = static_cast<int>(i);
            break;
        }
    }
    if (slot_idx < 0) {
        LOGW("light_install: no free light slot (%zu max)", kMaxHooks);
        return -1;
    }

    LightHookSlot& slot = g_light_slots[slot_idx];
    slot.libcore_offset = libcore_offset;
    if (label) std::strncpy(slot.label, label, sizeof(slot.label) - 1);
    slot.label[sizeof(slot.label) - 1] = '\0';
    slot.hits.store(0);

    std::uintptr_t target = base + libcore_offset;
    generic_fn orig = nullptr;
    slot.stub = shadowhook_hook_func_addr(
        reinterpret_cast<void*>(target),
        const_cast<void*>(kLightProxyTable[slot_idx]),
        reinterpret_cast<void**>(&orig));
    if (slot.stub == nullptr) {
        int err = shadowhook_get_errno();
        LOGE("light_install[%s, off=0x%lx]: shadowhook failed errno=%d %s",
             label ? label : "?", (unsigned long)libcore_offset,
             err, shadowhook_to_errmsg(err));
        slot.taken.store(false, std::memory_order_release);
        return -1;
    }
    slot.orig.store(orig, std::memory_order_release);
    LOGI("light_install[%s, slot=%d]: offset=0x%lx target=0x%lx orig=%p stub=%p",
         label ? label : "?", slot_idx,
         (unsigned long)libcore_offset, (unsigned long)target,
         (void*)orig, slot.stub);
    return slot_idx;
}

long AndroidExperimentHook::lightHitsForOffset(std::uintptr_t libcore_offset) {
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        if (g_light_slots[i].taken.load(std::memory_order_acquire) &&
            g_light_slots[i].libcore_offset == libcore_offset) {
            return static_cast<long>(g_light_slots[i].hits.load(std::memory_order_relaxed));
        }
    }
    return -1;
}

void AndroidExperimentHook::lightUninstallAll() {
    LOGI("light_uninstallAll: hit counts:");
    int active = 0;
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        if (!g_light_slots[i].taken.load(std::memory_order_acquire)) continue;
        active++;
        LightHookSlot& slot = g_light_slots[i];
        std::uint64_t hits = slot.hits.load(std::memory_order_relaxed);
        LOGI("  light slot[%zu] '%s' off=0x%lx  hits=%llu",
             i, slot.label, (unsigned long)slot.libcore_offset,
             (unsigned long long)hits);
        if (slot.stub) {
            shadowhook_unhook(slot.stub);
            slot.stub = nullptr;
        }
        slot.orig.store(nullptr, std::memory_order_release);
        slot.taken.store(false, std::memory_order_release);
    }
    LOGI("light_uninstallAll: cleared %d active light hooks", active);
}

int AndroidExperimentHook::lightActiveSlots() {
    int n = 0;
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        if (g_light_slots[i].taken.load(std::memory_order_relaxed)) n++;
    }
    return n;
}

} // namespace ane::profiler
