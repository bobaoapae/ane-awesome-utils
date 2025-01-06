package br.com.redesurftank.aneawesomeutils;

import android.util.Log;

import java.util.logging.Level;
import java.util.logging.Logger;

import io.sentry.Sentry;
import io.sentry.SentryEvent;
import io.sentry.SentryLevel;
import io.sentry.protocol.Message;

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
        SentryEvent event = new SentryEvent();
        Message message = new Message();
        message.setMessage(msg);
        message.setFormatted(String.format("[%s] %s", tag, msg));
        event.setMessage(message);
        event.setLevel(SentryLevel.ERROR);
        Sentry.captureEvent(event);
    }

    public static void e(String tag, String msg, Throwable throwable) {
        log(Log.ERROR, tag, msg, throwable);
        SentryEvent event = new SentryEvent(throwable);
        Message message = new Message();
        message.setMessage(msg);
        message.setFormatted(String.format("[%s] %s", tag, msg));
        event.setMessage(message);
        event.setLevel(SentryLevel.ERROR);
        Sentry.captureEvent(event);
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
            Class<?> classz = Class.forName("com.adobe.air.SharedAneLogger");
            java.lang.reflect.Method method = classz.getMethod("getInstance");
            Object instance = method.invoke(null);
            method = classz.getMethod("getLogger");
            Logger logger = (Logger) method.invoke(instance);
            logger.log(Level.INFO, "Got shared ANE logger");
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
