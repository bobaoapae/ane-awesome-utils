// Phase 4c — Android typed AS3 alloc capture (Path B per PROGRESS.md).
//
// This file implements the lazy-read pattern: the production hook on
// MMgc::FixedMalloc::Alloc (already installed by AndroidDeepMemoryHook in
// Phase 5) tags each allocation with a sequence number; the writer-thread
// drain phase reads the now-constructed object's VTable→Traits→name and
// emits a typed As3Alloc event alongside the existing untyped Alloc.
//
// This file currently provides ONLY the layout walker primitives. Wireup to
// the FixedMalloc hook deferred to next iteration. Validation via
// installProbe() against a sentinel object also deferred — first commit
// the introspection helpers in isolation so they can be unit-tested.

#include "AndroidAs3ObjectHook.hpp"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

#include "DeepProfilerController.hpp"
#include "AneprofFormat.hpp"

#define LOG_TAG "AneAs3ObjectHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ane::profiler {
namespace {

static std::atomic<DeepProfilerController*> g_controller{nullptr};
static std::atomic<bool>                    g_active{false};
static std::atomic<std::uint64_t>           g_diag_allocs_observed{0};
static std::atomic<std::uint64_t>           g_diag_names_resolved{0};
static std::atomic<std::uint64_t>           g_diag_names_unresolved{0};
static std::atomic<std::uint64_t>           g_diag_allocs_queued{0};
static std::atomic<std::uint64_t>           g_diag_allocs_dropped{0};

// Per-step failure counters — pinpoint which link in the
// ScriptObject→VTable→Traits→Stringp chain is breaking the resolve.
// Bumped inside resolveClassName before every `return false`.
static std::atomic<std::uint64_t>           g_diag_fail_obj_invalid{0};
static std::atomic<std::uint64_t>           g_diag_fail_vtable_invalid{0};
static std::atomic<std::uint64_t>           g_diag_fail_traits_invalid{0};
static std::atomic<std::uint64_t>           g_diag_fail_name_invalid{0};
static std::atomic<std::uint64_t>           g_diag_fail_string_buf{0};
static std::atomic<std::uint64_t>           g_diag_fail_string_len{0};

// Hex dump of the first N unresolved allocs — recorded at drain time, dumped
// at uninstall. Lets us reverse-engineer the actual VTable offset post-hoc
// without static analysis: pick a known-typed alloc (e.g. by size) from the
// dump, walk the bytes, and identify which dword is a vtable pointer.
static constexpr std::size_t kProbeDumpCount = 12;
static constexpr std::size_t kProbeDumpBytes = 64;
struct ProbeDump {
    std::uint64_t  ptr;
    std::uint64_t  size;
    std::uint8_t   bytes[kProbeDumpBytes];
};
static std::atomic<std::uint32_t>           g_probe_count{0};
static ProbeDump                            g_probe_dumps[kProbeDumpCount];

// Reusable layout instance. Populated from defaults and validated by probe.
static AndroidAs3ObjectHook::LayoutOffsets g_layout{};

// ---------------------------------------------------------------------------
// Ring buffer: pending allocs awaiting class-name resolution. Pushed from
// proxy_FixedAlloc (multi-producer), drained by a single worker thread
// (single-consumer). Lock-free via atomic counters; full queue drops events
// rather than blocking the alloc hot path.

static constexpr std::size_t kRingCapacity = 4096;  // power of 2 for masking

struct PendingAlloc {
    void*           ptr;
    std::uint64_t   size;
    std::uint64_t   timestamp_ns;
    std::uint32_t   thread_id;
};

static PendingAlloc          g_ring[kRingCapacity];
static std::atomic<std::uint64_t> g_ring_write{0};   // monotonic write counter
static std::atomic<std::uint64_t> g_ring_read{0};    // monotonic read counter

static std::thread           g_drain_thread;
static std::atomic<bool>     g_drain_stop{false};
static std::mutex            g_drain_mu;
static std::condition_variable g_drain_cv;

static inline std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
}

// ---------------------------------------------------------------------------
// Pointer validation — given a candidate pointer, decide if it points into
// the GC heap region. Conservative checks reject most non-heap pointers.
// Pointer width is sizeof(uintptr_t) — 8 on AArch64, 4 on ARMv7.

static inline bool isPlausibleHeapPtr(std::uintptr_t p) {
    if (p == 0) return false;
    if (p & (sizeof(void*) - 1)) return false;  // pointer-aligned
#if defined(__aarch64__)
    if (p < 0x1000) return false;
    if (p > 0x0000007FFFFFFFFFULL) return false;  // 39-bit userspace
#elif defined(__arm__)
    if (p < 0x1000) return false;
    if (p > 0xBFFFFFFFu) return false;            // ARMv7 Linux user 3GB ceiling
#endif
    return true;
}

// ---------------------------------------------------------------------------
// SIGSEGV-safe pointer chase. Defends the walker against unmapped pages
// encountered while dereferencing arbitrary heap pointers (most allocations
// entering the queue are NOT AS3 ScriptObjects; reading at canonical
// ScriptObject offsets lands in random data or unmapped regions).
//
// Two layers:
//   1. mincore() pre-check on the page containing `addr` — cheap syscall,
//      catches the unmapped case 100% of the time (returns -ENOMEM). This
//      is the primary line of defense.
//   2. siglongjmp safety net: the walker arms a per-thread sigsetjmp before
//      each guarded read; CrashSignalHandler.cpp's signalHandler calls our
//      As3WalkerGuard_maybeLongjmp() weak-link hook BEFORE its own crash
//      capture path, so SIGSEGV/SIGBUS inside the guard window unwind back
//      to the read site instead of aborting the process. Same pattern as
//      AirIMEGuard.
//
// We deliberately do NOT install our own sigaction — fighting over signal
// handler chain with CrashSignalHandler races on whoever was installed last.

static thread_local sigjmp_buf t_safe_read_jmp;
static thread_local bool       t_safe_read_active = false;
static long                    g_page_size = 4096;  // initialized at install

extern "C" bool As3WalkerGuard_maybeLongjmp(int sig, siginfo_t* info, void* ctx) {
    (void)info; (void)ctx;
    if (sig != SIGSEGV && sig != SIGBUS) return false;
    if (!t_safe_read_active) return false;
    t_safe_read_active = false;
    siglongjmp(t_safe_read_jmp, 1);
    // unreachable
    return true;
}

// Defined in CrashSignalHandler.cpp. Idempotent. We call it from install() so
// the As3WalkerGuard_maybeLongjmp weak hook actually gets reached when SIGSEGV
// fires inside the walker — without this, libc's default handler aborts the
// process before any of our chain runs.
extern "C" void AneAwesomeUtils_installSignalHandlerIfNeeded();

static void ensureSegvHandlerInstalled() {
    g_page_size = sysconf(_SC_PAGESIZE);
    if (g_page_size <= 0) g_page_size = 4096;
    AneAwesomeUtils_installSignalHandlerIfNeeded();
}

// Returns true if the page containing addr is currently mapped + resident.
// mincore returns -ENOMEM (i.e., negative) if any page in the range isn't
// mapped at all; that's the dominant failure mode we're guarding against.
// Resident-but-not-cached pages still count as mapped (mincore returns 0 with
// vec[0]=0 — that's still "page exists" which is what we want).
static inline bool isPageMapped(std::uintptr_t addr) {
    std::uintptr_t page_base = addr & ~(static_cast<std::uintptr_t>(g_page_size) - 1);
    unsigned char vec = 0;
    int rc = mincore(reinterpret_cast<void*>(page_base),
                     static_cast<std::size_t>(g_page_size), &vec);
    return rc == 0;
}

// SIGSEGV/SIGBUS-guarded read. Returns 0 on fault. Caller must have already
// validated alignment and range via isPlausibleHeapPtr().
template <typename T>
static inline T safeRead(std::uintptr_t addr) {
    if (!isPageMapped(addr)) return T{};
    T val = T{};
    t_safe_read_active = true;
    if (sigsetjmp(t_safe_read_jmp, 1) == 0) {
        val = *reinterpret_cast<const volatile T*>(addr);
    } else {
        val = T{};
    }
    t_safe_read_active = false;
    return val;
}

static inline std::uintptr_t readPtr(std::uintptr_t addr) {
    if (!isPlausibleHeapPtr(addr)) return 0;
    return safeRead<std::uintptr_t>(addr);
}

static inline std::uint32_t readU32(std::uintptr_t addr) {
    if (addr == 0 || (addr & 0x3) || addr < 0x1000) return 0;
    return safeRead<std::uint32_t>(addr);
}

static inline std::int32_t readI32(std::uintptr_t addr) {
    return static_cast<std::int32_t>(readU32(addr));
}

static inline std::uint8_t readU8(std::uintptr_t addr) {
    if (addr == 0 || addr < 0x1000) return 0;
    return safeRead<std::uint8_t>(addr);
}

// ---------------------------------------------------------------------------
// Walk: ScriptObject → VTable → Traits → Stringp _name → UTF-8 chars.
//
// Returns the resolved class name in `out` (max bytes-1 chars, NUL-terminated)
// on success, or false if any step fails validation.

bool resolveClassName(std::uintptr_t obj_ptr, char* out, std::size_t out_capacity) {
    if (out_capacity < 2) return false;
    out[0] = '\0';

    if (!isPlausibleHeapPtr(obj_ptr)) {
        g_diag_fail_obj_invalid.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Step 1: read AVM2 VTable* from ScriptObject + scriptobject_vtable_off
    std::uintptr_t vtable = readPtr(obj_ptr + g_layout.scriptobject_vtable_off);
    if (!isPlausibleHeapPtr(vtable)) {
        g_diag_fail_vtable_invalid.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Step 2: read Traits* from VTable + vtable_traits_off
    std::uintptr_t traits = readPtr(vtable + g_layout.vtable_traits_off);
    if (!isPlausibleHeapPtr(traits)) {
        g_diag_fail_traits_invalid.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Step 3: read Stringp _name from Traits + traits_name_off
    std::uintptr_t name_str = readPtr(traits + g_layout.traits_name_off);
    if (!isPlausibleHeapPtr(name_str)) {
        g_diag_fail_name_invalid.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Step 4: read String fields. Pointer-width-aware via the layout struct.
    std::uintptr_t buf  = readPtr(name_str + g_layout.string_buffer_off);
    std::int32_t   len  = readI32(name_str + g_layout.string_length_off);
    std::uint32_t  bits = readU32(name_str + g_layout.string_flags_off);

    if (!isPlausibleHeapPtr(buf)) {
        g_diag_fail_string_buf.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (len <= 0 || len > 256) {
        g_diag_fail_string_len.fetch_add(1, std::memory_order_relaxed);
        return false;  // class names are always reasonable
    }

    bool is_k16 = (bits & AndroidAs3ObjectHook::kStringWidthMask) != 0;
    bool is_7bit_ascii = (bits & AndroidAs3ObjectHook::k7BitAsciiFlag) != 0;

    // Bytes to copy. For class names, k16 strings should be rare (class names
    // are ASCII). If we see k16 + ASCII, treat each pair of bytes as a wide
    // char and pull the low byte. If k16 + non-ASCII, give up (would need
    // proper UTF-16→UTF-8 conversion which we skip for now).
    std::size_t copy_bytes = static_cast<std::size_t>(len);
    if (is_k16 && !is_7bit_ascii) {
        // skip non-ascii UTF-16 strings — class names don't appear here in
        // practice, so this is just a safety net.
        return false;
    }

    if (copy_bytes >= out_capacity) copy_bytes = out_capacity - 1;

    if (is_k16) {
        // Read 2 bytes per char, low byte only (since 7bit ASCII => high byte 0)
        for (std::size_t i = 0; i < copy_bytes; ++i) {
            std::uint8_t lo = readU8(buf + i * 2);
            if (lo < 0x20 || lo > 0x7e) {
                out[i] = '\0';
                return i > 0;  // partial success
            }
            out[i] = static_cast<char>(lo);
        }
        out[copy_bytes] = '\0';
    } else {
        for (std::size_t i = 0; i < copy_bytes; ++i) {
            std::uint8_t c = readU8(buf + i);
            if (c < 0x20 || c > 0x7e) {
                out[i] = '\0';
                return i > 0;
            }
            out[i] = static_cast<char>(c);
        }
        out[copy_bytes] = '\0';
    }

    return true;
}

// ---------------------------------------------------------------------------
// Drain thread: runs in its own thread, sleeps in 1ms increments. Each wake,
// drains entries that are >= kDeferDelayNs old, calls resolveClassName(), and
// emits As3Alloc into the DPC stream.

static void drainThreadMain() {
    char name_buf[128];
    while (!g_drain_stop.load(std::memory_order_acquire)) {
        std::uint64_t r = g_ring_read.load(std::memory_order_acquire);
        std::uint64_t w = g_ring_write.load(std::memory_order_acquire);

        const std::uint64_t now = nowNs();
        bool processed_any = false;

        while (r < w) {
            const PendingAlloc& entry = g_ring[r & (kRingCapacity - 1)];
            // If entry is too recent, wait for the constructor to finish.
            if (now - entry.timestamp_ns < AndroidAs3ObjectHook::kDeferDelayNs) {
                break;  // sleep, retry
            }

            DeepProfilerController* dpc = g_controller.load(std::memory_order_acquire);
            if (dpc != nullptr && entry.ptr != nullptr) {
                std::uintptr_t obj = reinterpret_cast<std::uintptr_t>(entry.ptr);
                bool resolved = resolveClassName(obj, name_buf, sizeof(name_buf));
                if (resolved) {
                    g_diag_names_resolved.fetch_add(1, std::memory_order_relaxed);
                    // Emit As3Alloc with class name as label/payload.
                    // Note: aneprof currently uses As3ObjectEvent {sample_id, size,
                    // type_name_len, stack_len} + variable label. We synthesize a
                    // unique sample_id from ptr to allow downstream dedup.
                    aneprof::As3ObjectEvent ev{};
                    ev.sample_id = static_cast<std::uint64_t>(obj);
                    ev.size = entry.size;
                    ev.type_name_len = static_cast<std::uint32_t>(std::strlen(name_buf));
                    ev.stack_len = 0;
                    // The DPC's As3 emission path needs the event + name bytes
                    // appended. For now, emit via a generic marker — actual
                    // As3Alloc EventType emission requires DPC method we'll wire
                    // up next iteration once API surface is confirmed.
                    char marker_value[160];
                    std::snprintf(marker_value, sizeof(marker_value),
                                  "{\"class\":\"%s\",\"size\":%llu,\"ptr\":\"0x%llx\"}",
                                  name_buf,
                                  (unsigned long long)entry.size,
                                  (unsigned long long)obj);
                    dpc->marker("as3_alloc", marker_value);
                } else {
                    g_diag_names_unresolved.fetch_add(1, std::memory_order_relaxed);
                    // Record first kProbeDumpCount unresolved alloc bytes for
                    // post-hoc layout discovery. Picks every-Nth unresolved
                    // alloc so we don't all-bunch on identical tiny strings
                    // from one cycle.
                    static std::atomic<std::uint64_t> probe_skip{0};
                    std::uint64_t s = probe_skip.fetch_add(1, std::memory_order_relaxed);
                    if ((s & 0x3F) == 0) {  // every 64th unresolved
                        std::uint32_t idx = g_probe_count.fetch_add(1, std::memory_order_relaxed);
                        if (idx < kProbeDumpCount) {
                            ProbeDump& slot = g_probe_dumps[idx];
                            slot.ptr = static_cast<std::uint64_t>(obj);
                            slot.size = entry.size;
                            // Page-checked safe copy of first kProbeDumpBytes bytes.
                            for (std::size_t b = 0; b < kProbeDumpBytes; ++b) {
                                slot.bytes[b] = readU8(obj + b);
                            }
                        }
                    }
                }
                g_diag_allocs_observed.fetch_add(1, std::memory_order_relaxed);
            }

            ++r;
            processed_any = true;
        }

        g_ring_read.store(r, std::memory_order_release);

        if (!processed_any) {
            // No work — sleep. CV wait with 1ms timeout so we don't stall on
            // shutdown.
            std::unique_lock<std::mutex> lk(g_drain_mu);
            g_drain_cv.wait_for(lk, std::chrono::milliseconds(1));
        }
    }
}

} // namespace

bool AndroidAs3ObjectHook::recordAllocPending(void* ptr, std::size_t size) {
    if (!g_active.load(std::memory_order_acquire)) return false;
    if (ptr == nullptr) return false;
    // Filter: AS3 ScriptObject minimum size is sizeof(VTable*) + composite-pad +
    // VTable* + delegate = 24 bytes. Anything smaller can't be a ScriptObject.
    if (size < 24) return false;

    std::uint64_t w = g_ring_write.fetch_add(1, std::memory_order_acq_rel);
    std::uint64_t r = g_ring_read.load(std::memory_order_acquire);
    if (w - r >= kRingCapacity) {
        // Queue full — drop. Don't decrement write to avoid races; the slot
        // we claimed will simply be overwritten by a future entry. Track drop.
        g_diag_allocs_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    PendingAlloc& slot = g_ring[w & (kRingCapacity - 1)];
    slot.ptr = ptr;
    slot.size = static_cast<std::uint64_t>(size);
    slot.timestamp_ns = nowNs();
    slot.thread_id = 0;  // could fill from gettid() if needed
    g_diag_allocs_queued.fetch_add(1, std::memory_order_relaxed);
    return true;
}

AndroidAs3ObjectHook::~AndroidAs3ObjectHook() {
    uninstall();
}

bool AndroidAs3ObjectHook::install(DeepProfilerController* controller) {
    if (installed_) return true;
    if (controller == nullptr) return false;

    // Install signal handler BEFORE we start any thread that does pointer
    // chasing. The handler is process-wide and idempotent.
    ensureSegvHandlerInstalled();

    g_controller.store(controller, std::memory_order_release);
    g_layout = layout_;
    g_ring_write.store(0, std::memory_order_release);
    g_ring_read.store(0, std::memory_order_release);
    g_diag_allocs_observed.store(0);
    g_diag_names_resolved.store(0);
    g_diag_names_unresolved.store(0);
    g_diag_allocs_queued.store(0);
    g_diag_allocs_dropped.store(0);
    g_drain_stop.store(false, std::memory_order_release);
    g_active.store(true, std::memory_order_release);
    g_drain_thread = std::thread(drainThreadMain);
    installed_ = true;

    LOGI("install: As3 typed-alloc resolver active. Layout: vtable=+%u "
         "traits=+%u name=+%u buf=+%u len=+%u flags=+%u; ring=%zu entries; "
         "defer=%lluns",
         (unsigned)layout_.scriptobject_vtable_off,
         (unsigned)layout_.vtable_traits_off,
         (unsigned)layout_.traits_name_off,
         (unsigned)layout_.string_buffer_off,
         (unsigned)layout_.string_length_off,
         (unsigned)layout_.string_flags_off,
         kRingCapacity,
         (unsigned long long)kDeferDelayNs);
    return true;
}

void AndroidAs3ObjectHook::uninstall() {
    g_active.store(false, std::memory_order_release);
    g_drain_stop.store(true, std::memory_order_release);
    g_drain_cv.notify_all();
    if (g_drain_thread.joinable()) {
        g_drain_thread.join();
    }
    g_controller.store(nullptr, std::memory_order_release);

    // Dump diagnostic counters at teardown so we can tell whether the
    // walker actually saw any allocs and whether the pointer-chase
    // (VTable→Traits→name) succeeded. Zero observed = no allocs reached
    // the queue (deep memory hook isn't feeding); zero resolved with
    // non-zero unresolved = layout offsets wrong for this libCore.so
    // build; non-zero dropped = ring too small for the rate.
    LOGI("uninstall: stats observed=%llu queued=%llu dropped=%llu "
         "resolved=%llu unresolved=%llu",
         (unsigned long long)g_diag_allocs_observed.load(),
         (unsigned long long)g_diag_allocs_queued.load(),
         (unsigned long long)g_diag_allocs_dropped.load(),
         (unsigned long long)g_diag_names_resolved.load(),
         (unsigned long long)g_diag_names_unresolved.load());

    // Per-step failure breakdown — the largest counter pinpoints which
    // pointer offset is wrong for this libCore.so build.
    LOGI("uninstall: failures obj_invalid=%llu vtable_invalid=%llu "
         "traits_invalid=%llu name_invalid=%llu string_buf=%llu string_len=%llu",
         (unsigned long long)g_diag_fail_obj_invalid.load(),
         (unsigned long long)g_diag_fail_vtable_invalid.load(),
         (unsigned long long)g_diag_fail_traits_invalid.load(),
         (unsigned long long)g_diag_fail_name_invalid.load(),
         (unsigned long long)g_diag_fail_string_buf.load(),
         (unsigned long long)g_diag_fail_string_len.load());

    // Hex dump of the first probe samples — used offline to identify the
    // VTable offset by inspection (look for repeating dword patterns
    // across allocs of similar size — that's the vtable pointer).
    std::uint32_t n = g_probe_count.load();
    if (n > kProbeDumpCount) n = kProbeDumpCount;
    for (std::uint32_t i = 0; i < n; ++i) {
        const ProbeDump& d = g_probe_dumps[i];
        char hex[kProbeDumpBytes * 3 + 1];
        for (std::size_t b = 0; b < kProbeDumpBytes; ++b) {
            std::snprintf(hex + b * 3, 4, "%02x ", d.bytes[b]);
        }
        hex[kProbeDumpBytes * 3] = '\0';
        LOGI("probe[%u] ptr=0x%llx size=%llu bytes=%s",
             i,
             (unsigned long long)d.ptr,
             (unsigned long long)d.size,
             hex);
    }

    installed_ = false;
}

bool AndroidAs3ObjectHook::installProbe() {
    // TBD next iteration: synthesize a known sentinel via a stable allocation
    // path, walk it through resolveClassName(), confirm we get the expected
    // class name back. Until then, install() optimistically uses defaults.
    return true;
}

std::uint64_t AndroidAs3ObjectHook::allocsObserved() const {
    return g_diag_allocs_observed.load(std::memory_order_relaxed);
}

std::uint64_t AndroidAs3ObjectHook::namesResolved() const {
    return g_diag_names_resolved.load(std::memory_order_relaxed);
}

std::uint64_t AndroidAs3ObjectHook::namesUnresolved() const {
    return g_diag_names_unresolved.load(std::memory_order_relaxed);
}

std::uint64_t AndroidAs3ObjectHook::allocsQueued() const {
    return g_diag_allocs_queued.load(std::memory_order_relaxed);
}

std::uint64_t AndroidAs3ObjectHook::allocsDropped() const {
    return g_diag_allocs_dropped.load(std::memory_order_relaxed);
}

} // namespace ane::profiler
