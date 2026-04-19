package br.com.redesurftank.aneawesomeutils;

import android.os.Handler;
import android.os.Looper;

/**
 * Catches and suppresses a narrow class of Android framework NPEs that fire
 * inside {@code FrameLayout.layoutChildren} on some vendor ROMs (seen heavily
 * on Motorola edge 20, Android 12-14). The framework race is:
 *
 * <pre>
 *   java.lang.NullPointerException: Attempt to invoke virtual method
 *   'int android.view.View.getVisibility()' on a null object reference
 *     at android.widget.FrameLayout.layoutChildren (FrameLayout.java:...)
 *     at android.widget.FrameLayout.onLayout (FrameLayout.java:...)
 *     at ...ViewRootImpl.performTraversals
 * </pre>
 *
 * {@code getChildAt(i)} returns null while {@code getChildCount()} already
 * returned {@code i+1} — a known vendor-side race between layout iteration and
 * concurrent view removal. It is not fixable from the app layer; the only
 * mitigation is to drop the bad layout pass and let the next frame re-lay-out.
 *
 * <p>Implementation uses the "Cockroach" pattern: we post a runnable to the
 * main looper that enters a nested {@link Looper#loop()} wrapped in try/catch.
 * The guard is extremely narrow — any exception not matching the exact
 * FrameLayout signature is re-thrown so the default crash handler runs.
 *
 * <p>Install once from {@link AneAwesomeUtilsExtension#initialize()} so the
 * nested loop is active before the first frame is drawn. Subsequent calls
 * are no-ops.
 */
public final class LayoutExceptionGuard {
    private static final String TAG = "LayoutExceptionGuard";
    private static volatile boolean installed = false;

    public static synchronized void install() {
        if (installed) return;
        installed = true;
        new Handler(Looper.getMainLooper()).post(() -> {
            while (true) {
                try {
                    Looper.loop();
                    // Looper.loop() only returns when quit() is called. If that
                    // happens our job is done.
                    return;
                } catch (Throwable e) {
                    if (!shouldSwallow(e)) {
                        // Propagate to the default uncaught handler so the app
                        // dies normally and Play Console still receives the
                        // crash. We cannot let the exception escape this
                        // runnable — that would just silently kill the main
                        // thread with no crash report.
                        Thread.UncaughtExceptionHandler ueh =
                                Thread.getDefaultUncaughtExceptionHandler();
                        if (ueh != null) {
                            ueh.uncaughtException(Thread.currentThread(), e);
                        }
                        return;
                    }
                    AneAwesomeUtilsLogging.w(TAG,
                            "Swallowed framework layout NPE: " + e.getClass().getSimpleName()
                                    + ": " + e.getMessage());
                    // Fall through: re-enter Looper.loop() and keep processing.
                }
            }
        });
    }

    private static boolean shouldSwallow(Throwable e) {
        if (!(e instanceof NullPointerException)) return false;
        String msg = e.getMessage();
        if (msg == null || !msg.contains("getVisibility()")) return false;
        StackTraceElement[] stack = e.getStackTrace();
        int depth = Math.min(4, stack.length);
        for (int i = 0; i < depth; i++) {
            StackTraceElement el = stack[i];
            if ("android.widget.FrameLayout".equals(el.getClassName())
                    && "layoutChildren".equals(el.getMethodName())) {
                return true;
            }
        }
        return false;
    }

    private LayoutExceptionGuard() {}
}
