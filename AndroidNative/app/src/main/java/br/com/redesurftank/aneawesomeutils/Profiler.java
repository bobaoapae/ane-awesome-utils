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

    private Profiler() {}
}
