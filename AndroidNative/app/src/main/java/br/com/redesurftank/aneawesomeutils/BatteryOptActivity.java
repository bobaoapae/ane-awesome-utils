package br.com.redesurftank.aneawesomeutils;

import static br.com.redesurftank.aneawesomeutils.AneAwesomeUtilsExtension.TAG;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.provider.Settings;

/**
 * Transparent wrapper Activity that launches battery optimization settings.
 *
 * 1. AIR's immersive mode dismisses system dialogs (Google Issue Tracker 36992828),
 *    so we use a separate transparent activity to isolate from AIR's focus management.
 *
 * 2. OEMs (Oppo/OnePlus/Realme) silently cancel ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS.
 *    We detect this (result in < 500ms) and fall back to the app's own settings page
 *    (ACTION_APPLICATION_DETAILS_SETTINGS) where the user can tap Battery > Unrestricted.
 */
public class BatteryOptActivity extends Activity {

    private static final int REQUEST_CODE_STANDARD = 12321;
    private static final int REQUEST_CODE_APP_SETTINGS = 12322;
    private long _launchTime;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        AneAwesomeUtilsLogging.i(TAG, "BatteryOptActivity: onCreate, manufacturer=" + Build.MANUFACTURER);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            _launchTime = SystemClock.elapsedRealtime();
            Intent intent = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS);
            intent.setData(Uri.parse("package:" + getPackageName()));
            if (intent.resolveActivity(getPackageManager()) != null) {
                AneAwesomeUtilsLogging.i(TAG, "BatteryOptActivity: launching standard battery optimization dialog");
                startActivityForResult(intent, REQUEST_CODE_STANDARD);
            } else {
                AneAwesomeUtilsLogging.w(TAG, "BatteryOptActivity: standard intent not available");
                launchAppSettings();
            }
        } else {
            AneAwesomeUtilsLogging.i(TAG, "BatteryOptActivity: API < 23, finishing");
            finish();
        }
    }

    private void launchAppSettings() {
        AneAwesomeUtilsLogging.i(TAG, "BatteryOptActivity: opening app settings page for " + getPackageName());
        Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
        intent.setData(Uri.parse("package:" + getPackageName()));
        startActivityForResult(intent, REQUEST_CODE_APP_SETTINGS);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        long elapsed = SystemClock.elapsedRealtime() - _launchTime;
        AneAwesomeUtilsLogging.i(TAG, "BatteryOptActivity: onActivityResult, requestCode=" + requestCode
                + ", resultCode=" + resultCode + ", elapsed=" + elapsed + "ms");

        if (requestCode == REQUEST_CODE_STANDARD && resultCode == RESULT_CANCELED && elapsed < 500) {
            // OEM silently cancelled the standard dialog. Open app settings page instead.
            AneAwesomeUtilsLogging.w(TAG, "BatteryOptActivity: OEM cancelled standard dialog in " + elapsed
                    + "ms, opening app settings");
            launchAppSettings();
            return;
        }

        finish();
    }
}
