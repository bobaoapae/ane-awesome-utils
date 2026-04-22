package br.com.redesurftank.aneawesomeutils;

import android.os.Handler;
import android.os.Looper;

import com.adobe.fre.FREContext;

import java.util.LinkedHashSet;
import java.util.Set;

/**
 * Installs a runtime guard for AIR's AndroidInputConnection native methods to
 * recover from a known SIGSEGV caused by a UAF race between the IME binder
 * thread and AIR's teardown path. See AirIMEGuard.cpp for the full rationale.
 *
 * Usage: call {@link #install()} once early in app startup (done automatically
 * from {@link AneAwesomeUtilsExtension#initialize()}). Safe to call multiple
 * times — only the first successful call performs the JNI re-binding.
 *
 * Retry: libCore.so may not be loaded at FREExtension.initialize() time (AIR
 * defers the dlopen). If dlsym fails, nativeInstall returns false and we
 * schedule retries on the main looper with linear-then-capped backoff. Retries
 * stop as soon as one succeeds or after MAX_RETRIES attempts.
 */
public class AirIMEGuard {
    private static final String TAG = "AirIMEGuard";
    private static final String AIC_CLASS = "com.adobe.air.AndroidInputConnection";
    private static final int MAX_RETRIES = 10;
    // Delays in ms, indexed by attempt (0-based). After the last index, the
    // last value is reused. Total ceiling ≈ 18.75s if everything fails.
    private static final long[] RETRY_DELAYS_MS = { 250, 500, 1000, 2000, 3000, 3000, 3000, 3000, 3000, 3000 };

    static {
        // Same .so as the rest of the ANE. loadLibrary is idempotent — if
        // another class already loaded it (EmulatorDetection, NativeLogManager),
        // this is a no-op.
        try { System.loadLibrary("emulatordetector"); } catch (Throwable ignored) {}
    }

    private static volatile boolean installed = false;
    private static int retryAttempts = 0;

    public static synchronized boolean install() {
        if (installed) return true;
        // Adobe's AndroidInputConnection may be loaded by a different ClassLoader
        // than the ANE's (AIR can use a child class loader for runtimeClasses.jar).
        // RegisterNatives only affects the Class<?> we pass — so if we target the
        // wrong one, AIR's JNI lookup keeps returning the original symbol. We
        // resolve the class through every ClassLoader we can reach and register
        // against each distinct Class object; nativeInstall is cheap and returns
        // false for duplicates without crashing.
        Set<Class<?>> candidates = new LinkedHashSet<>();
        // FIRST probe: the PathClassLoader that loaded AIR's runtimeClasses.jar.
        // AndroidInputConnection lives in base.apk/runtimeClasses.jar and is
        // loaded by the same PathClassLoader as FREContext (both ship together).
        // This is the ONLY classloader whose ArtMethod table ART's art_jni_trampoline
        // dispatches through — RegisterNatives on any other Class<?> is silently
        // ignored by the JNI layer (returns JNI_OK but has no effect).
        try {
            tryLoad(candidates, FREContext.class.getClassLoader(), "air-fre");
        } catch (Throwable ignored) {}
        tryLoad(candidates, AirIMEGuard.class.getClassLoader(), "ane");
        tryLoad(candidates, Thread.currentThread().getContextClassLoader(), "thread-ctx");
        tryLoad(candidates, ClassLoader.getSystemClassLoader(), "system");
        try {
            Class<?> forName = Class.forName(AIC_CLASS);
            candidates.add(forName);
        } catch (Throwable ignored) {}

        if (candidates.isEmpty()) {
            AneAwesomeUtilsLogging.w(TAG, "AndroidInputConnection not found in any classloader, skipping install");
            return false;
        }

        boolean anyOk = false;
        for (Class<?> aicCls : candidates) {
            try {
                boolean ok = nativeInstall(aicCls);
                AneAwesomeUtilsLogging.i(TAG, "install target=" + aicCls
                        + " loader=" + aicCls.getClassLoader() + " ok=" + ok);
                anyOk = anyOk || ok;
            } catch (Throwable t) {
                AneAwesomeUtilsLogging.w(TAG, "nativeInstall threw for " + aicCls + ": " + t);
            }
        }
        installed = anyOk;
        if (!anyOk) {
            AneAwesomeUtilsLogging.w(TAG, "nativeInstall returned false for all " + candidates.size() + " candidates");
            scheduleRetry();
        } else if (retryAttempts > 0) {
            AneAwesomeUtilsLogging.i(TAG, "install succeeded on retry #" + retryAttempts);
        }
        return anyOk;
    }

    // Schedules install() again on the main looper. Called when nativeInstall
    // returned false — typically because libCore.so is not dlopen'd yet at
    // FREExtension.initialize() time. dlsym(RTLD_DEFAULT, ...) will start
    // returning the real JNI symbols once AIR's own runtime loader dlopen's
    // libCore, which we can't observe directly — so we poll with backoff.
    private static void scheduleRetry() {
        int attempt = retryAttempts;
        if (attempt >= MAX_RETRIES) {
            AneAwesomeUtilsLogging.w(TAG, "install: max retries (" + MAX_RETRIES + ") reached, giving up");
            return;
        }
        retryAttempts = attempt + 1;
        long delayMs = RETRY_DELAYS_MS[Math.min(attempt, RETRY_DELAYS_MS.length - 1)];
        AneAwesomeUtilsLogging.i(TAG, "install: scheduling retry #" + (attempt + 1) + " in " + delayMs + "ms");
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (!installed) install();
        }, delayMs);
    }

    private static void tryLoad(Set<Class<?>> out, ClassLoader cl, String label) {
        if (cl == null) return;
        try {
            out.add(cl.loadClass(AIC_CLASS));
        } catch (ClassNotFoundException e) {
            AneAwesomeUtilsLogging.d(TAG, "AndroidInputConnection absent from " + label + " classloader");
        } catch (Throwable t) {
            AneAwesomeUtilsLogging.w(TAG, "loadClass failed on " + label + ": " + t);
        }
    }

    private static native boolean nativeInstall(Class<?> androidInputConnectionClass);
}
