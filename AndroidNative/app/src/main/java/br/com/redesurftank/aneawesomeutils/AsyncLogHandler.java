package br.com.redesurftank.aneawesomeutils;

import java.io.FileOutputStream;
import java.io.IOException;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.logging.Handler;
import java.util.logging.LogRecord;

public class AsyncLogHandler extends Handler {
    private static final int MAX_QUEUE_SIZE = 500;

    private final BlockingQueue<LogRecord> queue;
    private final FileOutputStream outputStream;
    private final Thread writerThread;
    private volatile boolean running = true;

    public AsyncLogHandler(FileOutputStream outputStream) {
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
        if (!running || record == null) return;
        queue.offer(record); // Non-blocking, drops if full
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
