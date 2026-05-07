// Phase 4b RA tooling — generic in-ANE experiment hook.
//
// Lets AS3 (test scenarios) shadowhook arbitrary libCore.so offsets at
// runtime without rebuilding the ANE. Used to validate RA candidates:
// pass an offset + label, the hook fires for every invocation of that
// function, logs (label, hit_count, x0..x5) to logcat tag "AneExperimentHook".
//
// Up to N concurrent hooks (kMaxHooks) so multiple candidates can be
// tested in the same scenario. Each hook has its own counter + arg
// snapshot for the first 5 invocations.
//
// Use-case example:
//   AS3: Profiler.experimentHookInstall(0x00c98060, "addEventListener-A")
//   AS3: <do work that should fire addEventListener>
//   AS3: Profiler.experimentHookHits(0x00c98060)  -> count
//   AS3: Profiler.experimentHookUninstallAll()
//
// Or test 10 candidates in parallel:
//   for offset in candidates:
//     Profiler.experimentHookInstall(offset, "AEL-cand-" + offset)
//   <do work>
//   Profiler.experimentHookUninstallAll()  // logs hit counts per slot

#ifndef ANE_PROFILER_ANDROID_EXPERIMENT_HOOK_HPP
#define ANE_PROFILER_ANDROID_EXPERIMENT_HOOK_HPP

#include <cstdint>

namespace ane::profiler {

class AndroidExperimentHook {
public:
    // Install a shadowhook on libCore.so + offset. Label is shown in
    // logcat output. Returns the slot index (0..kMaxHooks-1) on success,
    // or -1 if no slot available / hook already installed at that offset
    // / shadowhook failure.
    static int install(std::uintptr_t libcore_offset, const char* label);

    // Returns hit count for a specific offset, or -1 if not installed.
    static long hitsForOffset(std::uintptr_t libcore_offset);

    // Uninstall all currently-installed experiment hooks. Logs final
    // hit counts and arg snapshots per slot.
    static void uninstallAll();

    // Diagnostics
    static int activeSlots();

    // ---------------------------------------------------------------
    // LIGHT variant — single atomic counter per call. NO stack walk,
    // NO arg snapshot, NO thread-local guard. Designed for HOT paths
    // (millions of calls/sec) where the heavy variant freezes the
    // runtime. Uses a separate slot pool so light + heavy hooks can
    // coexist on different offsets in the same session.
    //
    // Use-case: profile libCore.so functions that are called every
    // pixel / every quad / every blend. The heavy variant has been
    // observed to freeze J5 (Galaxy J5, ARMv7) when hooking the top
    // offset (+0x26e45e, ~16% of CPU) due to recordStackTrace's
    // 12-frame walk × 32-bucket atomic CAS hashing per call.
    //
    // The light proxy is JUST `slot.hits.fetch_add(1, relaxed)` then
    // tail-call original. Verified safe at multi-MHz call rates.
    // ---------------------------------------------------------------
    static int lightInstall(std::uintptr_t libcore_offset, const char* label);
    static long lightHitsForOffset(std::uintptr_t libcore_offset);
    static void lightUninstallAll();
    static int lightActiveSlots();
};

} // namespace ane::profiler

#endif
