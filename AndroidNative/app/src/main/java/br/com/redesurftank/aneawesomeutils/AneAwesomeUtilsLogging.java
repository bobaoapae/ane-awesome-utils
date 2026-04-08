package br.com.redesurftank.aneawesomeutils;

import android.util.Log;

import java.util.logging.Level;
import java.util.logging.Logger;

import com.adobe.air.SharedAneLogger;

public class AneAwesomeUtilsLogging {
    private static boolean enableReleaseLogging = false;
    private static Logger _logger;

    public static void setEnableReleaseLogging(boolean enableReleaseLogging) {
        AneAwesomeUtilsLogging.enableReleaseLogging = enableReleaseLogging;
    }

    public static void d(String tag, String msg) {
        if (enableReleaseLogging) {
            log(Log.DEBUG, tag, msg);
        }
    }

    public static void d(String tag, String msg, Throwable throwable) {
        if (enableReleaseLogging) {
            log(Log.DEBUG, tag, msg, throwable);
        }
    }

    public static void e(String tag, String msg) {
        log(Log.ERROR, tag, msg);
    }

    public static void e(String tag, String msg, Throwable throwable) {
        log(Log.ERROR, tag, msg, throwable);
    }

    public static void i(String tag, String msg) {
        log(Log.INFO, tag, msg);
    }

    public static void v(String tag, String msg) {
        if (enableReleaseLogging) {
            log(Log.DEBUG, tag, msg);
        }
    }

    public static void w(String tag, String msg) {
        if (enableReleaseLogging) {
            log(Log.WARN, tag, msg);
        }
    }

    private static Logger getLogger() {
        if (_logger == null) {
            _logger = getSharedAneLogger();
        }
        return _logger;
    }

    private static Logger getSharedAneLogger() {
        try {
            Logger logger = SharedAneLogger.getInstance().getLogger();
            logger.log(Level.INFO, "Got shared ANE logger (direct)");
            return logger;
        } catch (Exception e) {
            Log.e("AneAwesomeUtilsLogging", "Failed to get shared ANE logger", e);
            return Logger.getLogger(AneAwesomeUtilsLogging.class.getName());
        }
    }

    private static void log(int priority, String tag, String message) {
        getLogger().log(getPriorityString(priority), tag + ": " + message);
    }

    private static void log(int priority, String tag, String message, Throwable throwable) {
        getLogger().log(getPriorityString(priority), tag + ": " + message, throwable);
    }

    private static Level getPriorityString(int priority) {
        switch (priority) {
            case Log.DEBUG:
                return Level.FINE;
            case Log.INFO:
                return Level.INFO;
            case Log.WARN:
                return Level.WARNING;
            case Log.ERROR:
                return Level.SEVERE;
            default:
                return Level.ALL;
        }
    }
}
