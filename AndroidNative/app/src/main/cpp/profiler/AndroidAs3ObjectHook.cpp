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
#include <cstdint>
#include <cstring>

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

// Reusable layout instance. Populated from defaults and validated by probe.
static AndroidAs3ObjectHook::LayoutOffsets g_layout{};

// ---------------------------------------------------------------------------
// Pointer validation — given a candidate pointer, decide if it points into
// the GC heap region. Conservative checks reject most non-heap pointers.

static inline bool isPlausibleHeapPtr(std::uintptr_t p) {
    if (p == 0) return false;
    if (p & 0x7) return false;            // 8-byte aligned
    // Userspace range on AArch64 Android is typically 0x0000_0000_0000_0000..
    // 0x0000_007F_FFFF_FFFF (39-bit). Reject obvious tag values, RCObject
    // poison patterns, and kernel-space.
    if (p < 0x1000) return false;
    if (p > 0x0000007FFFFFFFFFULL) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Read a u64 from a possibly-bad pointer. Returns 0 on segfault. We use a
// signal-handler-protected read here, but for the initial implementation a
// straight read is used — the caller is expected to validate ranges first.
//
// TBD: install a thread-local SIGSEGV trampoline like Windows crash detector
// does; for now rely on isPlausibleHeapPtr filters.

static inline std::uint64_t readU64(std::uintptr_t addr) {
    if (!isPlausibleHeapPtr(addr)) return 0;
    return *reinterpret_cast<const volatile std::uint64_t*>(addr);
}

static inline std::uint32_t readU32(std::uintptr_t addr) {
    if (addr == 0 || (addr & 0x3) || addr < 0x1000) return 0;
    return *reinterpret_cast<const volatile std::uint32_t*>(addr);
}

static inline std::int32_t readI32(std::uintptr_t addr) {
    return static_cast<std::int32_t>(readU32(addr));
}

static inline std::uint8_t readU8(std::uintptr_t addr) {
    if (addr == 0 || addr < 0x1000) return 0;
    return *reinterpret_cast<const volatile std::uint8_t*>(addr);
}

// ---------------------------------------------------------------------------
// Walk: ScriptObject → VTable → Traits → Stringp _name → UTF-8 chars.
//
// Returns the resolved class name in `out` (max bytes-1 chars, NUL-terminated)
// on success, or false if any step fails validation.

bool resolveClassName(std::uintptr_t obj_ptr, char* out, std::size_t out_capacity) {
    if (out_capacity < 2) return false;
    out[0] = '\0';

    if (!isPlausibleHeapPtr(obj_ptr)) return false;

    // Step 1: read AVM2 VTable* from ScriptObject + 16
    std::uintptr_t vtable = readU64(obj_ptr + g_layout.scriptobject_vtable_off);
    if (!isPlausibleHeapPtr(vtable)) return false;

    // Step 2: read Traits* from VTable + 40
    std::uintptr_t traits = readU64(vtable + g_layout.vtable_traits_off);
    if (!isPlausibleHeapPtr(traits)) return false;

    // Step 3: read Stringp _name from Traits + 144
    std::uintptr_t name_str = readU64(traits + g_layout.traits_name_off);
    if (!isPlausibleHeapPtr(name_str)) return false;

    // Step 4: read String fields. The buffer pointer is at +8, length at +24,
    // bitsAndFlags at +28.
    std::uintptr_t buf  = readU64(name_str + g_layout.string_buffer_off);
    std::int32_t   len  = readI32(name_str + g_layout.string_length_off);
    std::uint32_t  bits = readU32(name_str + g_layout.string_flags_off);

    if (!isPlausibleHeapPtr(buf)) return false;
    if (len <= 0 || len > 256) return false;  // class names are always reasonable

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

} // namespace

AndroidAs3ObjectHook::~AndroidAs3ObjectHook() {
    uninstall();
}

bool AndroidAs3ObjectHook::install(DeepProfilerController* controller) {
    if (installed_) return true;
    if (controller == nullptr) return false;

    // Default offsets are assumed valid for the target build-id. Probe before
    // going hot — but the probe itself is also deferred. For now: install in
    // skeleton mode, hook firing deferred to next iteration.
    g_controller.store(controller, std::memory_order_release);
    g_layout = layout_;
    g_active.store(true, std::memory_order_release);
    installed_ = true;

    LOGI("install: skeleton mode — hook integration with FixedMalloc::Alloc "
         "deferred. Layout: vtable=+%u traits=+%u name=+%u buf=+%u len=+%u flags=+%u",
         (unsigned)layout_.scriptobject_vtable_off,
         (unsigned)layout_.vtable_traits_off,
         (unsigned)layout_.traits_name_off,
         (unsigned)layout_.string_buffer_off,
         (unsigned)layout_.string_length_off,
         (unsigned)layout_.string_flags_off);
    return true;
}

void AndroidAs3ObjectHook::uninstall() {
    g_active.store(false, std::memory_order_release);
    g_controller.store(nullptr, std::memory_order_release);
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

} // namespace ane::profiler
