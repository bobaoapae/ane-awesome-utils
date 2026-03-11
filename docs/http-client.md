# HTTP Client

The HTTP client supports all standard HTTP methods with HTTP/2, TLS 1.3, and the Happy Eyeballs algorithm (RFC 8305) for fast dual-stack connections.

**Platforms:** Windows, Android, macOS, iOS

## loadUrl

```actionscript
public function loadUrl(
    url:String,
    method:String = "GET",
    variables:Object = null,
    headers:Object = null,
    onResult:Function = null,
    onError:Function = null,
    onProgress:Function = null
):void
```

### Parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `url` | `String` | *(required)* | The URL to request. Query string parameters are automatically parsed and merged with `variables`. |
| `method` | `String` | `"GET"` | HTTP method: `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, etc. |
| `variables` | `Object` | `null` | Key-value pairs sent as request body (JSON-encoded). Can also be a `URLVariables` or `Dictionary`. |
| `headers` | `Object` | `null` | Key-value pairs for custom HTTP headers. Can also be a `Dictionary`. |
| `onResult` | `Function` | `null` | Callback on success: `function(data:ByteArray):void` |
| `onError` | `Function` | `null` | Callback on error: `function(error:Error):void` |
| `onProgress` | `Function` | `null` | Callback for progress: `function(currentBytes:Number, totalBytes:Number):void` |

### Basic GET Request

```actionscript
AneAwesomeUtils.instance.loadUrl(
    "https://api.example.com/users",
    "GET",
    null,
    null,
    function(data:ByteArray):void {
        data.position = 0;
        var json:Object = JSON.parse(data.readUTFBytes(data.bytesAvailable));
        trace("Users: " + json.length);
    },
    function(error:Error):void {
        trace("Request failed: " + error.message);
    }
);
```

### POST with JSON Body

```actionscript
var body:Object = {
    username: "john",
    email: "john@example.com"
};

var headers:Object = {
    "Content-Type": "application/json"
};

AneAwesomeUtils.instance.loadUrl(
    "https://api.example.com/users",
    "POST",
    body,
    headers,
    function(data:ByteArray):void {
        trace("User created successfully");
    },
    function(error:Error):void {
        trace("Error: " + error.message);
    }
);
```

### Request with Progress Tracking

```actionscript
AneAwesomeUtils.instance.loadUrl(
    "https://cdn.example.com/large-file.zip",
    "GET",
    null,
    null,
    function(data:ByteArray):void {
        trace("Download complete: " + data.length + " bytes");
    },
    function(error:Error):void {
        trace("Download failed: " + error.message);
    },
    function(currentBytes:Number, totalBytes:Number):void {
        var pct:Number = (totalBytes > 0) ? (currentBytes / totalBytes * 100) : 0;
        trace("Progress: " + pct.toFixed(1) + "%");
    }
);
```

### Using URLVariables

```actionscript
import flash.net.URLVariables;

var vars:URLVariables = new URLVariables();
vars.page = "1";
vars.limit = "20";

AneAwesomeUtils.instance.loadUrl(
    "https://api.example.com/items",
    "GET",
    vars,
    null,
    function(data:ByteArray):void {
        // handle response
    }
);
```

### Query String Merging

If the URL contains query parameters, they are automatically merged with the `variables` object:

```actionscript
// The query string "page=1" is merged with the variables object
AneAwesomeUtils.instance.loadUrl(
    "https://api.example.com/items?page=1",
    "GET",
    { limit: "20" },  // merged: page=1&limit=20
    null,
    onResult
);
```

## Networking Features

### Happy Eyeballs (RFC 8305)

Connections automatically race IPv4 and IPv6, picking whichever responds first. No configuration needed.

### HTTP/2

HTTP/2 is used by default when the server supports it, with automatic fallback to HTTP/1.1.

### TLS 1.3

TLS 1.3 is the default when the server supports it, providing better security and faster handshakes.

### Custom DNS

The ANE uses Cloudflare (`1.1.1.1`) and Google (`8.8.8.8`) DNS by default. You can override DNS for specific hosts using [Static Host Resolution](dns.md).
