package br.com.redesurftank.aneawesomeutils;

import com.adobe.fre.FREContext;
import com.adobe.fre.FREExtension;

public class AneAwesomeUtilsExtension implements FREExtension {

    public static final String TAG = "AneAwesomeUtilsExtension";
    private AneAwesomeUtilsContext _context;

    @Override
    public void initialize() {
        // Install the AIR IME UAF guard as early as possible — ANE.initialize()
        // runs when AIR registers the extension, before the AS3 side even calls
        // awesomeUtils_initialize. If a user taps an input field before AS3 has
        // initialized the ANE, AIR's JNI lookup caches the original symbol and
        // later RegisterNatives is effectively ignored for that call chain.
        // Installing here closes that window. Idempotent — safe if already done.
        try { AirIMEGuard.install(); } catch (Throwable ignored) {}
        // Start periodic runtime-pressure sampling (30s tick on main looper).
        // Logs JSON snapshots into the upload pipeline AND pushes values into
        // the crash signal handler's breadcrumb slots, so future crash reports
        // carry "thread count + heap state at moment of death" for OOM triage.
        try { RuntimeStatsCollector.start(); } catch (Throwable ignored) {}
    }

    @Override
    public FREContext createContext(String s) {
        if (_context == null) {
            _context = new AneAwesomeUtilsContext();
        }
        return _context;
    }

    @Override
    public void dispose() {

    }
}
