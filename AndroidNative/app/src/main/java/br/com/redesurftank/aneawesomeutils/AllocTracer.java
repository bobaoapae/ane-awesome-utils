package br.com.redesurftank.aneawesomeutils;

import com.bytedance.shadowhook.ShadowHook;

/**
 * Native allocation tracer — instruments libCore.so's malloc/calloc/realloc/
 * free/mmap/munmap via PLT/GOT hooking (bytehook). Allocations >= 64 KB
 * have their full unwound stack captured into a live-allocation table; freed
 * allocs are removed. Calling {@link #dumpAllocs(int)} returns the largest N
 * surviving allocations as JSON, each with a symbolized stack trace.
 *
 * <p>Workflow:
 * <ol>
 *   <li>Snapshot mem before workload</li>
 *   <li>{@code start()} — installs hooks (only on libCore.so callers)</li>
 *   <li>Run the workload (e.g., a single PVP battle)</li>
 *   <li>{@code dumpAllocs(50)} — JSON of top-50 surviving allocs by size</li>
 *   <li>{@code stop()} — uninstalls hooks</li>
 * </ol>
 *
 * <p>The stack frames in each alloc are "libname+0xoffset" pairs, suitable
 * for offline addr2line lookup against the libCore.so build-id. The AS3
 * callsite is visible as the topmost {@code libCore.so+0x...} frame whose
 * offset corresponds to a known AVM2 dispatch point.
 */
public final class AllocTracer {

    private static volatile boolean sInited = false;

    static {
        try { System.loadLibrary("emulatordetector"); } catch (Throwable ignored) {}
    }

    private static synchronized void ensureShadowHookInited() {
        if (sInited) return;
        int rc = ShadowHook.init(new ShadowHook.ConfigBuilder()
                .setMode(ShadowHook.Mode.UNIQUE)  // dedupe identical hooks
                .setDebuggable(false)
                .build());
        if (rc != 0) {
            AneAwesomeUtilsLogging.e("AllocTracer", "ShadowHook.init returned " + rc);
        }
        sInited = true;
    }

    /** Begin tracing. Returns 1 on success, 0 if already active, -1 on failure. */
    public static int start() {
        ensureShadowHookInited();
        return nativeStart();
    }

    /** Stop tracing and uninstall hooks. Returns 1 on success, 0 if not active. */
    public static int stop() {
        return nativeStop();
    }

    /**
     * Snapshot the live-allocation table, sorted by size desc. {@code topN}
     * limits output (-1 = all).
     */
    public static String dumpAllocs(int topN) {
        return nativeDump(topN);
    }

    /**
     * Tag the current "phase" — every subsequent alloc record is stamped with
     * this name until the next mark. Used for attributing leaked allocations
     * to game phases (matchroom_enter, battle_start, etc.). Returns the
     * assigned phase id (>= 1).
     */
    public static int markPhase(String name) {
        return nativeMarkPhase(name == null ? "" : name);
    }

    /**
     * Walk the live table and {@code free()} every entry whose phase name
     * contains {@code phaseSubstring} AND was allocated more than
     * {@code minAgeMs} ago. Used to reclaim Android-only AIR runtime leaks
     * after the game scope owning those allocations has ended.
     *
     * <p>Returns JSON: {@code {"scanned","matched","freed","freedBytes",...}}.
     *
     * <p>Caller contract:
     * <ol>
     *   <li>AS3 GC ran twice + {@code mallopt(M_PURGE_ALL)} ran (so AS3 refs
     *       are dropped and scudo released what it could).</li>
     *   <li>The phase substring matches scopes that are LOGICALLY DEAD —
     *       e.g. {@code "matchroom"} only after fully back in hall.</li>
     *   <li>{@code minAgeMs} is high enough that the current phase's
     *       allocations are never matched (>= 2 seconds is safe).</li>
     * </ol>
     */
    public static String purgeStalePhase(String phaseSubstring, int minAgeMs, int maxFree) {
        return nativePurgeStalePhase(phaseSubstring == null ? "" : phaseSubstring, minAgeMs, maxFree);
    }

    private static native int nativeStart();
    private static native int nativeStop();
    private static native String nativeDump(int topN);
    private static native int nativeMarkPhase(String name);
    private static native String nativePurgeStalePhase(String phaseSubstring, int minAgeMs, int maxFree);

    private AllocTracer() {}
}
