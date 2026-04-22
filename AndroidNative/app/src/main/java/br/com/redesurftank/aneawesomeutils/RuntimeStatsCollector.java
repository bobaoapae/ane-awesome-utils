package br.com.redesurftank.aneawesomeutils;

import android.os.Debug;
import android.os.Handler;
import android.os.Looper;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Periodically samples process-wide runtime pressure (thread count, heap sizes,
 * VmRss/VmSize) and:
 *   1. Logs a JSON snapshot via AneAwesomeUtilsLogging — flows into the
 *      XOR-encoded log file uploaded on crash.
 *   2. Pushes the values into native breadcrumb slots (updated via
 *      nativeUpdateBreadcrumb) so the crash-signal handler appends the latest
 *      snapshot to the crash log footer. Next crash report therefore carries
 *      "state at moment of death" (thread count, heap %, VmSize) — key for
 *      OOM / thread-leak diagnosis.
 *
 * Runs on the main looper via Handler.postDelayed with a 30s interval.
 * No background thread added (doesn't add to thread count it measures).
 * ~1-2 ms per tick (one /proc read + Runtime calls + one JNI set).
 * Zero cost between ticks.
 */
public final class RuntimeStatsCollector {
    private static final String TAG = "RuntimeStats";
    // 30s — matches typical log upload cadence; infrequent enough that the
    // combined per-tick cost is immeasurable, frequent enough that the
    // breadcrumb is rarely older than a minute when a crash occurs.
    private static final long INTERVAL_MS = 30_000L;
    private static final long FIRST_TICK_DELAY_MS = 5_000L;

    private static final AtomicBoolean started = new AtomicBoolean(false);
    private static Handler handler;

    static {
        try { System.loadLibrary("emulatordetector"); } catch (Throwable ignored) {}
    }

    /** Idempotent. First call wires up the 30s tick; later calls no-op. */
    public static void start() {
        if (!started.compareAndSet(false, true)) return;
        handler = new Handler(Looper.getMainLooper());
        handler.postDelayed(RuntimeStatsCollector::tick, FIRST_TICK_DELAY_MS);
    }

    private static void tick() {
        try {
            int threadCount = countThreads();
            Runtime rt = Runtime.getRuntime();
            long jvmUsedKb = (rt.totalMemory() - rt.freeMemory()) / 1024L;
            long jvmMaxKb  = rt.maxMemory() / 1024L;
            long nativeKb  = Debug.getNativeHeapAllocatedSize() / 1024L;

            long vmRssKb = 0L, vmSizeKb = 0L;
            try (BufferedReader r = new BufferedReader(new FileReader("/proc/self/status"))) {
                String line;
                while ((line = r.readLine()) != null) {
                    if      (line.startsWith("VmRSS:"))  vmRssKb  = parseKb(line);
                    else if (line.startsWith("VmSize:")) vmSizeKb = parseKb(line);
                }
            } catch (Throwable ignored) { /* best-effort */ }

            // Single-pass concatenation; no Locale-sensitive String.format.
            String json = new StringBuilder(160)
                    .append("{\"threads\":").append(threadCount)
                    .append(",\"jvmUsedKb\":").append(jvmUsedKb)
                    .append(",\"jvmMaxKb\":").append(jvmMaxKb)
                    .append(",\"nativeKb\":").append(nativeKb)
                    .append(",\"vmRssKb\":").append(vmRssKb)
                    .append(",\"vmSizeKb\":").append(vmSizeKb)
                    .append('}')
                    .toString();

            AneAwesomeUtilsLogging.i(TAG, json);

            try {
                nativeUpdateBreadcrumb(threadCount, jvmUsedKb, jvmMaxKb, nativeKb, vmRssKb, vmSizeKb);
            } catch (UnsatisfiedLinkError ignored) {
                // native lib not loaded in edge cases (unit tests, etc) — log-only
            }
        } catch (Throwable t) {
            // Never crash on telemetry. Swallow quietly — one missed sample is fine.
        } finally {
            if (started.get()) handler.postDelayed(RuntimeStatsCollector::tick, INTERVAL_MS);
        }
    }

    /**
     * Count entries under /proc/self/task — one per live thread in the process.
     * Cheaper than {@code Thread.getAllStackTraces().size()} which stops every
     * thread to grab stacks; we only need the cardinality.
     */
    private static int countThreads() {
        String[] names = new File("/proc/self/task").list();
        return names == null ? -1 : names.length;
    }

    /** Parse "VmRSS:    16420 kB" → 16420. No regex (too slow for hot path). */
    private static long parseKb(String line) {
        long v = 0L;
        for (int i = 0, n = line.length(); i < n; i++) {
            char c = line.charAt(i);
            if (c >= '0' && c <= '9') v = v * 10L + (c - '0');
        }
        return v;
    }

    private static native void nativeUpdateBreadcrumb(
            int threadCount, long jvmUsedKb, long jvmMaxKb, long nativeKb, long vmRssKb, long vmSizeKb);

    private RuntimeStatsCollector() {}
}
