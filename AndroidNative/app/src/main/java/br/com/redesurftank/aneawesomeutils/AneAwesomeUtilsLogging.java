package br.com.redesurftank.aneawesomeutils;

import android.util.Log;

import io.sentry.Sentry;
import io.sentry.SentryEvent;
import io.sentry.SentryLevel;
import io.sentry.protocol.Message;

public class AneAwesomeUtilsLogging {
    private static boolean enableReleaseLogging = false;

    public static void setEnableReleaseLogging(boolean enableReleaseLogging) {
        AneAwesomeUtilsLogging.enableReleaseLogging = enableReleaseLogging;
    }

    public static void d(String tag, String msg) {
        if (enableReleaseLogging) {
            Log.d(tag, msg);
        }
    }

    public static void d(String tag, String msg, Throwable throwable) {
        if (enableReleaseLogging) {
            Log.d(tag, msg, throwable);
        }
    }

    public static void e(String tag, String msg) {
        Log.e(tag, msg);
        SentryEvent event = new SentryEvent();
        Message message = new Message();
        message.setMessage(msg);
        message.setFormatted(String.format("[%s] %s", tag, msg));
        event.setMessage(message);
        event.setLevel(SentryLevel.ERROR);
        Sentry.captureEvent(event);
    }

    public static void e(String tag, String msg, Throwable throwable) {
        Log.e(tag, msg, throwable);
        SentryEvent event = new SentryEvent(throwable);
        Message message = new Message();
        message.setMessage(msg);
        message.setFormatted(String.format("[%s] %s", tag, msg));
        event.setMessage(message);
        event.setLevel(SentryLevel.ERROR);
        Sentry.captureEvent(event);
    }

    public static void i(String tag, String msg) {
        if (enableReleaseLogging) {
            Log.i(tag, msg);
        }
    }

    public static void v(String tag, String msg) {
        if (enableReleaseLogging) {
            Log.v(tag, msg);
        }
    }

    public static void w(String tag, String msg) {
        if (enableReleaseLogging) {
            Log.w(tag, msg);
        }
    }
}
