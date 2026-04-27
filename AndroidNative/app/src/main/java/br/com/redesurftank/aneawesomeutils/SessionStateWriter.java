package br.com.redesurftank.aneawesomeutils;

import android.os.Handler;
import android.os.Looper;

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Periodically persists {@link NativeLogManager}'s session-quality snapshot to
 * {@code .session_state.json}. On the next launch, that file is rotated to
 * {@code .session_state.previous.json} and merged into the crash bundle's
 * metadata as {@code quality_hints} — giving each crash report a "what was the
 * session like up to the moment of death" summary (uptime, error/warn counts,
 * last-log timestamp, milestones reached).
 *
 * <p>Mirrors the lifecycle of {@link RuntimeStatsCollector}: idempotent
 * {@code start()}, fixed cadence on the main looper, never throws on the tick
 * path. 10 s tick keeps the on-disk snapshot fresh enough that a crash + AS3
 * loader restart sees state from at most 10 s before the kill.
 */
public final class SessionStateWriter {
    private static final long INTERVAL_MS = 10_000L;
    // First tick at 2 s — late enough that init() finished registering, soon
    // enough to capture early-boot crashes (which are common when the issue
    // is wrong AS3 initialization order).
    private static final long FIRST_TICK_DELAY_MS = 2_000L;

    private static final AtomicBoolean started = new AtomicBoolean(false);
    private static Handler handler;

    /** Idempotent. First call wires up the 10s tick; later calls no-op. */
    public static void start() {
        if (!started.compareAndSet(false, true)) return;
        handler = new Handler(Looper.getMainLooper());
        handler.postDelayed(SessionStateWriter::tick, FIRST_TICK_DELAY_MS);
    }

    /** Force an immediate persist. Safe to call from any thread; delegates to main looper. */
    public static void flushNow() {
        if (handler == null) {
            // start() not called yet — fall back to a synchronous persist so
            // callers like onBackground() still get a snapshot even if the
            // bootstrap order is wrong.
            try { NativeLogManager.persistSessionState(); } catch (Throwable ignored) {}
            return;
        }
        handler.post(() -> {
            try { NativeLogManager.persistSessionState(); } catch (Throwable ignored) {}
        });
    }

    private static void tick() {
        try {
            NativeLogManager.persistSessionState();
        } catch (Throwable ignored) {
            // never crash on telemetry
        } finally {
            if (started.get()) handler.postDelayed(SessionStateWriter::tick, INTERVAL_MS);
        }
    }

    private SessionStateWriter() {}
}
