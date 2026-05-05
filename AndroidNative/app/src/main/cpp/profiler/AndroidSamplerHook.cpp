// Phase 4a — Android sampler vftable diagnostic hook implementation.
// See header for context.
//
// Strategy: collect every UNIQUE function pointer reachable from AvmCore via
// 1-level pointer + first-8-vftable-slots, hook each with a numbered proxy
// that increments a counter. The function that fires thousands of times under
// alloc churn is recordAllocationSample.

#include "AndroidSamplerHook.hpp"

#include <android/log.h>
#include <shadowhook.h>
#include <atomic>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "AndroidGcHook.hpp"
#include "DeepProfilerController.hpp"

#define LOG_TAG "AneSamplerHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {
void* getCapturedGcSingleton();
}

namespace ane::profiler {
namespace {

constexpr std::size_t kMaxHooks = 96;  // collect up to 96 unique functions

using generic_fn = long (*)(long, long, long, long, long, long, long, long);

static std::atomic<std::uint64_t> g_diag_hits[kMaxHooks]{};
static std::atomic<generic_fn>    g_orig_fn[kMaxHooks]{};
static void*                      g_stub[kMaxHooks]{};
static std::uintptr_t             g_fn_addr[kMaxHooks]{};
static std::uintptr_t             g_origin_vftable[kMaxHooks]{};
static std::size_t                g_origin_slot[kMaxHooks]{};
static std::uintptr_t             g_origin_avmcore_off[kMaxHooks]{};
static std::atomic<bool>          g_active{false};
static thread_local bool          t_in_proxy = false;

// Per-hook proxy. Each must be a distinct C function (shadowhook needs
// unique trampoline addresses). 96 proxies via macro.
#define DEFINE_PROXY(N)                                                      \
    static long proxy_##N(long a0, long a1, long a2, long a3,                \
                           long a4, long a5, long a6, long a7) {             \
        g_diag_hits[N].fetch_add(1, std::memory_order_relaxed);              \
        if (t_in_proxy || !g_active.load(std::memory_order_relaxed)) {       \
            generic_fn fn = g_orig_fn[N].load(std::memory_order_relaxed);    \
            return fn ? fn(a0, a1, a2, a3, a4, a5, a6, a7) : 0;              \
        }                                                                    \
        t_in_proxy = true;                                                   \
        generic_fn fn = g_orig_fn[N].load(std::memory_order_relaxed);        \
        long rv = fn ? fn(a0, a1, a2, a3, a4, a5, a6, a7) : 0;               \
        t_in_proxy = false;                                                  \
        return rv;                                                           \
    }

DEFINE_PROXY(0)  DEFINE_PROXY(1)  DEFINE_PROXY(2)  DEFINE_PROXY(3)
DEFINE_PROXY(4)  DEFINE_PROXY(5)  DEFINE_PROXY(6)  DEFINE_PROXY(7)
DEFINE_PROXY(8)  DEFINE_PROXY(9)  DEFINE_PROXY(10) DEFINE_PROXY(11)
DEFINE_PROXY(12) DEFINE_PROXY(13) DEFINE_PROXY(14) DEFINE_PROXY(15)
DEFINE_PROXY(16) DEFINE_PROXY(17) DEFINE_PROXY(18) DEFINE_PROXY(19)
DEFINE_PROXY(20) DEFINE_PROXY(21) DEFINE_PROXY(22) DEFINE_PROXY(23)
DEFINE_PROXY(24) DEFINE_PROXY(25) DEFINE_PROXY(26) DEFINE_PROXY(27)
DEFINE_PROXY(28) DEFINE_PROXY(29) DEFINE_PROXY(30) DEFINE_PROXY(31)
DEFINE_PROXY(32) DEFINE_PROXY(33) DEFINE_PROXY(34) DEFINE_PROXY(35)
DEFINE_PROXY(36) DEFINE_PROXY(37) DEFINE_PROXY(38) DEFINE_PROXY(39)
DEFINE_PROXY(40) DEFINE_PROXY(41) DEFINE_PROXY(42) DEFINE_PROXY(43)
DEFINE_PROXY(44) DEFINE_PROXY(45) DEFINE_PROXY(46) DEFINE_PROXY(47)
DEFINE_PROXY(48) DEFINE_PROXY(49) DEFINE_PROXY(50) DEFINE_PROXY(51)
DEFINE_PROXY(52) DEFINE_PROXY(53) DEFINE_PROXY(54) DEFINE_PROXY(55)
DEFINE_PROXY(56) DEFINE_PROXY(57) DEFINE_PROXY(58) DEFINE_PROXY(59)
DEFINE_PROXY(60) DEFINE_PROXY(61) DEFINE_PROXY(62) DEFINE_PROXY(63)
DEFINE_PROXY(64) DEFINE_PROXY(65) DEFINE_PROXY(66) DEFINE_PROXY(67)
DEFINE_PROXY(68) DEFINE_PROXY(69) DEFINE_PROXY(70) DEFINE_PROXY(71)
DEFINE_PROXY(72) DEFINE_PROXY(73) DEFINE_PROXY(74) DEFINE_PROXY(75)
DEFINE_PROXY(76) DEFINE_PROXY(77) DEFINE_PROXY(78) DEFINE_PROXY(79)
DEFINE_PROXY(80) DEFINE_PROXY(81) DEFINE_PROXY(82) DEFINE_PROXY(83)
DEFINE_PROXY(84) DEFINE_PROXY(85) DEFINE_PROXY(86) DEFINE_PROXY(87)
DEFINE_PROXY(88) DEFINE_PROXY(89) DEFINE_PROXY(90) DEFINE_PROXY(91)
DEFINE_PROXY(92) DEFINE_PROXY(93) DEFINE_PROXY(94) DEFINE_PROXY(95)

#undef DEFINE_PROXY

#define PROXY_ENTRY(N) (const void*)&proxy_##N
static const void* kProxyTable[kMaxHooks] = {
    PROXY_ENTRY(0),  PROXY_ENTRY(1),  PROXY_ENTRY(2),  PROXY_ENTRY(3),
    PROXY_ENTRY(4),  PROXY_ENTRY(5),  PROXY_ENTRY(6),  PROXY_ENTRY(7),
    PROXY_ENTRY(8),  PROXY_ENTRY(9),  PROXY_ENTRY(10), PROXY_ENTRY(11),
    PROXY_ENTRY(12), PROXY_ENTRY(13), PROXY_ENTRY(14), PROXY_ENTRY(15),
    PROXY_ENTRY(16), PROXY_ENTRY(17), PROXY_ENTRY(18), PROXY_ENTRY(19),
    PROXY_ENTRY(20), PROXY_ENTRY(21), PROXY_ENTRY(22), PROXY_ENTRY(23),
    PROXY_ENTRY(24), PROXY_ENTRY(25), PROXY_ENTRY(26), PROXY_ENTRY(27),
    PROXY_ENTRY(28), PROXY_ENTRY(29), PROXY_ENTRY(30), PROXY_ENTRY(31),
    PROXY_ENTRY(32), PROXY_ENTRY(33), PROXY_ENTRY(34), PROXY_ENTRY(35),
    PROXY_ENTRY(36), PROXY_ENTRY(37), PROXY_ENTRY(38), PROXY_ENTRY(39),
    PROXY_ENTRY(40), PROXY_ENTRY(41), PROXY_ENTRY(42), PROXY_ENTRY(43),
    PROXY_ENTRY(44), PROXY_ENTRY(45), PROXY_ENTRY(46), PROXY_ENTRY(47),
    PROXY_ENTRY(48), PROXY_ENTRY(49), PROXY_ENTRY(50), PROXY_ENTRY(51),
    PROXY_ENTRY(52), PROXY_ENTRY(53), PROXY_ENTRY(54), PROXY_ENTRY(55),
    PROXY_ENTRY(56), PROXY_ENTRY(57), PROXY_ENTRY(58), PROXY_ENTRY(59),
    PROXY_ENTRY(60), PROXY_ENTRY(61), PROXY_ENTRY(62), PROXY_ENTRY(63),
    PROXY_ENTRY(64), PROXY_ENTRY(65), PROXY_ENTRY(66), PROXY_ENTRY(67),
    PROXY_ENTRY(68), PROXY_ENTRY(69), PROXY_ENTRY(70), PROXY_ENTRY(71),
    PROXY_ENTRY(72), PROXY_ENTRY(73), PROXY_ENTRY(74), PROXY_ENTRY(75),
    PROXY_ENTRY(76), PROXY_ENTRY(77), PROXY_ENTRY(78), PROXY_ENTRY(79),
    PROXY_ENTRY(80), PROXY_ENTRY(81), PROXY_ENTRY(82), PROXY_ENTRY(83),
    PROXY_ENTRY(84), PROXY_ENTRY(85), PROXY_ENTRY(86), PROXY_ENTRY(87),
    PROXY_ENTRY(88), PROXY_ENTRY(89), PROXY_ENTRY(90), PROXY_ENTRY(91),
    PROXY_ENTRY(92), PROXY_ENTRY(93), PROXY_ENTRY(94), PROXY_ENTRY(95),
};
#undef PROXY_ENTRY

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

// Section bounds populated lazily via /proc/self/maps for libCore.so
struct LibCoreBounds {
    std::uintptr_t base = 0;
    std::uintptr_t text_lo = 0, text_hi = 0;
    std::uintptr_t rodata_lo = 0, rodata_hi = 0;
    std::uintptr_t data_lo = 0, data_hi = 0;
    bool populated = false;
};
static LibCoreBounds g_lc{};

static void populateLibCoreBounds() {
    if (g_lc.populated) return;
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        if (!std::strstr(line, "libCore.so")) continue;
        unsigned long lo = 0, hi = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) < 3) continue;
        if (perms[0] == 'r' && perms[2] == 'x') {
            if (g_lc.text_lo == 0 || lo < g_lc.text_lo) g_lc.text_lo = lo;
            if (hi > g_lc.text_hi) g_lc.text_hi = hi;
        } else if (perms[0] == 'r' && perms[1] == '-' && perms[2] == '-') {
            if (g_lc.rodata_lo == 0 || lo < g_lc.rodata_lo) g_lc.rodata_lo = lo;
            if (hi > g_lc.rodata_hi) g_lc.rodata_hi = hi;
        } else if (perms[0] == 'r' && perms[1] == 'w') {
            if (g_lc.data_lo == 0 || lo < g_lc.data_lo) g_lc.data_lo = lo;
            if (hi > g_lc.data_hi) g_lc.data_hi = hi;
        }
        if (g_lc.base == 0 || lo < g_lc.base) g_lc.base = lo;
    }
    std::fclose(f);
    g_lc.populated = true;
}

static bool isPtrInText(std::uintptr_t p) {
    return p >= g_lc.text_lo && p < g_lc.text_hi;
}
static bool isPtrInData(std::uintptr_t p) {
    return (p >= g_lc.data_lo && p < g_lc.data_hi) ||
           (p >= g_lc.rodata_lo && p < g_lc.rodata_hi);
}

// Walk AvmCore (gc+0x10) for sub-objects. For each pointer field that
// looks like a C++ object (vftable head in .data, slot[0] in .text),
// collect the function pointers in slots [0..7] of that vftable. Return
// list of unique (fn_addr, vftable, slot, avmcore_off) tuples.
static std::size_t collectSamplerCandidates(std::uintptr_t avmcore) {
    std::size_t hook_count = 0;
    auto already_added = [&hook_count](std::uintptr_t fn) -> bool {
        for (std::size_t i = 0; i < hook_count; i++) {
            if (g_fn_addr[i] == fn) return true;
        }
        return false;
    };
    constexpr std::size_t kAvmcoreScan = 0x100;
    for (std::size_t off = 0; off < kAvmcoreScan; off += 8) {
        std::uintptr_t obj = 0;
        if (!safeReadPtr(avmcore + off, obj)) continue;
        if (obj == 0 || obj < 0x1000) continue;
        std::uintptr_t vftable = 0;
        if (!safeReadPtr(obj, vftable)) continue;
        if (!isPtrInData(vftable)) continue;
        // Validate slot[0] is in .text (real vftable signature)
        std::uintptr_t s0 = 0;
        if (!safeReadPtr(vftable, s0)) continue;
        if (!isPtrInText(s0)) continue;

        // Collect first 8 slots of this vftable
        for (std::size_t slot = 0; slot < 8; slot++) {
            std::uintptr_t fn = 0;
            if (!safeReadPtr(vftable + slot * 8, fn)) continue;
            if (fn == 0 || !isPtrInText(fn)) continue;
            if (already_added(fn)) continue;
            if (hook_count >= kMaxHooks) {
                LOGW("collectSamplerCandidates: hit kMaxHooks=%zu cap", kMaxHooks);
                return hook_count;
            }
            g_fn_addr[hook_count] = fn;
            g_origin_vftable[hook_count] = vftable;
            g_origin_slot[hook_count] = slot;
            g_origin_avmcore_off[hook_count] = off;
            hook_count++;
        }
    }
    return hook_count;
}

} // namespace

AndroidSamplerHook::~AndroidSamplerHook() {
    uninstall();
}

bool AndroidSamplerHook::install(DeepProfilerController* /*controller*/) {
    if (installed_) return true;

    populateLibCoreBounds();
    LOGI("install: libCore base=0x%lx text=0x%lx-0x%lx rodata=0x%lx-0x%lx data=0x%lx-0x%lx",
         (unsigned long)g_lc.base,
         (unsigned long)g_lc.text_lo,   (unsigned long)g_lc.text_hi,
         (unsigned long)g_lc.rodata_lo, (unsigned long)g_lc.rodata_hi,
         (unsigned long)g_lc.data_lo,   (unsigned long)g_lc.data_hi);

    void* gc = getCapturedGcSingleton();
    if (gc == nullptr) {
        LOGW("install: no GC singleton — call after at least one Collect");
        return false;
    }
    std::uintptr_t avmcore = 0;
    if (!safeReadPtr(reinterpret_cast<std::uintptr_t>(gc) + 0x10, avmcore)) {
        LOGE("install: gc+0x10 not readable");
        return false;
    }
    sampler_obj_ = avmcore;
    sampler_vftable_ = 0;  // not a single vftable in multi-candidate mode

    // Reset state
    for (auto& c : g_diag_hits) c.store(0, std::memory_order_relaxed);
    for (auto& s : g_stub) s = nullptr;
    for (auto& o : g_orig_fn) o.store(nullptr, std::memory_order_relaxed);
    std::memset(g_fn_addr, 0, sizeof(g_fn_addr));
    std::memset(g_origin_vftable, 0, sizeof(g_origin_vftable));
    std::memset(g_origin_slot, 0, sizeof(g_origin_slot));
    std::memset(g_origin_avmcore_off, 0, sizeof(g_origin_avmcore_off));

    std::size_t cand = collectSamplerCandidates(avmcore);
    LOGI("install: %zu unique candidate functions to hook from AvmCore=0x%lx",
         cand, (unsigned long)avmcore);

    hooked_count_ = 0;
    for (std::size_t i = 0; i < cand; i++) {
        generic_fn orig = nullptr;
        void* stub = shadowhook_hook_func_addr(
            reinterpret_cast<void*>(g_fn_addr[i]),
            const_cast<void*>(kProxyTable[i]),
            reinterpret_cast<void**>(&orig));
        if (stub == nullptr) {
            int err = shadowhook_get_errno();
            LOGW("install: hook[%zu] failed at fn=0x%lx (vt=0x%lx slot=%zu off=0x%lx) errno=%d %s",
                 i, (unsigned long)g_fn_addr[i],
                 (unsigned long)g_origin_vftable[i], g_origin_slot[i],
                 (unsigned long)g_origin_avmcore_off[i],
                 err, shadowhook_to_errmsg(err));
            continue;
        }
        g_stub[i] = stub;
        g_orig_fn[i].store(orig, std::memory_order_release);
        hooked_count_++;
    }

    if (hooked_count_ == 0) {
        LOGE("install: zero candidates hooked");
        return false;
    }

    g_active.store(true, std::memory_order_release);
    installed_ = true;
    LOGI("install: %zu/%zu candidates hooked", hooked_count_, cand);
    return true;
}

void AndroidSamplerHook::uninstall() {
    if (!installed_) return;
    g_active.store(false, std::memory_order_release);
    LOGI("uninstall: hits per hooked function (only non-zero):");
    // Sort-ish: log only entries with hits > 0, in decreasing-hit order
    struct Entry {
        std::size_t i;
        std::uint64_t hits;
    };
    Entry entries[kMaxHooks];
    std::size_t n = 0;
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        std::uint64_t h = g_diag_hits[i].load(std::memory_order_relaxed);
        if (h > 0) entries[n++] = {i, h};
    }
    // simple insertion sort (n is tiny)
    for (std::size_t a = 1; a < n; a++) {
        Entry e = entries[a];
        std::size_t b = a;
        while (b > 0 && entries[b-1].hits < e.hits) {
            entries[b] = entries[b-1];
            b--;
        }
        entries[b] = e;
    }
    for (std::size_t k = 0; k < n; k++) {
        std::size_t i = entries[k].i;
        LOGI("  hook[%zu] hits=%llu fn=0x%lx vt=0x%lx slot=%zu avmcore_off=0x%lx",
             i, (unsigned long long)entries[k].hits,
             (unsigned long)g_fn_addr[i],
             (unsigned long)g_origin_vftable[i],
             g_origin_slot[i],
             (unsigned long)g_origin_avmcore_off[i]);
    }
    LOGI("uninstall: %zu hooks fired (of %zu installed)", n, hooked_count_);
    // Tear down all hooks
    for (std::size_t i = 0; i < kMaxHooks; i++) {
        if (g_stub[i] != nullptr) {
            shadowhook_unhook(g_stub[i]);
            g_stub[i] = nullptr;
            g_orig_fn[i].store(nullptr, std::memory_order_relaxed);
        }
    }
    hooked_count_ = 0;
    installed_ = false;
}

std::uint64_t AndroidSamplerHook::slotHits(std::size_t i) const {
    if (i >= kMaxHooks) return 0;
    return g_diag_hits[i].load(std::memory_order_relaxed);
}

} // namespace ane::profiler
