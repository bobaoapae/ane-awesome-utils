package br.com.redesurftank.aneawesomeutils;

import com.bytedance.shadowhook.ShadowHook;

/**
 * Android profiler — Scout TCP byte tap that produces .flmc files compatible
 * with the cross-platform analyzer in {@code tools/profiler-cli/}.
 *
 * <p>Mirrors the Windows profiler's lifecycle:
 * <ol>
 *   <li>Adobe AIR is force-connected to a loopback Scout listener at port
 *     {@code telemetryPort} (game-side AS3 sets this up via {@code mm.cfg}
 *     or {@code TelemetrySession.startSampling}).</li>
 *   <li>{@code start(outputPath, headerJson, telemetryPort)} installs PLT
 *     hooks on libc {@code send}/{@code sendto}/{@code connect}/{@code close}.
 *     Sockets peered to {@code 127.0.0.1:telemetryPort} are marked; only
 *     bytes from those are captured into the .flmc.</li>
 *   <li>{@code stop()} uninstalls hooks and finalizes the file.</li>
 *   <li>{@code getStatus()} returns a JSON snapshot of capture state +
 *     diagnostic counters (bytes in/out, dropped, hook calls).</li>
 * </ol>
 */
public final class Profiler {

    private static volatile boolean sInited = false;

    static {
        try { System.loadLibrary("emulatordetector"); } catch (Throwable ignored) {}
    }

    private static synchronized void ensureShadowHookInited() {
        if (sInited) return;
        int rc = ShadowHook.init(new ShadowHook.ConfigBuilder()
                .setMode(ShadowHook.Mode.UNIQUE)
                .setDebuggable(false)
                .build());
        if (rc != 0) {
            AneAwesomeUtilsLogging.e("Profiler", "ShadowHook.init returned " + rc);
        }
        sInited = true;
    }

    /**
     * Begin capture. {@code telemetryPort} is the port AIR was force-connected
     * to for Scout telemetry (0 = capture all sends, no filter).
     * Returns 1 on success; negative on failure.
     */
    public static int start(String outputPath, String headerJson, int telemetryPort) {
        ensureShadowHookInited();
        return nativeStart(outputPath, headerJson, telemetryPort);
    }

    public static int stop() {
        return nativeStop();
    }

    public static String getStatus() {
        return nativeGetStatus();
    }

    /**
     * Begin capture in deep .aneprof mode. Mirrors the Windows profiler's
     * full event stream: timing (Phase 3 TBD), native memory via alloc_tracer
     * wiring, snapshots, markers. {@code as3Sampling} is reserved for Phase 4
     * (IMemorySampler hook on libCore.so) and currently logs a warning if true.
     *
     * <p>The user must also call {@link AllocTracer#start()} separately to
     * install the libc shadowhook proxies. This bridge wires alloc_tracer's
     * destination controller; the proxies themselves are independent.
     *
     * <p>Returns 1 on success; negative on failure (controller already active,
     * empty path, etc).
     */
    public static int startDeep(String outputPath, String headerJson,
                                 boolean timing, boolean memory, boolean snapshots,
                                 int maxLive, int snapshotIntervalMs,
                                 boolean as3Sampling) {
        ensureShadowHookInited();
        return nativeStartDeep(outputPath, headerJson, timing, memory, snapshots,
                                maxLive, snapshotIntervalMs, as3Sampling);
    }

    public static int stopDeep() {
        return nativeStopDeep();
    }

    public static boolean snapshot(String label) {
        return nativeSnapshot(label);
    }

    public static boolean marker(String name, String valueJson) {
        return nativeMarker(name, valueJson);
    }

    public static String getStatusDeep() {
        return nativeGetStatusDeep();
    }

    /**
     * Phase 3+4 compiler-injected method probe: called by AS3 code compiled
     * with {@code --profile-probes} on every method entry. Pushes the
     * {@code method_id} onto the per-thread method stack inside the
     * DeepProfilerController; native alloc events tagged via alloc_tracer
     * inherit the current top-of-stack as their {@code method_id} payload.
     */
    public static boolean probeEnter(int methodId) {
        return nativeProbeEnter(methodId);
    }

    /**
     * Companion to {@link #probeEnter(int)}. Pops the per-thread method
     * stack at AS3 method exit. {@code method_id} is included for
     * verification — the controller asserts the popped ID matches.
     */
    public static boolean probeExit(int methodId) {
        return nativeProbeExit(methodId);
    }

    /**
     * Register the method-id → name mapping table. Called once at app startup
     * with a packed binary blob produced by the AS3 compiler's
     * {@code --profile-probes} pass. {@code aneprof_analyze.py} uses this
     * table to render human-readable AS3 method names in reports.
     */
    public static boolean registerMethodTable(byte[] tableBytes) {
        return nativeRegisterMethodTable(tableBytes);
    }

    /**
     * Phase 7b — emit an AS3-side Frame event with optional label. Separate
     * from the Phase 6 RenderFrame events auto-emitted by the EGL hook;
     * intended for scene-transition markers, "battle_start", etc.
     */
    public static boolean recordFrame(long frameIndex, long durationNs,
                                       int allocationCount, long allocationBytes,
                                       String label) {
        return nativeRecordFrame(frameIndex, durationNs, allocationCount, allocationBytes, label);
    }

    /**
     * Phase 7a — programmatic GC trigger. Invokes {@code MMgc::GC::Collect} on
     * the captured GC singleton (recovered at runtime from the first observed
     * collection cycle through {@code AndroidGcHook}). Returns false if the
     * singleton has not been captured yet (no GC has fired since profiler
     * start), in which case AS3 should retry after the runtime triggers an
     * automatic collection.
     */
    public static boolean requestGc() {
        return nativeRequestGc();
    }

    /**
     * Warmup helper — install the GC observer hook EARLY (at app boot) so
     * natural startup GCs are observed and the GC singleton is captured
     * BEFORE the user calls profilerStart with as3ObjectSampling=true.
     *
     * Without this warmup, profilerStart's eager Phase 4a sampler hook
     * install fails (singleton not yet captured), and pc0/pc1 attribution
     * is unavailable for the session — only Phase 4c typed-alloc events
     * are emitted.
     *
     * Idempotent: safe to call multiple times. Cost when active: ~5ns
     * per natural GC cycle (rare). Per-alloc cost: zero.
     *
     * Recommended call site: very early in AS3 boot, e.g., from the
     * Loading.as init flow or first frame after app shows. Returns true
     * on successful hook install; false if libCore.so isn't loaded yet
     * or the build-id isn't recognized (re-call later in that case).
     */
    public static boolean warmupGcObserver() {
        return nativeWarmupGcObserver();
    }

    /**
     * RA helper — dump AvmCore (recovered via gc_this+0x10 from the captured
     * GC singleton) with a labeled prefix. Used during Phase 4a sampler RA
     * to take pre/post snapshots around {@code flash.sampler.startSampling()}.
     * Output goes to logcat tag {@code AneGcHook}. Returns false if no GC
     * has been captured yet.
     */
    public static boolean dumpAvmCore(String label) {
        return nativeDumpAvmCore(label);
    }

    /**
     * Phase 4a RA — install diagnostic hook on every non-null sampler
     * vftable slot. Requires {@link #requestGc()} to have captured the
     * GC singleton (call forceGcViaChurn first). Per-slot hit counts
     * logged at uninstall.
     */
    public static boolean samplerHookInstall() {
        return nativeSamplerHookInstall();
    }

    /** Phase 4a RA — uninstall + log per-slot hit counts. */
    public static boolean samplerHookUninstall() {
        return nativeSamplerHookUninstall();
    }

    /**
     * Phase 4a productive — install recordAllocationSample hook. Resolves
     * class names via Traits walk and emits as3_alloc_sampler markers.
     */
    public static boolean as3SamplerInstall() {
        return nativeAs3SamplerInstall();
    }

    /** Phase 4a productive — uninstall + log capture-rate stats. */
    public static boolean as3SamplerUninstall() {
        return nativeAs3SamplerUninstall();
    }

    /**
     * Phase 4b RA tooling — install a generic shadowhook on libCore.so +
     * offset. Logs args (x0..x5 AAPCS64) + hit counts to logcat tag
     * AneExperimentHook. Returns slot index (0..31) on success, -1 on
     * failure (no free slot, already hooked, or shadowhook error).
     *
     * Use to validate RA candidates without rebuilding the ANE: install
     * a few candidate offsets, run AS3 code that should fire one of them,
     * uninstall and inspect counts.
     */
    public static int experimentHookInstall(long libcoreOffset, String label) {
        return nativeExperimentHookInstall(libcoreOffset, label);
    }

    /** Returns hit count at the given offset, or -1 if not installed. */
    public static long experimentHookHits(long libcoreOffset) {
        return nativeExperimentHookHits(libcoreOffset);
    }

    /** Uninstall all experiment hooks; logs hit + arg snapshots. */
    public static void experimentHookUninstallAll() {
        nativeExperimentHookUninstallAll();
    }

    /**
     * LIGHT variant — single atomic counter per call. NO stack walk,
     * NO arg snapshot. For HOT libCore.so functions where the heavy
     * variant freezes the runtime (J5 Galaxy ARMv7 hooked +0x26e45e
     * with the heavy variant → AS3 timer-driven WebSocket timed out
     * → "Connection closed" because hook overhead × millions of
     * calls/sec stalled the AS3 main loop).
     *
     * Slot pool is separate from the heavy hook — light + heavy can
     * coexist on different offsets in the same session.
     */
    public static int experimentHookLightInstall(long libcoreOffset, String label) {
        return nativeExperimentHookLightInstall(libcoreOffset, label);
    }

    public static long experimentHookLightHits(long libcoreOffset) {
        return nativeExperimentHookLightHits(libcoreOffset);
    }

    public static void experimentHookLightUninstallAll() {
        nativeExperimentHookLightUninstallAll();
    }

    private static native int     nativeStart(String outputPath, String headerJson, int telemetryPort);
    private static native int     nativeStop();
    private static native String  nativeGetStatus();

    private static native int     nativeStartDeep(String outputPath, String headerJson,
                                                   boolean timing, boolean memory, boolean snapshots,
                                                   int maxLive, int snapshotIntervalMs,
                                                   boolean as3Sampling);
    private static native int     nativeStopDeep();
    private static native boolean nativeSnapshot(String label);
    private static native boolean nativeMarker(String name, String valueJson);
    private static native String  nativeGetStatusDeep();

    private static native boolean nativeProbeEnter(int methodId);
    private static native boolean nativeProbeExit(int methodId);
    private static native boolean nativeRegisterMethodTable(byte[] tableBytes);
    private static native boolean nativeRecordFrame(long frameIndex, long durationNs,
                                                     int allocationCount, long allocationBytes,
                                                     String label);
    private static native boolean nativeRequestGc();
    private static native boolean nativeWarmupGcObserver();
    private static native boolean nativeDumpAvmCore(String label);
    private static native boolean nativeSamplerHookInstall();
    private static native boolean nativeSamplerHookUninstall();
    private static native boolean nativeAs3SamplerInstall();
    private static native boolean nativeAs3SamplerUninstall();
    private static native int     nativeExperimentHookInstall(long offset, String label);
    private static native long    nativeExperimentHookHits(long offset);
    private static native void    nativeExperimentHookUninstallAll();
    private static native int     nativeExperimentHookLightInstall(long offset, String label);
    private static native long    nativeExperimentHookLightHits(long offset);
    private static native void    nativeExperimentHookLightUninstallAll();

    /**
     * RA helper — returns absolute address of recordAllocationSample
     * (Adobe IMemorySampler vtable[12]) computed via captured GC singleton.
     * Returns 0 if GC not yet captured. Use as `experimentHookInstall`
     * target to capture stack traces of every AS3 alloc.
     */
    public static long getSamplerRecordAllocAddr() {
        return nativeGetSamplerRecordAllocAddr();
    }
    private static native long    nativeGetSamplerRecordAllocAddr();

    private Profiler() {}
}
