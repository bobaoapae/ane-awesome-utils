# Getting Started

## Installation

1. Copy `br.com.redesurftank.aneawesomeutils.ane` into your AIR project's `extensions/` directory (or wherever you keep ANEs).

2. Add the extension ID to your application descriptor XML:

```xml
<extensions>
    <extensionID>br.com.redesurftank.aneawesomeutils</extensionID>
</extensions>
```

### Android Permissions

For Android, add the following to your application descriptor:

```xml
<android>
    <manifestAdditions><![CDATA[
        <manifest android:installLocation="auto">
            <uses-permission android:name="android.permission.INTERNET"/>
            <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE"/>
            <!-- Only if using battery optimization features -->
            <uses-permission android:name="android.permission.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS"/>
        </manifest>
    ]]></manifestAdditions>
</android>
```

## Initialization

Initialize the ANE **once** during your application startup:

```actionscript
import AneAwesomeUtils;

// Check platform support
if (!AneAwesomeUtils.isSupported) {
    trace("AneAwesomeUtils is not supported on this platform.");
    return;
}

// Initialize
var initialized:Boolean = AneAwesomeUtils.instance.initialize();
if (!initialized) {
    trace("Failed to initialize AneAwesomeUtils.");
    return;
}

trace("AneAwesomeUtils ready.");
```

You can check the initialization state at any time:

```actionscript
if (AneAwesomeUtils.instance.successInit) {
    // ANE is ready
}
```

## Cleanup

When your application closes, dispose of the ANE:

```actionscript
AneAwesomeUtils.instance.dispose();
```

This will:
- Close all active WebSocket connections
- Cancel pending HTTP requests
- Release native resources

## Platform Detection

The ANE supports Windows, Android, macOS, and iOS. The `isSupported` property checks the current platform automatically. Some features are platform-specific — calling a Windows-only method on Android will be silently ignored or return a default value.

## Next Steps

- [HTTP Client](http-client.md) - Make HTTP requests
- [WebSocket](websocket.md) - Real-time binary communication
- [Utilities](utilities.md) - Compression, file I/O, device ID
- [Windows Features](platform-windows.md) - Windows-specific functionality
- [Android Features](platform-android.md) - Android-specific functionality
