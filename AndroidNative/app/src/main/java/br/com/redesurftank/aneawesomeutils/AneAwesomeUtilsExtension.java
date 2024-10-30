package br.com.redesurftank.aneawesomeutils;

import com.adobe.fre.FREContext;
import com.adobe.fre.FREExtension;

public class AneAwesomeUtilsExtension implements FREExtension {

    public static final String TAG = "AneAwesomeUtilsExtension";
    private AneAwesomeUtilsContext _context;

    @Override
    public void initialize() {

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
