# mTLS Client Certificates

The ANE supports mutual TLS (mTLS) authentication by allowing you to register client certificates for specific hosts. Once registered, all HTTP and WebSocket connections to that host will use the client certificate during the TLS handshake.

**Platforms:** Windows, Android, macOS, iOS

## addClientCertificate

```actionscript
public function addClientCertificate(host:String, p12Data:String, password:String):void
```

### Parameters

| Parameter | Type | Description |
|---|---|---|
| `host` | `String` | The hostname to associate the certificate with (e.g., `"api.example.com"`) |
| `p12Data` | `String` | Base64-encoded PKCS#12 (.p12 / .pfx) certificate data |
| `password` | `String` | Password for the PKCS#12 file |

### Example

```actionscript
import flash.filesystem.File;
import flash.filesystem.FileStream;
import flash.utils.ByteArray;

// Load the .p12 certificate file
var certFile:File = File.applicationDirectory.resolvePath("client-cert.p12");
var stream:FileStream = new FileStream();
stream.open(certFile, "read");
var certBytes:ByteArray = new ByteArray();
stream.readBytes(certBytes);
stream.close();

// Base64-encode the certificate
import aneAwesomeUtils.Base64;
var certBase64:String = Base64.encode(certBytes);

// Register the certificate for a specific host
AneAwesomeUtils.instance.addClientCertificate(
    "secure-api.example.com",
    certBase64,
    "my-cert-password"
);

// All subsequent requests to this host will use the client certificate
AneAwesomeUtils.instance.loadUrl(
    "https://secure-api.example.com/protected",
    "GET",
    null, null,
    function(data:ByteArray):void {
        trace("mTLS request succeeded");
    },
    function(error:Error):void {
        trace("mTLS request failed: " + error.message);
    }
);
```

### With WebSocket

```actionscript
// Register cert first
AneAwesomeUtils.instance.addClientCertificate(
    "wss-server.example.com",
    certBase64,
    "password"
);

// WebSocket connections to this host will also use the certificate
var ws:AneWebSocket = AneAwesomeUtils.instance.createWebSocket();
ws.connect("wss://wss-server.example.com/ws");
```

## Notes

- Certificates are registered per-host and persist for the lifetime of the ANE context.
- The PKCS#12 data must be Base64-encoded before passing to `addClientCertificate`.
- The certificate is used for both HTTP (loadUrl) and WebSocket connections to the registered host.
- Call `addClientCertificate` before making requests to the target host.
