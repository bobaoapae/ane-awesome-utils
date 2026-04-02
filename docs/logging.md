# Logging

The ANE provides two logging systems: a custom AS3 logging interface for internal ANE messages, and a full native logging pipeline with file persistence, rotation, crash detection, and cross-ANE sharing.

**Platforms:** Windows, Android, macOS, iOS

---

## Native Logging

The native logging system writes structured log files to disk with automatic 7-day rotation, unexpected shutdown detection, and platform-specific system log integration.

### Initialization

```actionscript
// Initialize logging with a profile name
// Windows/macOS: separates logs by profile (multi-account support)
// Android/iOS: profile is always "default" regardless of argument
var logPath:String = AneAwesomeUtils.instance.initLog("myProfile");
trace("Logs at: " + logPath);
```

**Returns** the full path to the log directory.

**On init, the system:**
1. Creates the log directory if needed
2. Checks for unexpected shutdown (previous session didn't close cleanly)
3. Deletes log files older than 7 days
4. Opens today's log file in append mode

### Writing Log Messages

```actionscript
AneAwesomeUtils.instance.writeLogMessage("INFO", "MyModule", "Player connected");
AneAwesomeUtils.instance.writeLogMessage("ERROR", "Network", "Connection timeout");
AneAwesomeUtils.instance.writeLogMessage("DEBUG", "GameLoop", "Frame delta: 16ms");
AneAwesomeUtils.instance.writeLogMessage("WARN", "Memory", "High allocation rate");
```

**Log levels:** `DEBUG`, `INFO`, `WARN`, `ERROR`

Each message is written to file and also output to the platform's system log:

| Platform | System Log | File Log |
|---|---|---|
| Windows | stdout | Yes |
| Android | android.util.Log (logcat) | Yes |
| macOS | os_log | Yes |
| iOS | os_log | Yes |

### Log File Format

```
[2026-04-01 14:30:45] [INFO] [MyModule] Player connected
[2026-04-01 14:30:46] [ERROR] [Network] Connection timeout
```

### Log File Structure

```
<base_dir>/ane-awesome-utils-logs/<profile>/
    .session_active              # Marker file (exists while app is running)
    ane-log-2026-03-25.txt       # One file per day
    ane-log-2026-03-26.txt
    ane-log-2026-04-01.txt       # Today's log
```

**Base directory per platform:**
- **Windows:** Application directory (same folder as the AIR app executable)
- **Android:** `context.getFilesDir()`
- **macOS:** Current working directory
- **iOS:** Current working directory

### Unexpected Shutdown Detection

The logging system tracks session state using a `.session_active` marker file. If the app crashes or is killed, the marker remains on disk. On the next `initLog()`, the ANE detects this and fires a callback with information about the old log files.

```actionscript
// Set the callback BEFORE calling initLog()
AneAwesomeUtils.instance.onUnexpectedShutdown = function(oldLogsJson:String):void {
    trace("Previous session crashed! Old logs: " + oldLogsJson);
    
    // oldLogsJson is a JSON array:
    // [{"date":"2026-03-31","size":4523,"path":"/full/path/ane-log-2026-03-31.txt"}, ...]
    
    var logs:Array = JSON.parse(oldLogsJson) as Array;
    for each (var log:Object in logs) {
        trace("  " + log.date + " (" + log.size + " bytes)");
    }
    
    // You can read the old logs, upload them, or delete them
    AneAwesomeUtils.instance.readLogFile(logs[0].date, function(data:ByteArray):void {
        // Upload crash log to server...
    });
};

// Now initialize - if crash is detected, callback fires
AneAwesomeUtils.instance.initLog("default");
```

### Listing Log Files

```actionscript
var json:String = AneAwesomeUtils.instance.getLogFileList();
// Returns: [{"date":"2026-03-25","size":1234,"path":"/full/path/..."}, ...]

var files:Array = JSON.parse(json) as Array;
for each (var f:Object in files) {
    trace(f.date + " - " + f.size + " bytes");
}
```

### Reading Log Files (Async)

```actionscript
// Read a specific day's log
AneAwesomeUtils.instance.readLogFile("2026-04-01",
    function(data:ByteArray):void {
        trace("Log content: " + data.toString());
    },
    function(error:Error):void {
        trace("Read failed: " + error.message);
    }
);

// Read ALL logs concatenated (pass null for date)
AneAwesomeUtils.instance.readLogFile(null,
    function(data:ByteArray):void {
        // data contains all log files concatenated, sorted by date
        uploadToServer(data);
    }
);
```

### Deleting Log Files

```actionscript
// Delete a specific day's log
AneAwesomeUtils.instance.deleteLogFile("2026-03-25");

// Delete ALL log files
AneAwesomeUtils.instance.deleteLogFile();
```

### Profile Separation (Windows/macOS)

On Windows and macOS, you can use different profiles to separate logs by account:

```actionscript
// Each profile gets its own log directory
AneAwesomeUtils.instance.initLog("account_123");
// Logs go to: <app>/ane-awesome-utils-logs/account_123/

// Later, switch to different account
AneAwesomeUtils.instance.initLog("account_456");
// Logs go to: <app>/ane-awesome-utils-logs/account_456/
```

On Android and iOS, the profile is always `"default"` regardless of the argument.

---

## Cross-ANE Shared Logging

The native logging system supports shared logging across multiple ANEs in the same application.

### Android: SharedAneLogger

This ANE owns the `com.adobe.air.SharedAneLogger` singleton. Other ANEs can access it via reflection to write to the same log file:

```java
// From another ANE's Java code:
try {
    Class<?> cls = Class.forName("com.adobe.air.SharedAneLogger");
    Method getInstance = cls.getMethod("getInstance");
    Object instance = getInstance.invoke(null);
    Method getLogger = cls.getMethod("getLogger");
    Logger logger = (Logger) getLogger.invoke(instance);
    logger.log(Level.INFO, "MyOtherANE: something happened");
} catch (Exception e) {
    // SharedAneLogger not available (ane-awesome-utils not loaded)
}
```

### Windows: Exported Functions

Other ANEs can call exported functions from the DLL:

```cpp
// From another ANE's C++ code:
typedef void (*SharedLogWrite)(const char*, const char*, const char*);
typedef const char* (*SharedLogGetPath)();

HMODULE mod = GetModuleHandleA("AneAwesomeUtilsWindows.dll");
if (mod) {
    auto write = (SharedLogWrite)GetProcAddress(mod, "AneAwesomeUtils_SharedLog_Write");
    auto getPath = (SharedLogGetPath)GetProcAddress(mod, "AneAwesomeUtils_SharedLog_GetPath");
    
    if (write) write("INFO", "MyOtherANE", "something happened");
    if (getPath) printf("Shared log at: %s\n", getPath());
}
```

### macOS/iOS: Exported Symbols

```c
// From another ANE's C/Objective-C code:
extern void AneAwesomeUtils_SharedLog_Write(const char*, const char*, const char*) __attribute__((weak));
extern const char* AneAwesomeUtils_SharedLog_GetPath(void) __attribute__((weak));

if (AneAwesomeUtils_SharedLog_Write) {
    AneAwesomeUtils_SharedLog_Write("INFO", "MyOtherANE", "something happened");
}
```

Or via `dlsym`:
```c
void (*write)(const char*, const char*, const char*) = dlsym(RTLD_DEFAULT, "AneAwesomeUtils_SharedLog_Write");
if (write) write("INFO", "MyOtherANE", "something happened");
```

---

## ILogging Interface (AS3-level)

Separately from the native logging, the ANE provides a custom AS3 logging interface for internal ANE messages (WebSocket events, HTTP errors, etc.):

```actionscript
import aneAwesomeUtils.ILogging;

public class MyLogger implements ILogging {
    public function onLog(level:String, message:String):void {
        trace("[" + level + "] " + message);
    }
}

AneAwesomeUtils.instance.logging = new MyLogger();
```

If no custom logger is set, the ANE uses `trace()` by default. This is independent from the native file logging system.

---

## API Reference

| Method | Description |
|---|---|
| `initLog(profile:String = "default"):String` | Initialize logging, returns log directory path |
| `writeLogMessage(level:String, tag:String, message:String):void` | Write a log entry |
| `getLogFileList():String` | Get JSON array of log files |
| `readLogFile(date:String, onResult:Function, onError:Function):void` | Async read log as ByteArray |
| `deleteLogFile(date:String):Boolean` | Delete log file(s) |
| `set onUnexpectedShutdown(callback:Function):void` | Set crash detection callback |
| `set logging(value:ILogging):void` | Set AS3-level log handler |
