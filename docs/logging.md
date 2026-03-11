# Logging

The ANE provides a custom logging interface so you can integrate its log output with your application's logging system.

**Platforms:** Windows, Android, macOS, iOS

## ILogging Interface

```actionscript
package aneAwesomeUtils {
    public interface ILogging {
        function onLog(level:String, message:String):void;
    }
}
```

## Setting a Custom Logger

```actionscript
public function set logging(value:ILogging):void
```

If no custom logger is set, the ANE uses `trace()` by default.

## Example

```actionscript
import aneAwesomeUtils.ILogging;

public class MyLogger implements ILogging {
    public function onLog(level:String, message:String):void {
        var timestamp:String = new Date().toLocaleTimeString();
        trace("[" + timestamp + "] [" + level + "] " + message);

        // Or send to a remote logging service, write to file, etc.
    }
}

// Set the custom logger
AneAwesomeUtils.instance.logging = new MyLogger();
```

## Log Levels

The `level` parameter typically contains:

| Level | Description |
|---|---|
| `"INFO"` | General information |
| `"ERROR"` | Errors and failures |
| `"info"` | Native-level information |
| `"error"` | Native-level errors |
