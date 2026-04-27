package br.com.redesurftank.aneawesomeutils;

import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import com.adobe.air.SharedAneLogger;
import com.adobe.air.utils.AIRLogger;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FilterOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.logging.Level;
import java.util.logging.Logger;

public class NativeLogManager {
    private static final String TAG = "NativeLogManager";

    static {
        try { System.loadLibrary("emulatordetector"); } catch (Throwable ignored) {}
    }

    private static native void nativeInstallSignalHandler(String logFilePath, byte[] xorKey);
    private static final String LOG_DIR_NAME = "ane-awesome-utils-logs";
    private static final String SESSION_MARKER = ".session_active";
    private static final int ROTATION_DAYS = 7;
    private static final int MAX_LOG_FILES = 30;

    private static final byte[] LOG_XOR_KEY = {
        0x4A, 0x7B, 0x2C, 0x5D, 0x1E, 0x6F, 0x3A, (byte)0x8B,
        (byte)0x9C, 0x0D, (byte)0xFE, (byte)0xAF, 0x50, (byte)0xE1, 0x72, (byte)0xC3
    };

    private static String logDirPath;
    private static String profile;
    private static String currentDate;
    private static File currentLogFile;
    private static FileOutputStream rawLogOutputStream;
    private static XorOutputStream logOutputStream;
    private static AsyncLogHandler asyncHandler;
    private static final String BACKGROUND_MARKER = ".background_since";
    // Crash marker written by CrashSignalHandler.cpp when a native signal handler
    // fires. Presence on next launch = real crash; absence + SESSION_MARKER =
    // OS kill / user swipe-from-recents (don't report).
    private static final String CRASH_MARKER = ".crash_marker";
    // 15 min covers typical multitasking (responder mensagem, atender ligação, etc.)
    // sem perder LMK kills longos. Antes era 2 min, gerava muitos falsos positivos.
    private static final long BACKGROUND_GRACE_PERIOD_MS = 15 * 60 * 1000; // 15 minutes
    private static boolean unexpectedShutdown;
    private static String unexpectedShutdownInfo;
    private static final Object lock = new Object();
    private static final SimpleDateFormat dateFormat = new SimpleDateFormat("yyyy-MM-dd", Locale.US);
    private static final SimpleDateFormat sessionFormat = new SimpleDateFormat("yyyy-MM-dd_HHmmss", Locale.US);
    private static final SimpleDateFormat timestampFormat = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US);

    /**
     * OutputStream wrapper that XORs all bytes written through it.
     *
     * <p>Both write methods are {@code synchronized} on the instance because this
     * stream is shared between {@link NativeLogManager#write(String, String, String)}
     * (main thread, holding {@link #lock}) and {@link AsyncLogHandler}'s writer
     * thread. Without this, the two writers could interleave updates to
     * {@code offset}, producing garbled bytes that decrypt to junk.
     */
    private static class XorOutputStream extends FilterOutputStream {
        private final byte[] key;
        private long offset = 0;

        XorOutputStream(OutputStream out, byte[] key) {
            super(out);
            this.key = key;
        }

        @Override
        public synchronized void write(int b) throws IOException {
            out.write(b ^ (key[(int)(offset % key.length)] & 0xFF));
            offset++;
        }

        @Override
        public synchronized void write(byte[] buf, int off, int len) throws IOException {
            byte[] xored = new byte[len];
            for (int i = 0; i < len; i++) {
                xored[i] = (byte)(buf[off + i] ^ key[(int)((offset + i) % key.length)]);
            }
            out.write(xored, 0, len);
            offset += len;
        }
    }

    private static void xorDecrypt(byte[] data) {
        for (int i = 0; i < data.length; i++) {
            data[i] ^= LOG_XOR_KEY[i % LOG_XOR_KEY.length];
        }
    }

    public static synchronized String init(String storagePath, String profileArg) {
        try {
            profile = "default";

            File baseDir = new File(storagePath, LOG_DIR_NAME + "/" + profile);
            if (!baseDir.exists()) {
                baseDir.mkdirs();
            }
            logDirPath = baseDir.getAbsolutePath();

            // Detection algorithm for "real crash" vs "OS kill / user swipe":
            //   SESSION_MARKER existe?  N → fechou limpo, não reporta.
            //                            S → continuar:
            //   CRASH_MARKER existe?    S → signal handler disparou = crash real, REPORTA.
            //                            N → continuar (sem signal, pode ser kill normal):
            //   BACKGROUND_MARKER + grace > 15min → kill por OS em bg longo, não reporta.
            //   BACKGROUND_MARKER + grace < 15min → kill recente, dúvida — não reporta
            //     (antes reportava mas gera muitos falsos positivos com swipe-from-recents).
            //   Sem BG marker, sem CRASH marker → process killed em foreground sem signal
            //     (LMK extremo, OS reboot). Raro. Não reporta — sem signal não temos info útil.
            File sessionMarker = new File(baseDir, SESSION_MARKER);
            File bgMarker = new File(baseDir, BACKGROUND_MARKER);
            File crashMarker = new File(baseDir, CRASH_MARKER);
            if (sessionMarker.exists()) {
                if (crashMarker.exists()) {
                    // Real native crash: signal handler fired and wrote the marker.
                    unexpectedShutdown = true;
                    String crashedFilename = readMarkerContent(sessionMarker);
                    unexpectedShutdownInfo = collectUnexpectedShutdownInfo(baseDir, crashedFilename);
                    Log.i(TAG, "Previous session crashed (native signal), reporting");
                    crashMarker.delete();
                } else if (bgMarker.exists()) {
                    long bgSince = readTimestamp(bgMarker);
                    long elapsed = System.currentTimeMillis() - bgSince;
                    unexpectedShutdown = false;
                    unexpectedShutdownInfo = null;
                    if (bgSince > 0 && elapsed > BACKGROUND_GRACE_PERIOD_MS) {
                        Log.i(TAG, "Previous session killed by OS after " + (elapsed / 1000) + "s in background, not reporting");
                    } else {
                        Log.i(TAG, "Previous session killed in background (" + (elapsed / 1000) + "s) without crash signal, not reporting");
                    }
                } else {
                    // No bg marker, no crash marker. Either:
                    //  - User swipe-from-recents while app in foreground (Android may not deliver onPause/DEACTIVATE in time)
                    //  - Extreme LMK in foreground (rare on modern devices)
                    //  - OS reboot
                    // Without a signal we have no diagnostic info anyway. Skip to reduce noise.
                    unexpectedShutdown = false;
                    unexpectedShutdownInfo = null;
                    Log.i(TAG, "Previous session ended in foreground without crash signal, not reporting (likely swipe-from-recents or LMK)");
                }
                sessionMarker.delete();
                bgMarker.delete();
            } else {
                unexpectedShutdown = false;
                unexpectedShutdownInfo = null;
                bgMarker.delete(); // clean up stale bg marker if any
                crashMarker.delete(); // clean up stale crash marker if any
            }

            // Rotate old log files
            rotateOldFiles(baseDir);

            // Open a new session log file (each app launch = new file)
            Date now = new Date();
            currentDate = dateFormat.format(now);
            String sessionTs = sessionFormat.format(now);
            currentLogFile = new File(baseDir, "ane-log-" + sessionTs + ".txt");
            rawLogOutputStream = new FileOutputStream(currentLogFile, false);
            logOutputStream = new XorOutputStream(rawLogOutputStream, LOG_XOR_KEY);

            // fsync the file + parent directory to guarantee the entry is on disk before any crash.
            // Opening a FileOutputStream on a directory fails with EISDIR on Android, so use
            // android.system.Os.open(O_RDONLY) + Os.fsync to fsync the dir's inode entry.
            rawLogOutputStream.getFD().sync();
            try {
                FileDescriptor dfd = Os.open(baseDir.getAbsolutePath(), OsConstants.O_RDONLY, 0);
                try {
                    Os.fsync(dfd);
                } finally {
                    Os.close(dfd);
                }
            } catch (Exception ignored) {}

            // Configure SharedAneLogger with AsyncLogHandler
            Logger sharedLogger = SharedAneLogger.getInstance().getLogger();
            asyncHandler = new AsyncLogHandler(logOutputStream);
            sharedLogger.addHandler(asyncHandler);
            sharedLogger.setLevel(Level.ALL);

            // Install native signal handlers (SIGSEGV, SIGABRT, etc.) to log crash info.
            // We pass LOG_XOR_KEY so the handler can encode the crash footer with the
            // same scheme as XorOutputStream — otherwise the footer would come back as
            // garbage when readLogFile() XOR-decrypts the whole file.
            try { nativeInstallSignalHandler(currentLogFile.getAbsolutePath(), LOG_XOR_KEY); } catch (Throwable ignored) {}

            // Enable AIR runtime logging (captures internal AIR logs into SharedAneLogger)
            try { AIRLogger.Enable(true); } catch (Throwable ignored) {}

            // Install uncaught exception handler to log crashes before death
            installCrashHandler();

            // Create session marker with current log filename
            FileOutputStream markerOut = new FileOutputStream(sessionMarker);
            markerOut.write(("ane-log-" + sessionTs + ".txt").getBytes());
            markerOut.getFD().sync();
            markerOut.close();

            Log.i(TAG, "NativeLogManager initialized, logDir=" + logDirPath);
            return logDirPath;
        } catch (Exception e) {
            Log.e(TAG, "Error initializing NativeLogManager", e);
            return null;
        }
    }

    private static String readMarkerContent(File marker) {
        try {
            FileInputStream fis = new FileInputStream(marker);
            byte[] buf = new byte[(int) marker.length()];
            fis.read(buf);
            fis.close();
            return new String(buf).trim();
        } catch (Exception e) {
            return "";
        }
    }

    private static String collectUnexpectedShutdownInfo(File baseDir, String crashedFilename) {
        if (crashedFilename == null || crashedFilename.isEmpty()) return "[]";

        File crashedFile = new File(baseDir, crashedFilename);
        if (!crashedFile.exists()) return "[]";

        String datePart = extractDateFromFilename(crashedFilename);
        if (datePart == null) return "[]";

        return "[{\"date\":\"" + datePart
                + "\",\"size\":" + crashedFile.length()
                + ",\"path\":\"" + crashedFile.getAbsolutePath().replace("\\", "\\\\").replace("\"", "\\\"")
                + "\"}]";
    }

    private static String extractDateFromFilename(String filename) {
        // Extract YYYY-MM-DD from "ane-log-YYYY-MM-DD_HHmmss.txt" or "ane-log-YYYY-MM-DD.txt"
        if (!filename.startsWith("ane-log-") || filename.length() < 18) return null;
        return filename.substring(8, 18); // "YYYY-MM-DD"
    }

    private static void rotateOldFiles(File baseDir) {
        long cutoffMillis = System.currentTimeMillis() - ((long) ROTATION_DAYS * 24 * 60 * 60 * 1000);
        File[] files = baseDir.listFiles((dir, name) -> name.startsWith("ane-log-") && name.endsWith(".txt"));
        if (files == null) return;

        // Age-based rotation: drop anything older than ROTATION_DAYS.
        for (File f : files) {
            try {
                String datePart = extractDateFromFilename(f.getName());
                if (datePart == null) continue;
                Date fileDate = dateFormat.parse(datePart);
                if (fileDate != null && fileDate.getTime() < cutoffMillis) {
                    f.delete();
                    Log.d(TAG, "Rotated old log file: " + f.getName());
                }
            } catch (Exception e) {
                // Skip files with unparseable names
            }
        }

        // Count-based rotation: every session creates a new file, so frequent
        // launches pile up files until the age cutoff kicks in. Cap at
        // MAX_LOG_FILES and drop the oldest. Filenames embed a sortable
        // timestamp ("ane-log-YYYY-MM-DD_HHmmss.txt"), so name order == age order.
        files = baseDir.listFiles((dir, name) -> name.startsWith("ane-log-") && name.endsWith(".txt"));
        if (files == null || files.length <= MAX_LOG_FILES) return;
        Arrays.sort(files, (a, b) -> a.getName().compareTo(b.getName()));
        int toDelete = files.length - MAX_LOG_FILES;
        for (int i = 0; i < toDelete; i++) {
            if (files[i].delete()) {
                Log.d(TAG, "Rotated excess log file: " + files[i].getName());
            }
        }
    }

    public static void write(String level, String tag, String message) {
        Date now = new Date();
        String timestamp;
        synchronized (timestampFormat) {
            timestamp = timestampFormat.format(now);
        }
        String line = "[" + timestamp + "] [" + level + "] [" + tag + "] " + message + "\n";

        synchronized (lock) {
            if (logOutputStream != null) {
                try {
                    // Check if we need to rotate to a new day's file
                    String today;
                    synchronized (dateFormat) {
                        today = dateFormat.format(now);
                    }
                    if (!today.equals(currentDate)) {
                        currentDate = today;
                        logOutputStream.close();
                        String sessionTs;
                        synchronized (sessionFormat) {
                            sessionTs = sessionFormat.format(now);
                        }
                        currentLogFile = new File(logDirPath, "ane-log-" + sessionTs + ".txt");
                        rawLogOutputStream = new FileOutputStream(currentLogFile, false);
                        logOutputStream = new XorOutputStream(rawLogOutputStream, LOG_XOR_KEY);
                        try { nativeInstallSignalHandler(currentLogFile.getAbsolutePath(), LOG_XOR_KEY); } catch (Throwable ignored) {}
                    }
                    logOutputStream.write(line.getBytes());
                    logOutputStream.flush();
                } catch (IOException e) {
                    Log.e(TAG, "Error writing log", e);
                }
            }
        }

        // Also write to logcat
        switch (level.toUpperCase(Locale.US)) {
            case "ERROR":
                Log.e(tag, message);
                break;
            case "WARN":
                Log.w(tag, message);
                break;
            case "INFO":
                Log.i(tag, message);
                break;
            case "DEBUG":
            default:
                Log.d(tag, message);
                break;
        }

        // NOTE: we intentionally do NOT forward to SharedAneLogger here. The
        // AsyncLogHandler attached in init() would then write the same line to
        // logOutputStream a second time (in millis format), doubling the file
        // size for every AS3 message. SharedAneLogger still receives AIR's own
        // internal logs (via AIRLogger.Enable) and ANE-internal logs routed
        // through AneAwesomeUtilsLogging — those have no other sink, so the
        // single AsyncLogHandler path is correct for them.
    }

    public static String getLogFiles() {
        if (logDirPath == null) return "[]";
        File dir = new File(logDirPath);
        File[] files = dir.listFiles((d, name) -> name.startsWith("ane-log-") && name.endsWith(".txt"));
        if (files == null || files.length == 0) return "[]";

        Arrays.sort(files, (a, b) -> a.getName().compareTo(b.getName()));

        StringBuilder sb = new StringBuilder();
        sb.append("[");
        boolean first = true;
        for (File f : files) {
            if (!first) sb.append(",");
            first = false;
            String datePart = extractDateFromFilename(f.getName());
            if (datePart == null) continue;
            sb.append("{\"date\":\"").append(datePart)
                    .append("\",\"size\":").append(f.length())
                    .append(",\"path\":\"").append(f.getAbsolutePath().replace("\\", "\\\\").replace("\"", "\\\""))
                    .append("\"}");
        }
        sb.append("]");
        return sb.toString();
    }

    public static byte[] readLogFile(String date) {
        if (logDirPath == null) return new byte[0];
        File dir = new File(logDirPath);

        try {
            File[] files = dir.listFiles((d, name) -> name.startsWith("ane-log-") && name.endsWith(".txt"));
            if (files == null || files.length == 0) return new byte[0];
            Arrays.sort(files, (a, b) -> a.getName().compareTo(b.getName()));

            ByteArrayOutputStream result = new ByteArrayOutputStream();
            for (File f : files) {
                if (date != null && !date.isEmpty()) {
                    String fileDate = extractDateFromFilename(f.getName());
                    if (!date.equals(fileDate)) continue;
                }
                // Read entire file and XOR-decrypt (each file starts at offset 0)
                byte[] raw = readFileBytes(f);
                xorDecrypt(raw);
                result.write(raw);
            }
            return result.toByteArray();
        } catch (Exception e) {
            Log.e(TAG, "Error reading log file", e);
            return new byte[0];
        }
    }

    private static byte[] readFileBytes(File file) throws IOException {
        FileInputStream fis = new FileInputStream(file);
        ByteArrayOutputStream baos = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        int bytesRead;
        while ((bytesRead = fis.read(buffer)) != -1) {
            baos.write(buffer, 0, bytesRead);
        }
        fis.close();
        return baos.toByteArray();
    }

    public static boolean deleteLogFile(String date) {
        if (logDirPath == null) return false;
        File dir = new File(logDirPath);

        try {
            File[] files = dir.listFiles((d, name) -> name.startsWith("ane-log-") && name.endsWith(".txt"));
            if (files == null) return true;
            boolean allDeleted = true;
            for (File f : files) {
                if (date != null && !date.isEmpty()) {
                    String fileDate = extractDateFromFilename(f.getName());
                    if (!date.equals(fileDate)) continue;
                }
                if (!f.delete()) allDeleted = false;
            }
            return allDeleted;
        } catch (Exception e) {
            Log.e(TAG, "Error deleting log file", e);
            return false;
        }
    }

    public static synchronized void close() {
        try {
            if (asyncHandler != null) {
                asyncHandler.close();
                asyncHandler = null;
            }
        } catch (Exception e) {
            Log.e(TAG, "Error closing async handler", e);
        }

        synchronized (lock) {
            try {
                if (logOutputStream != null) {
                    logOutputStream.close();
                    logOutputStream = null;
                    rawLogOutputStream = null;
                }
            } catch (Exception e) {
                Log.e(TAG, "Error closing log output stream", e);
            }
        }

        // Delete session marker
        if (logDirPath != null) {
            File sessionMarker = new File(logDirPath, SESSION_MARKER);
            sessionMarker.delete();
        }
    }

    /**
     * Record that the app went to background. If the OS kills the process
     * after a long time in background, the next session uses this timestamp
     * to distinguish power-management kills from real crashes/force-closes.
     */
    public static void onBackground() {
        if (logDirPath == null) return;
        try {
            File bgMarker = new File(logDirPath, BACKGROUND_MARKER);
            FileOutputStream out = new FileOutputStream(bgMarker);
            out.write(String.valueOf(System.currentTimeMillis()).getBytes());
            out.getFD().sync();
            out.close();
        } catch (Exception e) {
            Log.e(TAG, "Error writing background marker", e);
        }
    }

    /**
     * Record that the app returned to foreground. Removes the background
     * marker so that crashes in foreground are always reported.
     */
    public static void onForeground() {
        if (logDirPath == null) return;
        try {
            File bgMarker = new File(logDirPath, BACKGROUND_MARKER);
            bgMarker.delete();
        } catch (Exception e) {
            Log.e(TAG, "Error deleting background marker", e);
        }
    }

    private static long readTimestamp(File file) {
        try {
            FileInputStream fis = new FileInputStream(file);
            byte[] buf = new byte[(int) file.length()];
            fis.read(buf);
            fis.close();
            return Long.parseLong(new String(buf).trim());
        } catch (Exception e) {
            return 0;
        }
    }

    public static boolean hadUnexpectedShutdown() {
        return unexpectedShutdown;
    }

    public static String getUnexpectedShutdownInfo() {
        return unexpectedShutdownInfo;
    }

    private static void installCrashHandler() {
        Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            try {
                String msg = "UNCAUGHT EXCEPTION - Thread: " + thread.getName() +
                        " (ID: " + thread.getId() + "): " + throwable;
                write("ERROR", TAG, msg);
            } catch (Exception ignored) {}
            if (previous != null) {
                previous.uncaughtException(thread, throwable);
            }
        });
    }
}
