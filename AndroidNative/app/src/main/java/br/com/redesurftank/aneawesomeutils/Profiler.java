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

    private static native int    nativeStart(String outputPath, String headerJson, int telemetryPort);
    private static native int    nativeStop();
    private static native String nativeGetStatus();

    private Profiler() {}
}
