package br.com.redesurftank.aneawesomeutils;

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
 */
public class AirIMEGuard {
    private static final String TAG = "AirIMEGuard";
    private static final String AIC_CLASS = "com.adobe.air.AndroidInputConnection";

    static {
        // Same .so as the rest of the ANE. loadLibrary is idempotent — if
        // another class already loaded it (EmulatorDetection, NativeLogManager),
        // this is a no-op.
        try { System.loadLibrary("emulatordetector"); } catch (Throwable ignored) {}
    }

    private static volatile boolean installed = false;

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
        }
        return anyOk;
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
