package br.com.redesurftank.aneawesomeutils;

import android.os.Build;

import java.io.File;
import java.io.FileOutputStream;
import java.util.Arrays;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Packages a crash session's log + metadata into a single ZIP uploadable via the
 * existing multipart endpoint. Replaces the raw-.txt upload path — the ZIP carries
 * the same plaintext log PLUS device/version/breadcrumb context in a sidecar JSON,
 * turning each crash into a self-describing bundle.
 *
 * Output layout (inside the zip):
 *   crash.log         XOR-decrypted log contents (same bytes the old uploader posted)
 *   metadata.json     build + device + session metadata for post-mortem classification
 *
 * ZIP is stored at {storagePath}/crash-bundles/crash-bundle-{date}-{ts}.zip.
 * Bundles are pruned by {@link #pruneBundles(String, int, long)} — AS3 calls that
 * on successful upload to free space.
 *
 * Threading: no explicit synchronization. Intended to be called on a background
 * executor (the FREFunction wrapper in AneAwesomeUtilsContext does this via
 * _backGroundExecutor). NativeLogManager.readLogFile() is already thread-safe.
 */
public final class CrashBundleBuilder {
    private static final String TAG = "CrashBundleBuilder";
    private static final String BUNDLE_DIR_NAME = "crash-bundles";

    /**
     * Build a ZIP for a given log date. If {@code date} is null/empty, packages
     * the latest available log via NativeLogManager's "all logs for latest day"
     * semantics. Returns the absolute path of the created ZIP, or null on
     * failure (missing log, IO error).
     */
    public static String build(String storagePath, String date, String appVersion, String sessionId) {
        if (storagePath == null || storagePath.isEmpty()) return null;

        byte[] logBytes;
        try {
            logBytes = NativeLogManager.readLogFile(date);
        } catch (Throwable t) {
            AneAwesomeUtilsLogging.w(TAG, "readLogFile failed: " + t);
            return null;
        }
        if (logBytes == null || logBytes.length == 0) {
            AneAwesomeUtilsLogging.w(TAG, "no log bytes for date=" + date);
            return null;
        }

        File bundleDir = new File(storagePath, BUNDLE_DIR_NAME);
        if (!bundleDir.exists() && !bundleDir.mkdirs()) {
            AneAwesomeUtilsLogging.w(TAG, "failed to create bundle dir: " + bundleDir);
            return null;
        }

        String dateKey = (date == null || date.isEmpty()) ? "latest" : date;
        String fname = "crash-bundle-" + dateKey + "-" + System.currentTimeMillis() + ".zip";
        File zipFile = new File(bundleDir, fname);

        try (ZipOutputStream zos = new ZipOutputStream(new FileOutputStream(zipFile))) {
            zos.setLevel(9); // max compression — logs are very compressible, worth the ms

            ZipEntry logEntry = new ZipEntry("crash.log");
            zos.putNextEntry(logEntry);
            zos.write(logBytes);
            zos.closeEntry();

            String meta = buildMetadataJson(date, appVersion, sessionId, logBytes.length);
            ZipEntry metaEntry = new ZipEntry("metadata.json");
            zos.putNextEntry(metaEntry);
            zos.write(meta.getBytes("UTF-8"));
            zos.closeEntry();
        } catch (Throwable t) {
            AneAwesomeUtilsLogging.e(TAG, "zip write failed: " + t);
            zipFile.delete();
            return null;
        }

        AneAwesomeUtilsLogging.i(TAG, "bundle created: " + zipFile.getAbsolutePath()
                + " (log=" + logBytes.length + "B, zip=" + zipFile.length() + "B)");
        return zipFile.getAbsolutePath();
    }

    /**
     * Drop bundle files older than {@code maxAgeMs} OR beyond the {@code keepLast}
     * most-recent. Safe to call on every startup.
     */
    public static void pruneBundles(String storagePath, int keepLast, long maxAgeMs) {
        if (storagePath == null) return;
        File bundleDir = new File(storagePath, BUNDLE_DIR_NAME);
        if (!bundleDir.exists()) return;
        File[] files = bundleDir.listFiles((d, n) -> n.startsWith("crash-bundle-") && n.endsWith(".zip"));
        if (files == null || files.length == 0) return;
        Arrays.sort(files, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        long now = System.currentTimeMillis();
        for (int i = 0; i < files.length; i++) {
            boolean overCount = i >= keepLast;
            boolean tooOld = (now - files[i].lastModified()) > maxAgeMs;
            if (overCount || tooOld) {
                if (files[i].delete()) {
                    AneAwesomeUtilsLogging.d(TAG, "pruned: " + files[i].getName());
                }
            }
        }
    }

    /** Delete a specific bundle by absolute path — called by AS3 after upload succeeds. */
    public static boolean delete(String bundlePath) {
        if (bundlePath == null) return false;
        File f = new File(bundlePath);
        return f.exists() && f.delete();
    }

    private static String buildMetadataJson(String date, String appVersion, String sessionId, int logSize) {
        StringBuilder sb = new StringBuilder(512);
        sb.append("{\n");
        sb.append("  \"schema\": 1,\n");
        sb.append("  \"app_version\": ").append(quote(appVersion)).append(",\n");
        sb.append("  \"session_id\": ").append(quote(sessionId)).append(",\n");
        sb.append("  \"date\": ").append(quote(date)).append(",\n");
        sb.append("  \"bundle_ts_ms\": ").append(System.currentTimeMillis()).append(",\n");
        sb.append("  \"abi\": ").append(quote(primaryAbi())).append(",\n");
        sb.append("  \"supported_abis\": ").append(jsonArray(Build.SUPPORTED_ABIS)).append(",\n");
        sb.append("  \"api_level\": ").append(Build.VERSION.SDK_INT).append(",\n");
        sb.append("  \"android_release\": ").append(quote(Build.VERSION.RELEASE)).append(",\n");
        sb.append("  \"android_codename\": ").append(quote(Build.VERSION.CODENAME)).append(",\n");
        sb.append("  \"android_incremental\": ").append(quote(Build.VERSION.INCREMENTAL)).append(",\n");
        sb.append("  \"device_brand\": ").append(quote(Build.BRAND)).append(",\n");
        sb.append("  \"device_manufacturer\": ").append(quote(Build.MANUFACTURER)).append(",\n");
        sb.append("  \"device_model\": ").append(quote(Build.MODEL)).append(",\n");
        sb.append("  \"device_product\": ").append(quote(Build.PRODUCT)).append(",\n");
        sb.append("  \"device_board\": ").append(quote(Build.BOARD)).append(",\n");
        sb.append("  \"device_hardware\": ").append(quote(Build.HARDWARE)).append(",\n");
        sb.append("  \"build_fingerprint\": ").append(quote(Build.FINGERPRINT)).append(",\n");
        sb.append("  \"log_size_bytes\": ").append(logSize).append("\n");
        sb.append("}\n");
        return sb.toString();
    }

    private static String primaryAbi() {
        if (Build.SUPPORTED_ABIS != null && Build.SUPPORTED_ABIS.length > 0) {
            return Build.SUPPORTED_ABIS[0];
        }
        return "unknown";
    }

    private static String jsonArray(String[] arr) {
        if (arr == null || arr.length == 0) return "[]";
        StringBuilder sb = new StringBuilder(64);
        sb.append("[");
        for (int i = 0; i < arr.length; i++) {
            if (i > 0) sb.append(",");
            sb.append(quote(arr[i]));
        }
        sb.append("]");
        return sb.toString();
    }

    private static String quote(String s) {
        if (s == null) return "null";
        StringBuilder sb = new StringBuilder(s.length() + 2);
        sb.append('"');
        for (int i = 0, n = s.length(); i < n; i++) {
            char c = s.charAt(i);
            switch (c) {
                case '\\': sb.append("\\\\"); break;
                case '"':  sb.append("\\\""); break;
                case '\n': sb.append("\\n");  break;
                case '\r': sb.append("\\r");  break;
                case '\t': sb.append("\\t");  break;
                default:
                    if (c < 0x20) sb.append("?");
                    else sb.append(c);
            }
        }
        sb.append('"');
        return sb.toString();
    }

    private CrashBundleBuilder() {}
}
