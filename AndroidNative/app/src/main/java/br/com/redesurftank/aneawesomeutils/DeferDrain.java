package br.com.redesurftank.aneawesomeutils;

import com.bytedance.shadowhook.ShadowHook;

/**
 * Workaround for the Adobe AIR Android libCore.so deferred-destruction leak.
 *
 * <p>See {@code defer_drain.cpp} header comment + {@code PROGRESS.md} Iter 11
 * for the full RA write-up. Summary: libCore.so destructor at file offset
 * 0x43648c (BitmapData/Texture struct destructor) defers cleanup when an
 * in-use lock at struct[+0x189] is held; the deferred-completion function
 * at file offset 0x43be04 only fires from render-path callers; during the
 * 35 s matchroom_match_wait phase no render frames hit those callers, so
 * deferred destructions accumulate ad infinitum and pixel buffers leak.
 *
 * <p>Fix: hook the destructor, observe deferred-branch taken, snapshot the
 * owner struct ptr; background thread periodically calls the deferred-
 * completion function on each pending owner. Uses libCore.so's own drain
 * function — we don't free anything ourselves.
 *
 * <p>Activation: call {@link #install()} once on app startup (or just
 * before the first {@link AllocTracer#start()}). Idempotent. Background
 * thread runs at ~3 s cadence with negligible CPU (just walks a small
 * std::unordered_set and calls the drain fn).
 */
public final class DeferDrain {

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
            AneAwesomeUtilsLogging.e("DeferDrain", "ShadowHook.init returned " + rc);
        }
        sInited = true;
    }

    /** Install the destructor hook and start the background drain thread.
     *  Returns 1 on success, -1 on failure. Idempotent. */
    public static int install() {
        ensureShadowHookInited();
        return nativeInstall();
    }

    /** Stop the background thread and remove the destructor hook.
     *  Calls in flight may still complete normally. */
    public static int uninstall() {
        return nativeUninstall();
    }

    /** Returns a JSON string with installation state + diag counters. */
    public static String getStatus() {
        return nativeStatus();
    }

    private static native int nativeInstall();
    private static native int nativeUninstall();
    private static native String nativeStatus();

    private DeferDrain() {}
}
