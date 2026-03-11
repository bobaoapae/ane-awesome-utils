# Android-Specific Features

Features available only on Android. Calling these methods on other platforms is safe — they are silently ignored or return default values.

## Battery Optimization

Android aggressively kills background apps to save battery. These methods allow you to check and request exclusion from battery optimization, which is important for apps that need persistent network connections.

### Check Status

```actionscript
public function isBatteryOptimizationIgnored():Boolean
```

Returns `true` if the app is already excluded from battery optimization. Returns `true` on non-Android platforms.

### Request Exclusion

```actionscript
public function requestBatteryOptimizationExclusion():void
```

Opens the system dialog asking the user to exclude the app from battery optimization. The user must approve manually.

### Example

```actionscript
if (!AneAwesomeUtils.instance.isBatteryOptimizationIgnored()) {
    // Show the user a dialog explaining why this is needed, then:
    AneAwesomeUtils.instance.requestBatteryOptimizationExclusion();
}
```

> **Note:** Requires the `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` permission in your Android manifest.

---

## Connection Configuration

Configure timeouts and keep-alive intervals for HTTP and WebSocket connections on Android.

```actionscript
public function configureConnection(
    pingInterval:int,
    connectTimeout:int,
    readTimeout:int,
    writeTimeout:int
):void
```

### Parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `pingInterval` | `int` | `30` | WebSocket keep-alive ping interval in seconds |
| `connectTimeout` | `int` | `10` | Connection timeout in seconds |
| `readTimeout` | `int` | `30` | Read timeout in seconds |
| `writeTimeout` | `int` | `30` | Write timeout in seconds |

### Example

```actionscript
// Aggressive timeouts for a game
AneAwesomeUtils.instance.configureConnection(
    15,  // ping every 15 seconds
    5,   // connect timeout: 5 seconds
    10,  // read timeout: 10 seconds
    10   // write timeout: 10 seconds
);
```

---

## Release Connection Resources

Release wake locks and connection resources held by the ANE on Android.

```actionscript
public function releaseConnectionResources():void
```

### Example

```actionscript
// When the app is going to background or shutting down
AneAwesomeUtils.instance.releaseConnectionResources();
```

---

## Network State Monitoring

Monitor network connectivity changes on Android.

### Properties

```actionscript
public function get networkAvailable():Boolean
```

Returns the current network availability status. Defaults to `true`.

### Listeners

```actionscript
public function addNetworkStateChangeListener(listener:Function):void
public function removeNetworkStateChangeListener(listener:Function):void
```

### Example

```actionscript
import flash.events.StatusEvent;

// Check current state
if (AneAwesomeUtils.instance.networkAvailable) {
    trace("Network is available");
}

// Listen for changes
AneAwesomeUtils.instance.addNetworkStateChangeListener(
    function(event:StatusEvent):void {
        var isAvailable:Boolean = (event.code == "available");
        trace("Network state changed: " + (isAvailable ? "online" : "offline"));

        if (!isAvailable) {
            // Show offline indicator, queue requests, etc.
        }
    }
);

// Stop listening
// AneAwesomeUtils.instance.removeNetworkStateChangeListener(myListener);
```

> **Note:** On non-Android platforms, `networkAvailable` always returns `true` and the listeners are never fired.
