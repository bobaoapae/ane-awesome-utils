package br.com.redesurftank.aneawesomeutils;

import android.util.Log;

import java.io.IOException;
import java.io.OutputStream;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;

public class AsyncLogHandler extends Handler {
    private static final int MAX_QUEUE_SIZE = 500;
    // Single fixed tag for SharedAneLogger-routed messages so `adb logcat -s ANEShared`
    // captures every Java-side ANE log (AneAwesomeUtilsLogging) and AIR's internal
    // logs (via AIRLogger.Enable). NativeLogManager.write has its own per-message
    // tag passthrough for AS3 messages — those use a different code path.
    private static final String LOGCAT_TAG = "ANEShared";

    private final BlockingQueue<LogRecord> queue;
    private final OutputStream outputStream;
    private final Thread writerThread;
    private volatile boolean running = true;

    public AsyncLogHandler(OutputStream outputStream) {
        this.outputStream = outputStream;
        this.queue = new LinkedBlockingQueue<>(MAX_QUEUE_SIZE);
        this.writerThread = new Thread(this::processQueue, "AsyncLogHandler-Writer");
        this.writerThread.setDaemon(true);
        this.writerThread.start();
    }

    private void processQueue() {
        while (running || !queue.isEmpty()) {
            try {
                LogRecord record = queue.take();
                String formatted = formatRecord(record);
                outputStream.write(formatted.getBytes());
                outputStream.flush();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            } catch (IOException e) {
                // Silently drop on IO errors
            }
        }
        // Flush remaining records
        LogRecord record;
        while ((record = queue.poll()) != null) {
            try {
                String formatted = formatRecord(record);
                outputStream.write(formatted.getBytes());
            } catch (IOException e) {
                // ignore
            }
        }
        try {
            outputStream.flush();
        } catch (IOException e) {
            // ignore
        }
    }

    private String formatRecord(LogRecord record) {
        return record.getMillis() + " [" + record.getLevel() + "] " + record.getMessage() + "\n";
    }

    @Override
    public void publish(LogRecord record) {
        if (record == null) return;
        // Logcat echo runs unconditionally — independent of `running` and queue
        // capacity. The encrypted file is best-effort (drops on full / shut-down)
        // but logcat is the live debugging surface and must never silently lose
        // a line. android.util.Log is itself rate-limited by the kernel.
        echoToLogcat(record);
        if (!running) return;
        queue.offer(record); // Non-blocking, drops if full
    }

    private static void echoToLogcat(LogRecord record) {
        String msg = record.getMessage();
        if (msg == null) msg = "";
        Throwable t = record.getThrown();
        Level lvl = record.getLevel();
        int v = lvl == null ? Level.INFO.intValue() : lvl.intValue();
        if (v >= Level.SEVERE.intValue()) {
            if (t != null) Log.e(LOGCAT_TAG, msg, t); else Log.e(LOGCAT_TAG, msg);
        } else if (v >= Level.WARNING.intValue()) {
            if (t != null) Log.w(LOGCAT_TAG, msg, t); else Log.w(LOGCAT_TAG, msg);
        } else if (v >= Level.INFO.intValue()) {
            if (t != null) Log.i(LOGCAT_TAG, msg, t); else Log.i(LOGCAT_TAG, msg);
        } else {
            if (t != null) Log.d(LOGCAT_TAG, msg, t); else Log.d(LOGCAT_TAG, msg);
        }
    }

    @Override
    public void flush() {
        // Flushing is handled by the writer thread
    }

    @Override
    public void close() {
        running = false;
        writerThread.interrupt();
        try {
            writerThread.join(2000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
