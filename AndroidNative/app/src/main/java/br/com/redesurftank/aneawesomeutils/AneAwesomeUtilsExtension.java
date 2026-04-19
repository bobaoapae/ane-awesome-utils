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
        // Install main-thread layout exception guard. Recovers from vendor-ROM
        // framework NPEs inside FrameLayout.layoutChildren (see crash report
        // rank 1, ~36 occ on Motorola edge 20). The bad layout pass is dropped;
        // the next frame re-lays-out normally.
        try { LayoutExceptionGuard.install(); } catch (Throwable ignored) {}
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
