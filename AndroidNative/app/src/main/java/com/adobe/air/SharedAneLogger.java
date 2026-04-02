package com.adobe.air;

import java.util.logging.Logger;

public class SharedAneLogger {
    private static SharedAneLogger _instance;
    private final Logger _logger;

    public static synchronized SharedAneLogger getInstance() {
        if (_instance == null) {
            _instance = new SharedAneLogger();
        }
        return _instance;
    }

    public SharedAneLogger() {
        _logger = Logger.getLogger("SharedAneLogger");
    }

    public Logger getLogger() {
        return _logger;
    }
}
