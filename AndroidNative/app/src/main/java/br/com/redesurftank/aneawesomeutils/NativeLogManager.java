package br.com.redesurftank.aneawesomeutils;

import android.content.Context;
import android.util.Log;

import com.adobe.air.SharedAneLogger;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.logging.Level;
import java.util.logging.Logger;

public class NativeLogManager {
    private static final String TAG = "NativeLogManager";
    private static final String LOG_DIR_NAME = "ane-awesome-utils-logs";
    private static final String SESSION_MARKER = ".session_active";
    private static final int ROTATION_DAYS = 7;

    private static String logDirPath;
    private static String profile;
    private static File currentLogFile;
    private static FileOutputStream logOutputStream;
    private static AsyncLogHandler asyncHandler;
    private static boolean unexpectedShutdown;
    private static String unexpectedShutdownInfo;
    private static final Object lock = new Object();
    private static final SimpleDateFormat dateFormat = new SimpleDateFormat("yyyy-MM-dd", Locale.US);
    private static final SimpleDateFormat timestampFormat = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US);

    public static synchronized String init(Context context, String profileArg) {
        try {
            profile = "default";

            File baseDir = new File(context.getFilesDir(), LOG_DIR_NAME + "/" + profile);
            if (!baseDir.exists()) {
                baseDir.mkdirs();
            }
            logDirPath = baseDir.getAbsolutePath();

            // Check for unexpected shutdown
            File sessionMarker = new File(baseDir, SESSION_MARKER);
            if (sessionMarker.exists()) {
                unexpectedShutdown = true;
                unexpectedShutdownInfo = collectUnexpectedShutdownInfo(baseDir);
            } else {
                unexpectedShutdown = false;
                unexpectedShutdownInfo = null;
            }

            // Rotate old log files
            rotateOldFiles(baseDir);

            // Open today's log file
            String today = dateFormat.format(new Date());
            currentLogFile = new File(baseDir, "ane-log-" + today + ".txt");
            logOutputStream = new FileOutputStream(currentLogFile, true);

            // Configure SharedAneLogger with AsyncLogHandler
            Logger sharedLogger = SharedAneLogger.getInstance().getLogger();
            asyncHandler = new AsyncLogHandler(logOutputStream);
            sharedLogger.addHandler(asyncHandler);
            sharedLogger.setLevel(Level.ALL);

            // Create session marker
            new FileOutputStream(sessionMarker).close();

            Log.i(TAG, "NativeLogManager initialized, logDir=" + logDirPath);
            return logDirPath;
        } catch (Exception e) {
            Log.e(TAG, "Error initializing NativeLogManager", e);
            return null;
        }
    }

    private static String collectUnexpectedShutdownInfo(File baseDir) {
        StringBuilder sb = new StringBuilder();
        sb.append("[");
        String today = dateFormat.format(new Date());
        File[] files = baseDir.listFiles((dir, name) ->
                name.startsWith("ane-log-") && name.endsWith(".txt") && !name.equals("ane-log-" + today + ".txt"));
        if (files != null && files.length > 0) {
            Arrays.sort(files, (a, b) -> a.getName().compareTo(b.getName()));
            boolean first = true;
            for (File f : files) {
                if (!first) sb.append(",");
                first = false;
                String datePart = f.getName().replace("ane-log-", "").replace(".txt", "");
                sb.append("{\"date\":\"").append(datePart)
                        .append("\",\"size\":").append(f.length())
                        .append(",\"path\":\"").append(f.getAbsolutePath().replace("\\", "\\\\").replace("\"", "\\\""))
                        .append("\"}");
            }
        }
        sb.append("]");
        return sb.toString();
    }

    private static void rotateOldFiles(File baseDir) {
        long cutoffMillis = System.currentTimeMillis() - ((long) ROTATION_DAYS * 24 * 60 * 60 * 1000);
        File[] files = baseDir.listFiles((dir, name) -> name.startsWith("ane-log-") && name.endsWith(".txt"));
        if (files == null) return;
        for (File f : files) {
            try {
                String datePart = f.getName().replace("ane-log-", "").replace(".txt", "");
                Date fileDate = dateFormat.parse(datePart);
                if (fileDate != null && fileDate.getTime() < cutoffMillis) {
                    f.delete();
                    Log.d(TAG, "Rotated old log file: " + f.getName());
                }
            } catch (Exception e) {
                // Skip files with unparseable names
            }
        }
    }

    public static void write(String level, String tag, String message) {
        String timestamp;
        synchronized (timestampFormat) {
            timestamp = timestampFormat.format(new Date());
        }
        String line = "[" + timestamp + "] [" + level + "] [" + tag + "] " + message + "\n";

        synchronized (lock) {
            if (logOutputStream != null) {
                try {
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

        // Also log to SharedAneLogger
        try {
            Logger sharedLogger = SharedAneLogger.getInstance().getLogger();
            Level javaLevel;
            switch (level.toUpperCase(Locale.US)) {
                case "ERROR":
                    javaLevel = Level.SEVERE;
                    break;
                case "WARN":
                    javaLevel = Level.WARNING;
                    break;
                case "INFO":
                    javaLevel = Level.INFO;
                    break;
                case "DEBUG":
                default:
                    javaLevel = Level.FINE;
                    break;
            }
            sharedLogger.log(javaLevel, "[" + tag + "] " + message);
        } catch (Exception e) {
            // ignore
        }
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
            String datePart = f.getName().replace("ane-log-", "").replace(".txt", "");
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
            if (date == null || date.isEmpty()) {
                // Read all log files concatenated
                File[] files = dir.listFiles((d, name) -> name.startsWith("ane-log-") && name.endsWith(".txt"));
                if (files == null || files.length == 0) return new byte[0];
                Arrays.sort(files, (a, b) -> a.getName().compareTo(b.getName()));

                ByteArrayOutputStream baos = new ByteArrayOutputStream();
                for (File f : files) {
                    readFileInto(f, baos);
                }
                return baos.toByteArray();
            } else {
                // Read specific date file
                File f = new File(dir, "ane-log-" + date + ".txt");
                if (!f.exists()) return new byte[0];
                ByteArrayOutputStream baos = new ByteArrayOutputStream();
                readFileInto(f, baos);
                return baos.toByteArray();
            }
        } catch (Exception e) {
            Log.e(TAG, "Error reading log file", e);
            return new byte[0];
        }
    }

    private static void readFileInto(File file, ByteArrayOutputStream baos) throws IOException {
        FileInputStream fis = new FileInputStream(file);
        byte[] buffer = new byte[4096];
        int bytesRead;
        while ((bytesRead = fis.read(buffer)) != -1) {
            baos.write(buffer, 0, bytesRead);
        }
        fis.close();
    }

    public static boolean deleteLogFile(String date) {
        if (logDirPath == null) return false;
        File dir = new File(logDirPath);

        try {
            if (date == null || date.isEmpty()) {
                // Delete all log files (not session marker)
                File[] files = dir.listFiles((d, name) -> name.startsWith("ane-log-") && name.endsWith(".txt"));
                if (files == null) return true;
                boolean allDeleted = true;
                for (File f : files) {
                    if (!f.delete()) allDeleted = false;
                }
                return allDeleted;
            } else {
                File f = new File(dir, "ane-log-" + date + ".txt");
                return !f.exists() || f.delete();
            }
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

    public static boolean hadUnexpectedShutdown() {
        return unexpectedShutdown;
    }

    public static String getUnexpectedShutdownInfo() {
        return unexpectedShutdownInfo;
    }
}
