// Phase 7a — Android GC observer hook implementation. See header.

#include "AndroidGcHook.hpp"

#include <android/log.h>
#include <shadowhook.h>
#include <link.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

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

// Captured GC singleton — set on first observed Collect() call. Subsequent
// programmatic requestCollect() invocations call original Collect with this.
static std::atomic<void*> g_captured_gc_this{nullptr};

// Sampler RE: dump the GC struct on first capture so we can identify
// the m_sampler / m_core / GCCallback fields by inspection. Once-only.
static std::atomic<bool> g_struct_dumped{false};

// Per-section bounds of libCore.so, populated at install. Used by the
// pointer classifier so the dump labels each ptr as text/rodata/data/heap.
struct LibcoreSections {
    std::uintptr_t base = 0;
    std::uintptr_t text_lo = 0, text_hi = 0;
    std::uintptr_t rodata_lo = 0, rodata_hi = 0;
    std::uintptr_t data_lo = 0, data_hi = 0;
    std::uintptr_t end = 0;
};
static LibcoreSections g_libcore{};

static const char* classifyPtr(std::uintptr_t p) {
    if (p == 0) return "null";
    if (p & 7) return "unaligned";
    if (p < 0x1000) return "small";
    if (p > 0x0000007FFFFFFFFFULL) return "huge";
    if (g_libcore.base != 0) {
        if (p >= g_libcore.text_lo   && p < g_libcore.text_hi)   return "text";
        if (p >= g_libcore.rodata_lo && p < g_libcore.rodata_hi) return "rodata";
        if (p >= g_libcore.data_lo   && p < g_libcore.data_hi)   return "data";
        if (p >= g_libcore.base      && p < g_libcore.end)       return "lib";
    }
    return "heap";
}

// Page-checked deref: returns true and *out=ptr_at_addr on success,
// false if the page containing addr isn't mapped.
static bool safeReadPtr(std::uintptr_t addr, std::uintptr_t& out) {
    if (addr < 0x1000 || (addr & 7)) return false;
    // mincore check
    static long page_size_local = sysconf(_SC_PAGESIZE);
    std::uintptr_t page = addr & ~(static_cast<std::uintptr_t>(page_size_local) - 1);
    unsigned char vec = 0;
    if (mincore(reinterpret_cast<void*>(page), page_size_local, &vec) != 0) return false;
    if (!(vec & 1)) return false;
    out = *reinterpret_cast<volatile std::uintptr_t*>(addr);
    return true;
}

// Forward decl
static void dumpObjectStruct(void* obj, const char* label, std::size_t scan_bytes,
                             int recursion_left);

// Dump GC struct from gc_this (4KB worth) and identify pointer fields,
// classifying each as text/rodata/data/heap. For each `heap`-class pointer,
// also try to deref it once and dump first 4 slots of the pointed-to
// object (which may be a vftable head).
static void dumpGcStruct(void* gc_this) {
    if (gc_this == nullptr) return;
    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(gc_this);
    LOGI("dumpGcStruct: gc_this=%p (libCore base=0x%lx, text=0x%lx-0x%lx, "
         "rodata=0x%lx-0x%lx, data=0x%lx-0x%lx)",
         gc_this,
         (unsigned long)g_libcore.base,
         (unsigned long)g_libcore.text_lo,   (unsigned long)g_libcore.text_hi,
         (unsigned long)g_libcore.rodata_lo, (unsigned long)g_libcore.rodata_hi,
         (unsigned long)g_libcore.data_lo,   (unsigned long)g_libcore.data_hi);

    constexpr std::size_t kDumpBytes = 0x300;  // 768 bytes — enough for top-level GC fields
    for (std::size_t off = 0; off < kDumpBytes; off += 8) {
        std::uintptr_t v = 0;
        if (!safeReadPtr(base + off, v)) continue;
        const char* cls = classifyPtr(v);
        if (std::strcmp(cls, "null") == 0) continue;
        if (std::strcmp(cls, "small") == 0) continue;  // small int — not a ptr
        if (std::strcmp(cls, "unaligned") == 0) continue;
        if (std::strcmp(cls, "huge") == 0) continue;

        // We want pointers into mapped regions (text/rodata/data/lib/heap)
        LOGI("  this+0x%04zx = 0x%016lx [%s]", off, (unsigned long)v, cls);

        // If it's heap or lib (object pointer), try to read first 4 slots
        if (std::strcmp(cls, "heap") == 0 ||
            std::strcmp(cls, "lib")  == 0 ||
            std::strcmp(cls, "data") == 0) {
            std::uintptr_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            bool g0 = safeReadPtr(v + 0,  s0);
            bool g1 = safeReadPtr(v + 8,  s1);
            bool g2 = safeReadPtr(v + 16, s2);
            bool g3 = safeReadPtr(v + 24, s3);
            // Vftable signature: first slot is a pointer to .text
            if (g0) {
                const char* c0 = classifyPtr(s0);
                if (std::strcmp(c0, "text") == 0) {
                    // *(*p) lands in text → p points to a vftable
                    LOGI("    (vftable*) [+0]=0x%lx[%s] [+8]=0x%lx[%s] "
                         "[+16]=0x%lx[%s] [+24]=0x%lx[%s]",
                         (unsigned long)s0, c0,
                         (unsigned long)s1, g1 ? classifyPtr(s1) : "?",
                         (unsigned long)s2, g2 ? classifyPtr(s2) : "?",
                         (unsigned long)s3, g3 ? classifyPtr(s3) : "?");
                } else if (std::strcmp(c0, "data") == 0 || std::strcmp(c0, "rodata") == 0) {
                    // p might be an object whose first field IS a vftable*
                    // dereference once more to validate
                    std::uintptr_t vt0 = 0, vt1 = 0;
                    if (safeReadPtr(s0 + 0, vt0) && safeReadPtr(s0 + 8, vt1)) {
                        const char* cv0 = classifyPtr(vt0);
                        const char* cv1 = classifyPtr(vt1);
                        if (std::strcmp(cv0, "text") == 0 &&
                            std::strcmp(cv1, "text") == 0) {
                            LOGI("    (object*->vftable) vt=0x%lx[%s] "
                                 "slot0=0x%lx[%s] slot1=0x%lx[%s]",
                                 (unsigned long)s0, c0,
                                 (unsigned long)vt0, cv0,
                                 (unsigned long)vt1, cv1);
                        }
                    }
                }
            }
        }
    }
    LOGI("dumpGcStruct: done");
    // Note: caller can now use dumpAvmCore("LABEL") to recurse into the
    // AvmCore struct on-demand — used by Phase 4a sampler RA to diff
    // pre/post snapshots around flash.sampler.startSampling().
}

// Same as dumpGcStruct but generic — dumps an arbitrary object's pointer
// fields. Used to recurse from GC* into AvmCore* (which holds m_sampler).
static void dumpObjectStruct(void* obj, const char* label,
                             std::size_t scan_bytes, int recursion_left) {
    if (obj == nullptr) return;
    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(obj);
    LOGI("dumpObjectStruct[%s]: obj=%p, scan_bytes=%zu", label, obj, scan_bytes);

    for (std::size_t off = 0; off < scan_bytes; off += 8) {
        std::uintptr_t v = 0;
        if (!safeReadPtr(base + off, v)) continue;
        const char* cls = classifyPtr(v);
        if (std::strcmp(cls, "null") == 0) continue;
        if (std::strcmp(cls, "small") == 0) continue;
        if (std::strcmp(cls, "unaligned") == 0) continue;
        if (std::strcmp(cls, "huge") == 0) continue;

        LOGI("  [%s]+0x%04zx = 0x%016lx [%s]",
             label, off, (unsigned long)v, cls);

        // Same vftable detection as dumpGcStruct
        if (std::strcmp(cls, "heap") == 0 ||
            std::strcmp(cls, "lib")  == 0 ||
            std::strcmp(cls, "data") == 0) {
            std::uintptr_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            bool g0 = safeReadPtr(v + 0,  s0);
            bool g1 = safeReadPtr(v + 8,  s1);
            bool g2 = safeReadPtr(v + 16, s2);
            bool g3 = safeReadPtr(v + 24, s3);
            if (g0) {
                const char* c0 = classifyPtr(s0);
                if (std::strcmp(c0, "text") == 0) {
                    LOGI("    (vftable*) [+0]=0x%lx[%s] [+8]=0x%lx[%s] "
                         "[+16]=0x%lx[%s] [+24]=0x%lx[%s]",
                         (unsigned long)s0, c0,
                         (unsigned long)s1, g1 ? classifyPtr(s1) : "?",
                         (unsigned long)s2, g2 ? classifyPtr(s2) : "?",
                         (unsigned long)s3, g3 ? classifyPtr(s3) : "?");
                } else if (std::strcmp(c0, "data") == 0 ||
                           std::strcmp(c0, "rodata") == 0) {
                    std::uintptr_t vt0 = 0, vt1 = 0;
                    if (safeReadPtr(s0 + 0, vt0) && safeReadPtr(s0 + 8, vt1)) {
                        const char* cv0 = classifyPtr(vt0);
                        const char* cv1 = classifyPtr(vt1);
                        if (std::strcmp(cv0, "text") == 0 &&
                            std::strcmp(cv1, "text") == 0) {
                            LOGI("    (object*->vftable) vt=0x%lx[%s] "
                                 "slot0=0x%lx[%s] slot1=0x%lx[%s]",
                                 (unsigned long)s0, c0,
                                 (unsigned long)vt0, cv0,
                                 (unsigned long)vt1, cv1);
                            // SAMPLER candidate detected.
                            // Dump 16 vtable slots so we can identify the
                            // class by its method count / shape.
                            if (recursion_left > 0) {
                                LOGI("    [vtable@0x%lx full slots]:",
                                     (unsigned long)s0);
                                for (int slot = 0; slot < 16; slot++) {
                                    std::uintptr_t sv = 0;
                                    if (safeReadPtr(s0 + slot * 8, sv)) {
                                        const char* sc = classifyPtr(sv);
                                        LOGI("      [+0x%02x] = 0x%lx [%s]",
                                             slot * 8, (unsigned long)sv, sc);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    LOGI("dumpObjectStruct[%s]: done", label);
}

// Populate g_libcore section bounds from /proc/self/maps for the
// running libCore.so. Called once at install — bounds drive the ptr
// classifier in dumpGcStruct.
static void populateLibcoreSections(std::uintptr_t libcore_base) {
    g_libcore.base = libcore_base;
    // Read /proc/self/maps and extract all ranges that belong to libCore.so
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        // Match libCore.so in the path
        if (!std::strstr(line, "libCore.so")) continue;
        unsigned long lo = 0, hi = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) < 3) continue;
        // r-xp = text, r--p = rodata/data.rel.ro, rw-p = data, ---p = bss-ish
        if (perms[0] == 'r' && perms[2] == 'x') {
            if (g_libcore.text_lo == 0 || lo < g_libcore.text_lo) g_libcore.text_lo = lo;
            if (hi > g_libcore.text_hi) g_libcore.text_hi = hi;
        } else if (perms[0] == 'r' && perms[1] == '-' && perms[2] == '-') {
            if (g_libcore.rodata_lo == 0 || lo < g_libcore.rodata_lo) g_libcore.rodata_lo = lo;
            if (hi > g_libcore.rodata_hi) g_libcore.rodata_hi = hi;
        } else if (perms[0] == 'r' && perms[1] == 'w') {
            if (g_libcore.data_lo == 0 || lo < g_libcore.data_lo) g_libcore.data_lo = lo;
            if (hi > g_libcore.data_hi) g_libcore.data_hi = hi;
        }
        if (hi > g_libcore.end) g_libcore.end = hi;
    }
    std::fclose(f);
}

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

    // Capture singleton on first sighting. Subsequent calls overwrite (idempotent
    // if engine has only one GC instance, which avmplus does — single per
    // AvmCore). If overwrite changes the value, log it (may indicate per-iso GC).
    void* prev = g_captured_gc_this.exchange(gc_this, std::memory_order_acq_rel);
    if (prev == nullptr) {
        LOGI("captured GC singleton: this=%p (programmatic requestCollect now armed)", gc_this);
        // RE one-shot: dump struct on first capture so we can find m_sampler
        // / m_core / GCCallback by inspection. Removed once vftable located.
        bool expected = false;
        if (g_struct_dumped.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel)) {
            dumpGcStruct(gc_this);
        }
    } else if (prev != gc_this) {
        LOGW("GC singleton changed: %p -> %p (multi-GC?)", prev, gc_this);
    }

    DeepProfilerController* dpc = g_controller.load(std::memory_order_acquire);
    DeepProfilerController::Status before{};
    if (dpc != nullptr) before = dpc->status();

    const std::uint64_t t0 = nowNs();
    g_orig_collect(gc_this);
    const std::uint64_t t1 = nowNs();
    (void)t1;

    if (dpc != nullptr) {
        // Mirror Windows ProfilerAneBindings::profiler_request_gc: read
        // before/after live counters from DPC's own allocation tracking and
        // emit them on the GcCycle event. The values are bounded by Phase 5's
        // alloc-side tracking (free-side hook still missing — task #11) so
        // `before` will trend higher than reality; that's the same approximation
        // Windows uses. gc_id = start timestamp (cheap monotonic id).
        const DeepProfilerController::Status after = dpc->status();
        dpc->record_gc_cycle(
            /* gc_id            */ t0,
            /* kind             */ aneprof::GcCycleKind::NativeObserved,
            /* before_live_count*/ before.live_allocations,
            /* before_live_bytes*/ before.live_bytes,
            /* after_live_count */ after.live_allocations,
            /* after_live_bytes */ after.live_bytes,
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
    // Controller may be null for warmup-only install: shadowhook + singleton
    // capture without DPC event recording. Caller can later upgrade via
    // setController(). Proxy guards `if (dpc != nullptr)` so null is safe.
    if (installed_) {
        // Already installed — just upgrade the controller if requested.
        if (controller != nullptr) {
            g_controller.store(controller, std::memory_order_release);
        }
        return true;
    }

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

    // Populate libCore.so section bounds for the dump's pointer classifier
    populateLibcoreSections(info.base);
    g_struct_dumped.store(false, std::memory_order_release);

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

bool AndroidGcHook::dumpAvmCore(const char* label) {
    void* gc_this = g_captured_gc_this.load(std::memory_order_acquire);
    if (gc_this == nullptr) {
        LOGW("dumpAvmCore[%s]: no GC singleton captured yet — requires "
             "at least one Collect cycle since hook install", label ? label : "?");
        return false;
    }
    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(gc_this);
    std::uintptr_t avmcore = 0;
    if (!safeReadPtr(base + 0x10, avmcore) || avmcore == 0) {
        LOGW("dumpAvmCore[%s]: AvmCore* (gc+0x10) not readable", label ? label : "?");
        return false;
    }
    LOGI("dumpAvmCore[%s]: gc=%p AvmCore=0x%lx", label ? label : "?",
         gc_this, (unsigned long)avmcore);
    // Dump AvmCore itself with all its scalar/pointer fields. Logging
    // EVERY 8-byte word (not just pointers) so the diff catches counters
    // (sample buffer size, etc.) that grow monotonically when the sampler
    // is recording.
    constexpr std::size_t kAvmcoreScan = 0x2000;  // 8KB
    LOGI("dumpAvmCore[%s] FULL scan begin (%zu bytes)", label ? label : "?",
         (size_t)kAvmcoreScan);
    char tag[64];
    std::snprintf(tag, sizeof(tag), "AvmCore[%s]", label ? label : "?");
    // Log EVERY word — not just non-zero pointers. Diff tool will compare.
    for (std::size_t off = 0; off < kAvmcoreScan; off += 8) {
        std::uintptr_t v = 0;
        if (!safeReadPtr(avmcore + off, v)) continue;
        const char* cls = classifyPtr(v);
        // Skip pure null and small noise
        if (v == 0) continue;
        LOGI("  [%s]+0x%04zx = 0x%016lx [%s]", tag, off, (unsigned long)v, cls);
    }
    LOGI("dumpAvmCore[%s] FULL scan end", label ? label : "?");
    return true;
}

bool AndroidGcHook::requestCollect() {
    if (!g_active.load(std::memory_order_acquire)) return false;
    if (g_orig_collect == nullptr) return false;
    void* gc_this = g_captured_gc_this.load(std::memory_order_acquire);
    if (gc_this == nullptr) {
        LOGW("requestCollect: GC singleton not captured yet — call requires "
             "at least one observed Collect cycle first");
        return false;
    }
    LOGI("requestCollect: invoking GC::Collect(this=%p)", gc_this);
    // Re-enter through proxy so the cycle is recorded as a GcCycleEvent
    // (tagged Programmatic via flag once we extend kind enum). For now
    // the observer treats it identically to a runtime-triggered cycle.
    proxy_GCCollect(gc_this);
    return true;
}

// Global C accessor — used by AndroidSamplerHook to read the GC singleton
// without a tight cross-module dependency. Defined inside ane::profiler so
// it can reach the anonymous-namespace globals.
extern "C" void* getCapturedGcSingleton() {
    return g_captured_gc_this.load(std::memory_order_acquire);
}

} // namespace ane::profiler
