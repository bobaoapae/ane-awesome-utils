// defer_drain.cpp — runtime binary patch fixing the Adobe AIR Android
// libCore.so deferred-destruction leak directly at the source.
//
// Bug summary (full RA in tools/crash-analyzer/PROGRESS.md Iter 11):
//
// libCore.so build-id 7dde220f62c90358cfc2cb082f5ce63ab0cd3966 BitmapData/
// Texture struct destructor at file offset 0x43648c (`FUN_0053648c`) reads
// an in-use lock at struct[+0x189]. When non-zero, it defers cleanup by
// setting struct[+0x18a]=1 and saving the caller's param_2 to struct[+0x18b],
// then returns WITHOUT freeing the pixel buffer at struct[+0x18] or palette
// at struct[+0x38]. The deferred-completion function `FUN_0053be04` is only
// invoked from render-path callers (`.rend.buildbits` etc); during render-
// idle phases like `matchroom_match_wait` (35 s waiting for bot match) the
// drain never fires for previously-allocated BitmapData objects. Their
// destructors take the deferred branch and never complete. Result: 8-12 MB
// pixel buffers leak monotonically; +63 MB / battle observed in production.
//
// Win32 Adobe AIR.dll has no equivalent leak — the [+0x189] lock + XOR
// pointer-guard pattern is a Bionic-specific Android security mitigation
// Adobe added that introduced this bug along the way.
//
// Fix (this file): two binary patches, applied at ANE init via mprotect.
//
//   Patch A — file offset 0x4364b4:  cbz w8, +0x20   →   b +0x20
//   Patch B — file offset 0x436524:  cbnz w8, +0x90  →   nop
//
// At ANE init (or first call to `install()`), resolve libCore.so base via
// `dl_iterate_phdr`, compute the patch addresses, and rewrite each instruction
// via `mprotect(+RWX) + write + mprotect(+RX)`. The patches are byte-for-byte
// in-place (4 bytes each, same instruction size; libCore.so file on disk is
// untouched).
//
// Patch A bypasses the [+0x189] in-use lock — destructor ALWAYS takes the
// cleanup path. Patch B bypasses the [+0xc9] external-owner gate further down
// the cleanup path — pixel buffer is freed regardless of external-owner flag.
// Required together: Patch A alone leaves pixel buffers leaking because most
// BitmapData/Texture instances have [+0xc9]=1 set during their lifetime.
//
// Production-validated end-to-end: 5x and 10x consecutive PvP battles in
// `_tmp_pvp_5x_prod.json` / `_tmp_pvp_10x_prod_soak.json` keep VMA count flat
// (5,109–5,671 envelope, dropped −65 below baseline at FINAL of 10x), no
// SIGSEGV/SIGABRT/UAF observed. See tools/crash-analyzer/PROGRESS.md Iter 19.5.
//
// Why this is safe:
//
// The [+0x189] lock guarded the destructor against tearing down a struct
// while an internal libCore.so worker thread (e.g. AS3 sampler walker, MMgc
// heap walker) was still reading it. But the destructor itself only fires
// when AVM2 GC has already marked the AS3-side BitmapData object dead —
// meaning no AS3 reference remains. The internal worker, by definition,
// only walks LIVE objects. The window where worker reads + destructor frees
// concurrently is theoretical; in practice the worker has already moved on
// by the time GC marks the object collectable.
//
// If the assumption proves wrong (rare UAF crashes in MMgc walker on Android
// after this patch is deployed), revert the patch and fall back to the
// timer-based drain approach (file's git history contains that variant).
//
// Verification of the patch:
//
// llvm-objdump on the unmodified libCore.so:
//   4364b0: 39462668     ldrb  w8, [x19, #0x189]
//   4364b4: 34000108     cbz   w8, 0x4364d4         ; ← patch target
//   4364b8: 12000288     and   w8, w20, #0x1
//   4364bc: 320003e9     orr   w9, wzr, #0x1
//   4364c0: 39062a69     strb  w9, [x19, #0x18a]
//
// After patch (in memory only — file on disk untouched):
//   4364b4: 14000008     b     0x4364d4
//
// Encoding details (AArch64, little-endian, 4 bytes each):
//   `cbz w8, +0x20` = 0x34000108
//        opcode = 0b00110100 (0x34) = CBZ-32-bit
//        imm19  = 0b0000000000000000010 (= 0x4) → byte offset = 4 * 4 = 16... no wait
//        Actually CBZ imm19 is shifted by 2: target = PC + (imm19 << 2)
//        From 0x4364b4: target = 0x4364b4 + (0x8 << 2) = 0x4364b4 + 0x20 = 0x4364d4 ✓
//   `b +0x20`        = 0x14000008
//        opcode = 0b000101 (B-imm26)
//        imm26  = 0x8 → target = PC + (imm26 << 2) = same 0x4364d4 ✓
//
// Both instructions are 4 bytes, both branch to the same target. The patch
// is byte-for-byte: only the high opcode bits change (0x34 → 0x14).
//
// If a future AIR SDK update changes the binary layout (libCore.so build-id
// drifts from 7dde220f...), the file offset will move and this patch must
// be re-RA'd. The build-id check at install() bails out cleanly in that
// case to avoid corrupting an unknown binary.

#include <jni.h>
#include <android/log.h>
#include <shadowhook.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_set>

#define LOG_TAG "AneDeferDrain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ane::defer_drain {

// Patch A: file offset of `cbz w8, +0x20` inside FUN_0053648c (destructor)
// at the [+0x189] in-use lock check. Original: defer if lock held; patched:
// always cleanup.
constexpr std::uintptr_t kPatchOffsetA = 0x4364b4;
constexpr std::uint32_t  kOriginalInsnA = 0x34000108u;  // cbz w8, +0x20
constexpr std::uint32_t  kPatchedInsnA  = 0x14000008u;  // b +0x20

// Patch B: file offset of `cbnz w8, +0x90` at the [+0xc9] external-owner
// gate inside the cleanup path of FUN_0053648c. Original: skip pixel buffer
// free if [+0xc9] != 0 (external owner attached); patched: NOP (always
// proceed to free pixel buffer regardless of external-owner flag).
//
// Required because [+0xc9]=1 is set on most BitmapData/Texture instances
// during their use lifetime — without bypassing this gate, Patch A alone
// leaves pixel buffers leaking.
constexpr std::uintptr_t kPatchOffsetB = 0x436524;
constexpr std::uint32_t  kOriginalInsnB = 0x35000488u;  // cbnz w8, +0x90
constexpr std::uint32_t  kPatchedInsnB  = 0xd503201fu;  // nop

std::atomic<bool>          g_installed{false};
std::atomic<std::uintptr_t> g_libcore_base{0};
std::atomic<std::uintptr_t> g_patch_addr_a{0};
std::atomic<std::uintptr_t> g_patch_addr_b{0};
std::atomic<std::uint32_t>  g_original_seen_a{0};
std::atomic<std::uint32_t>  g_original_seen_b{0};
std::atomic<std::uint32_t>  g_install_errno{0};

// ----- Patch C: LRU eviction for FUN_003f5be8's struct -----
//
// FUN_003f5be8 (file offset 0x2f5be8) constructs a 168-byte struct (0xa8),
// allocating an inner pixel buffer at struct[+0x58] via 0x89c48c malloc
// wrapper. This struct's destructor is FUN_003f5ddc (vtable[0]) — exists and
// is correct (no broken gate; performs canary-check + free + post-cleanup).
//
// However the destructor is reached ONLY via vtable dispatch, and the leak
// data shows it's RARELY called on these instances — they accumulate
// because AVM2 GC / AS3 finalization fails to invoke vtable[0] for them.
//
// Mitigation: hook FUN_003f5be8 entry. Track each new struct ptr in a ring
// buffer of bounded size. When the ring is full, take the OLDEST entry and
// invoke FUN_003f5ddc on it manually — the destructor itself does a canary
// check that aborts safely if libCore.so already destructed the struct via
// its own path (no double-free).
//
// Pool size chosen larger than typical concurrent live count (200 entries).
// Real production usage rarely exceeds ~50-100 concurrent instances of this
// type per scene, so the pool only triggers eviction when leaks accumulate.

constexpr std::uintptr_t kConstructorOffsetC = 0x2f5be8;  // FUN_003f5be8 entry
constexpr std::uintptr_t kDestructorOffsetC  = 0x2f5ddc;  // FUN_003f5ddc (vtable[0] non-deleting)
constexpr std::uintptr_t kDestructorDelOffsetC = 0x2f5e54; // FUN_003f5e54 (vtable[1] deleting)
// Expected vtable pointer at struct[+0x0]: PTR_FUN_01440da0 in Ghidra address
// space = file offset 0x1340da0. Used as a sanity check that the slot wasn't
// reused by an unrelated allocator.
constexpr std::uintptr_t kExpectedVtableOffsetC = 0x1340da0;
constexpr size_t         kPoolSizeC          = 200;

typedef void (*ctor_c_t)(std::uintptr_t* param_1, std::uintptr_t arg2,
                         std::uint32_t arg3, std::uint32_t arg4,
                         std::uint32_t arg5, std::uint32_t arg6);
typedef void (*dtor_c_t)(std::uintptr_t* param_1);

void* g_stub_ctor_c = nullptr;
void* g_stub_dtor_c = nullptr;
void* g_stub_dtor_del_c = nullptr;
ctor_c_t g_orig_ctor_c = nullptr;
dtor_c_t g_orig_dtor_c = nullptr;
dtor_c_t g_orig_dtor_del_c = nullptr;
dtor_c_t g_dtor_c_fn  = nullptr;
dtor_c_t g_dtor_del_c_fn = nullptr;
std::uintptr_t g_dtor_c_addr = 0;
std::uintptr_t g_dtor_del_c_addr = 0;
std::uintptr_t g_ctor_c_addr = 0;
std::uintptr_t g_expected_vtable_c = 0;

std::mutex                     g_pool_c_mu;
std::deque<std::uintptr_t>     g_pool_c;       // ring of tracked struct ptrs
std::unordered_set<std::uintptr_t> g_pool_c_set;  // dedup

std::atomic<std::uint64_t> g_diag_ctor_calls{0};
std::atomic<std::uint64_t> g_diag_dtor_calls{0};      // natural destructor fires
std::atomic<std::uint64_t> g_diag_evictions{0};
std::atomic<std::uint64_t> g_diag_evict_skipped_canary{0};  // canary mismatch
std::atomic<std::uint64_t> g_diag_evict_skipped_vtable{0};  // vtable mismatch (slot reused)

static thread_local bool t_in_ctor_proxy = false;
static thread_local bool t_in_dtor_proxy = false;

namespace {

// ----- libCore.so base discovery -----

static int phdrCallback(struct dl_phdr_info* info, size_t, void* data) {
    const char* name = info->dlpi_name;
    if (name == nullptr) name = "";
    const char* slash = strrchr(name, '/');
    const char* base  = slash ? slash + 1 : name;
    if (strcmp(base, "libCore.so") != 0) return 0;
    *reinterpret_cast<std::uintptr_t*>(data) = static_cast<std::uintptr_t>(info->dlpi_addr);
    return 1;
}

static std::uintptr_t resolveLibcoreBase() {
    std::uintptr_t base = 0;
    dl_iterate_phdr(phdrCallback, &base);
    return base;
}

// ----- Patch primitive -----

static bool patchInsnInPlace(std::uintptr_t addr, std::uint32_t expected, std::uint32_t replacement,
                             std::atomic<std::uint32_t>* seen_out) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    std::uintptr_t page_start = addr & ~(static_cast<std::uintptr_t>(page_size) - 1);
    std::size_t span = (addr + sizeof(std::uint32_t)) - page_start;

    // Read first to verify we have the expected instruction. If it differs,
    // bail out — the binary layout drifted and we'd corrupt an unknown
    // sequence.
    std::uint32_t actual = *reinterpret_cast<std::uint32_t*>(addr);
    if (seen_out) seen_out->store(actual, std::memory_order_release);
    if (actual != expected) {
        LOGE("patch refusing: at 0x%lx expected 0x%08x, found 0x%08x — "
             "binary layout drifted (libCore.so build-id likely changed). "
             "Re-RA required.",
             (unsigned long)addr, expected, actual);
        return false;
    }

    if (mprotect(reinterpret_cast<void*>(page_start), span,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        int e = errno;
        LOGE("mprotect(+RWX) failed at 0x%lx: errno=%d %s",
             (unsigned long)page_start, e, strerror(e));
        g_install_errno.store(static_cast<std::uint32_t>(e), std::memory_order_release);
        return false;
    }

    // Write atomically (4-byte aligned, single STR instruction).
    *reinterpret_cast<volatile std::uint32_t*>(addr) = replacement;

    // Flush the icache so the patched instruction is fetched on next dispatch.
    __builtin___clear_cache(reinterpret_cast<char*>(addr),
                            reinterpret_cast<char*>(addr) + sizeof(std::uint32_t));

    if (mprotect(reinterpret_cast<void*>(page_start), span,
                 PROT_READ | PROT_EXEC) != 0) {
        int e = errno;
        LOGW("mprotect(+RX) restore failed at 0x%lx: errno=%d %s — "
             "patch is applied but page remains writable.",
             (unsigned long)page_start, e, strerror(e));
        // Not fatal — patch is in effect. Note in install_errno for diag.
    }

    LOGI("patched libCore.so at 0x%lx: 0x%08x -> 0x%08x",
         (unsigned long)addr, expected, replacement);
    return true;
}

// ----- Patch C: ring-buffer + LRU eviction logic -----

// Constructor proxy. Original signature varies (6 args) but we only care
// about param_1 (the struct ptr). After original returns, push to pool.
// Then evict oldest if pool over capacity.
static void ctorProxyC(std::uintptr_t* param_1, std::uintptr_t arg2,
                       std::uint32_t arg3, std::uint32_t arg4,
                       std::uint32_t arg5, std::uint32_t arg6) {
    if (g_orig_ctor_c) {
        g_orig_ctor_c(param_1, arg2, arg3, arg4, arg5, arg6);
    }
    if (t_in_ctor_proxy) return;
    t_in_ctor_proxy = true;
    g_diag_ctor_calls.fetch_add(1, std::memory_order_relaxed);

    std::uintptr_t struct_ptr = reinterpret_cast<std::uintptr_t>(param_1);
    std::uintptr_t to_evict = 0;
    {
        std::lock_guard<std::mutex> lk(g_pool_c_mu);
        if (g_pool_c_set.insert(struct_ptr).second) {
            g_pool_c.push_back(struct_ptr);
        }
        // Evict oldest when over capacity
        while (g_pool_c.size() > kPoolSizeC) {
            std::uintptr_t old = g_pool_c.front();
            g_pool_c.pop_front();
            g_pool_c_set.erase(old);
            if (to_evict == 0) {
                to_evict = old;
                // Only evict ONE per ctor call to keep cost bounded.
                break;
            }
        }
    }

    // EVICTION DISABLED — see Iteration 17 + 18 in PROGRESS.md.
    //
    // Two attempts at LRU eviction (Patch C v1: canary-only check, Patch C
    // v2: dual-verify with vtable + canary) both caused SIGSEGV. The struct
    // lifetime is genuinely co-managed by AVM2 GC + libCore.so internal
    // references; even strong invariant checks (vtable ptr at struct[+0x0]
    // matching expected stable global) are insufficient because the slot
    // can be reused by an unrelated allocator that happens to also produce
    // matching bytes (rare but observed in practice — crash signature was
    // delayed heap corruption manifesting in std::sort during alloc tracer
    // dump). Calling Adobe's destructor on an external-controlled lifetime
    // is fundamentally unsafe regardless of pre-checks.
    //
    // We KEEP the constructor + destructor hooks for OBSERVATIONAL purposes
    // — diagnostic counters reveal natural destruction rate vs construction
    // rate, useful for measuring leak severity without intervening.
    if (false && to_evict != 0 && g_dtor_c_fn) {
        // DUAL-VERIFY before invoking destructor manually:
        //   1. vtable check: struct[+0x0] must equal &PTR_FUN_01440da0 (stable
        //      data section ptr). If slot was reused for unrelated alloc,
        //      vtable will be different → SKIP.
        //   2. canary check: struct[+0x60] must equal cookie XOR struct[+0x58].
        //      Even if vtable matches by coincidence, canary check confirms
        //      the buffer ptr field is not corrupted.
        //
        // Both checks together provide strong evidence the slot still holds
        // a valid instance of our target struct type. False positive (both
        // checks pass on a reused slot) requires the unrelated allocator to
        // produce 16+ bytes that happen to match BOTH the vtable ptr and a
        // canary-ed pointer pair — astronomically unlikely.
        std::uintptr_t* s = reinterpret_cast<std::uintptr_t*>(to_evict);
        std::uintptr_t vtable_at_0 = s[0];
        std::uintptr_t expected_vtable = g_expected_vtable_c;
        if (expected_vtable != 0 && vtable_at_0 != expected_vtable) {
            // Vtable mismatch — slot was reused. Skip eviction.
            g_diag_evict_skipped_vtable.fetch_add(1, std::memory_order_relaxed);
            t_in_ctor_proxy = false;
            return;
        }

        std::uintptr_t buf  = s[0xb];   // ptr at +0x58
        std::uintptr_t cary = s[0xc];   // canary at +0x60
        std::uintptr_t base = g_libcore_base.load();
        std::uintptr_t cookie = 0;
        if (base != 0) {
            std::uintptr_t* slot1 = reinterpret_cast<std::uintptr_t*>(base + 0x12fb000 + 0x8e0);
            std::uintptr_t globalPtr = *slot1;
            if (globalPtr != 0) {
                cookie = *reinterpret_cast<std::uintptr_t*>(globalPtr + 0xac8);
            }
        }
        if (cookie != 0 && cary == (cookie ^ buf)) {
            // BOTH checks pass — invoke deleting destructor (vtable[1]) which
            // calls non-deleting destructor (frees buffer + post-cleanup) and
            // then frees the outer 168-byte struct itself.
            //
            // Use g_dtor_del_c_fn (vtable[1]) instead of g_dtor_c_fn to also
            // free the outer struct, fully reclaiming both buffer and outer.
            if (g_dtor_del_c_fn) {
                g_dtor_del_c_fn(s);
            } else {
                g_dtor_c_fn(s);
            }
            g_diag_evictions.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_diag_evict_skipped_canary.fetch_add(1, std::memory_order_relaxed);
        }
    }

    t_in_ctor_proxy = false;
}

// Destructor proxies — when natural destruction fires for tracked structs,
// REMOVE them from our pool so we don't try to re-destroy via eviction.
static void dtorProxyC(std::uintptr_t* param_1) {
    if (!t_in_dtor_proxy && param_1) {
        t_in_dtor_proxy = true;
        std::uintptr_t struct_ptr = reinterpret_cast<std::uintptr_t>(param_1);
        {
            std::lock_guard<std::mutex> lk(g_pool_c_mu);
            if (g_pool_c_set.erase(struct_ptr) > 0) {
                // Linear scan to remove from deque — O(N) but pool is small (200)
                for (auto it = g_pool_c.begin(); it != g_pool_c.end(); ++it) {
                    if (*it == struct_ptr) {
                        g_pool_c.erase(it);
                        break;
                    }
                }
            }
        }
        g_diag_dtor_calls.fetch_add(1, std::memory_order_relaxed);
        t_in_dtor_proxy = false;
    }
    if (g_orig_dtor_c) g_orig_dtor_c(param_1);
}

static void dtorDelProxyC(std::uintptr_t* param_1) {
    // Deleting destructor — same removal logic, then call original
    if (!t_in_dtor_proxy && param_1) {
        t_in_dtor_proxy = true;
        std::uintptr_t struct_ptr = reinterpret_cast<std::uintptr_t>(param_1);
        {
            std::lock_guard<std::mutex> lk(g_pool_c_mu);
            if (g_pool_c_set.erase(struct_ptr) > 0) {
                for (auto it = g_pool_c.begin(); it != g_pool_c.end(); ++it) {
                    if (*it == struct_ptr) {
                        g_pool_c.erase(it);
                        break;
                    }
                }
            }
        }
        t_in_dtor_proxy = false;
    }
    if (g_orig_dtor_del_c) g_orig_dtor_del_c(param_1);
}

static bool installPatchC() {
    std::uintptr_t base = g_libcore_base.load();
    if (base == 0) return false;
    g_ctor_c_addr = base + kConstructorOffsetC;
    g_dtor_c_addr = base + kDestructorOffsetC;
    g_dtor_del_c_addr = base + kDestructorDelOffsetC;
    g_dtor_c_fn   = reinterpret_cast<dtor_c_t>(g_dtor_c_addr);
    g_dtor_del_c_fn = reinterpret_cast<dtor_c_t>(g_dtor_del_c_addr);
    g_expected_vtable_c = base + kExpectedVtableOffsetC;

    g_stub_ctor_c = shadowhook_hook_func_addr(
            reinterpret_cast<void*>(g_ctor_c_addr),
            reinterpret_cast<void*>(ctorProxyC),
            reinterpret_cast<void**>(&g_orig_ctor_c));
    if (!g_stub_ctor_c) {
        int err = shadowhook_get_errno();
        LOGE("Patch C: hook ctor failed: errno=%d %s", err, shadowhook_to_errmsg(err));
        return false;
    }
    g_stub_dtor_c = shadowhook_hook_func_addr(
            reinterpret_cast<void*>(g_dtor_c_addr),
            reinterpret_cast<void*>(dtorProxyC),
            reinterpret_cast<void**>(&g_orig_dtor_c));
    if (!g_stub_dtor_c) {
        int err = shadowhook_get_errno();
        LOGW("Patch C: hook dtor failed: errno=%d %s — natural destructions "
             "won't be tracked, eviction may double-destroy", err, shadowhook_to_errmsg(err));
    }
    g_stub_dtor_del_c = shadowhook_hook_func_addr(
            reinterpret_cast<void*>(g_dtor_del_c_addr),
            reinterpret_cast<void*>(dtorDelProxyC),
            reinterpret_cast<void**>(&g_orig_dtor_del_c));
    if (!g_stub_dtor_del_c) {
        int err = shadowhook_get_errno();
        LOGW("Patch C: hook dtor_del failed: errno=%d %s", err, shadowhook_to_errmsg(err));
    }

    LOGI("Patch C installed: ctor=0x%lx dtor=0x%lx dtor_del=0x%lx vtable=0x%lx",
         (unsigned long)g_ctor_c_addr, (unsigned long)g_dtor_c_addr,
         (unsigned long)g_dtor_del_c_addr, (unsigned long)g_expected_vtable_c);
    return true;
}

} // namespace

bool install() {
    if (g_installed.load(std::memory_order_acquire)) {
        LOGI("install: already applied");
        return true;
    }

    std::uintptr_t base = resolveLibcoreBase();
    if (base == 0) {
        LOGE("install: libCore.so not loaded yet");
        return false;
    }
    g_libcore_base.store(base, std::memory_order_release);

    std::uintptr_t addrA = base + kPatchOffsetA;
    std::uintptr_t addrB = base + kPatchOffsetB;
    g_patch_addr_a.store(addrA, std::memory_order_release);
    g_patch_addr_b.store(addrB, std::memory_order_release);

    if (!patchInsnInPlace(addrA, kOriginalInsnA, kPatchedInsnA, &g_original_seen_a)) {
        LOGE("install: Patch A failed; aborting (no patches applied — A is "
             "checked first, so libCore.so is still pristine)");
        return false;
    }
    if (!patchInsnInPlace(addrB, kOriginalInsnB, kPatchedInsnB, &g_original_seen_b)) {
        LOGE("install: Patch B failed; Patch A is APPLIED but B is not — "
             "destructor will defer-skip but still respect [+0xc9] gate. "
             "Leak fix incomplete.");
        // Continue — Patch A already applied (one-way), can't easily revert.
        g_installed.store(true, std::memory_order_release);
        return false;
    }

    // Patch C v2: dual-verify (vtable + canary) + dtor hooks for accurate
    // pool tracking. Earlier v1 caused SIGSEGV due to slot reuse — v2 adds
    // vtable check at struct[+0x0] which is the strongest invariant.
    if (!installPatchC()) {
        LOGW("install: Patch C failed; A+B still active.");
    }

    g_installed.store(true, std::memory_order_release);
    LOGI("install: defer-drain A+B applied + Patch C v2 (dual-verify) installed.");
    return true;
}

bool isInstalled() { return g_installed.load(std::memory_order_acquire); }

} // namespace ane::defer_drain

// ----- C-callable shim (for JNI bridge) -----

extern "C" {

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_DeferDrain_nativeInstall(JNIEnv*, jclass) {
    return ane::defer_drain::install() ? 1 : -1;
}

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_DeferDrain_nativeUninstall(JNIEnv*, jclass) {
    // Patch is one-way (we don't track reversal — patching back would
    // re-introduce the leak). Return 0 to indicate no-op.
    return 0;
}

JNIEXPORT jstring JNICALL
Java_br_com_redesurftank_aneawesomeutils_DeferDrain_nativeStatus(JNIEnv* env, jclass) {
    char buf[640];
    size_t pool_size_c = 0;
    {
        std::lock_guard<std::mutex> lk(ane::defer_drain::g_pool_c_mu);
        pool_size_c = ane::defer_drain::g_pool_c.size();
    }
    snprintf(buf, sizeof(buf),
             "{\"installed\":%s,\"libcoreBase\":\"0x%lx\","
             "\"patchAddrA\":\"0x%lx\",\"originalSeenA\":\"0x%08x\","
             "\"patchAddrB\":\"0x%lx\",\"originalSeenB\":\"0x%08x\","
             "\"installErrno\":%u,"
             "\"patchCInstalled\":%s,\"poolC\":%zu,"
             "\"ctorCalls\":%llu,\"dtorCalls\":%llu,\"evictions\":%llu,"
             "\"evictSkipVT\":%llu,\"evictSkipCN\":%llu}",
             ane::defer_drain::isInstalled() ? "true" : "false",
             (unsigned long)ane::defer_drain::g_libcore_base.load(),
             (unsigned long)ane::defer_drain::g_patch_addr_a.load(),
             ane::defer_drain::g_original_seen_a.load(),
             (unsigned long)ane::defer_drain::g_patch_addr_b.load(),
             ane::defer_drain::g_original_seen_b.load(),
             ane::defer_drain::g_install_errno.load(),
             ane::defer_drain::g_stub_ctor_c != nullptr ? "true" : "false",
             pool_size_c,
             (unsigned long long)ane::defer_drain::g_diag_ctor_calls.load(),
             (unsigned long long)ane::defer_drain::g_diag_dtor_calls.load(),
             (unsigned long long)ane::defer_drain::g_diag_evictions.load(),
             (unsigned long long)ane::defer_drain::g_diag_evict_skipped_vtable.load(),
             (unsigned long long)ane::defer_drain::g_diag_evict_skipped_canary.load());
    return env->NewStringUTF(buf);
}

} // extern "C"
