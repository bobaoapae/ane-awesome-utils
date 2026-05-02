package br.com.redesurftank.aneawesomeutils;

/**
 * Native helpers for {@code awesomeUtils_probeTick} and
 * {@code awesomeUtils_triggerMemoryPurge}. Implementations live in
 * {@code probe_native.cpp}, packaged in {@code libemulatordetector.so}
 * (the existing native lib for this ANE — already loaded by
 * {@link RuntimeStatsCollector}'s static initializer).
 *
 * <p>These probes are read-only diagnostic surfaces. They never block the
 * caller for more than a few milliseconds and never throw — failures are
 * signaled by {@code null} (for {@link #nativeProbeMaps()}) or by the
 * underlying {@code mallopt} return value (for {@link #nativeMallopt}).
 */
public final class ProbeNative {

    static {
        // Idempotent — the lib is normally already loaded by RuntimeStatsCollector.
        try { System.loadLibrary("emulatordetector"); } catch (Throwable ignored) {}
    }

    /** Index map for the {@code long[]} returned by {@link #nativeProbeMaps()}. */
    public static final int SLOT_TOTAL              = 0;
    public static final int SLOT_SCUDO_SECONDARY    = 1;
    public static final int SLOT_SCUDO_PRIMARY      = 2;
    public static final int SLOT_STACKS             = 3;
    public static final int SLOT_DALVIK             = 4;
    public static final int SLOT_OTHER              = 5;
    public static final int SLOT_TOTAL_KB           = 6;
    public static final int SLOT_SCUDO_SECONDARY_KB = 7;
    public static final int SLOT_COUNT              = 8;

    /**
     * Stream-parse {@code /proc/self/maps} in C++ and return aggregate
     * counters indexed by {@code SLOT_*} constants above. Returns {@code null}
     * on a hard read failure.
     */
    public static native long[] nativeProbeMaps();

    /**
     * Stream-parse {@code /proc/self/maps} and return per-path aggregates as
     * a JSON string with shape:
     * <pre>{"totalCount":N,"totalSizeKb":N,"byPath":{"path":{"count":N,"sizeKb":N},...}}</pre>
     * <p>Returns {@code null} on a hard read failure. Callers diff two
     * snapshots (before/after a workload) to find which path category leaked.
     */
    public static native String nativeProbeMapsByPath();

    /**
     * Forward to bionic {@code mallopt(param, value)}. Common values:
     * <ul>
     *   <li>{@code -101} — {@code M_PURGE} — recommended cleanup.</li>
     *   <li>{@code -104} — {@code M_PURGE_ALL} — aggressive purge.</li>
     *   <li>{@code -100} — {@code M_DECAY_TIME} — adjust decay window.</li>
     * </ul>
     */
    public static native int nativeMallopt(int param, int value);

    private ProbeNative() {}
}
