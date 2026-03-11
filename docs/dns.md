# Static DNS / Host Resolution

Override DNS resolution for specific hostnames by mapping them to fixed IP addresses. This affects both HTTP and WebSocket connections.

**Platforms:** Windows, Android, macOS, iOS

## addStaticHostResolution

```actionscript
public function addStaticHostResolution(host:String, ip:String):void
```

Maps a hostname to a specific IP address. You can call this multiple times for the same host to add multiple IPs.

### Parameters

| Parameter | Type | Description |
|---|---|---|
| `host` | `String` | The hostname to resolve (e.g., `"api.example.com"`) |
| `ip` | `String` | The IP address to resolve to (IPv4 or IPv6) |

## removeStaticHostResolution

```actionscript
public function removeStaticHostResolution(host:String):void
```

Removes all static IP mappings for the given hostname, reverting to normal DNS resolution.

### Parameters

| Parameter | Type | Description |
|---|---|---|
| `host` | `String` | The hostname to clear |

## Examples

### Single IP Override

```actionscript
// Force api.example.com to resolve to a specific IP
AneAwesomeUtils.instance.addStaticHostResolution("api.example.com", "10.0.1.50");

// Requests will now go to 10.0.1.50
AneAwesomeUtils.instance.loadUrl("https://api.example.com/data", "GET",
    null, null, onResult, onError);
```

### Multiple IPs (Load Balancing)

```actionscript
// Add multiple IPs for the same host
AneAwesomeUtils.instance.addStaticHostResolution("api.example.com", "10.0.1.50");
AneAwesomeUtils.instance.addStaticHostResolution("api.example.com", "10.0.1.51");
AneAwesomeUtils.instance.addStaticHostResolution("api.example.com", "10.0.1.52");

// The Happy Eyeballs algorithm will race connections to all IPs
```

### Removing a Static Host

```actionscript
// Remove static resolution, revert to DNS
AneAwesomeUtils.instance.removeStaticHostResolution("api.example.com");
```

## Use Cases

- **Testing** - Point production hostnames to staging/local servers
- **Failover** - Hardcode fallback IPs for critical services
- **Performance** - Skip DNS lookup latency for known hosts
- **DNS Pinning** - Lock connections to specific servers
