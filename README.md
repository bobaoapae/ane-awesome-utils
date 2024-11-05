
# AneAwesomeUtils

**Extension ID:** `br.com.redesurftank.aneawesomeutils`

AneAwesomeUtils is an Adobe AIR Native Extension (ANE) compatible with Windows 32-bit, Android, macOS, and iOS platforms. It provides an efficient way to load URLs with various HTTP methods, custom headers, or parameters, and offers WebSocket support for real-time communication.

## Key Features

- **Happy Eyeballs (RFC 8305)**: Implements the Happy Eyeballs algorithm to improve connection performance by quickly falling back to the best available IP version (IPv4 or IPv6).
- **HTTP/2 Support**: Utilizes HTTP/2 for faster and more efficient network communication, with a fallback to HTTP/1 if needed.
- **TLS 1.3**: Supports the latest TLS 1.3 protocol for enhanced security and performance.
- **Custom DNS Resolver**: Uses Cloudflare and Google DNS by default, with the ability to configure custom DNS servers for flexibility.
- **WebSocket Support**: Allows you to create and manage WebSocket connections, send and receive binary data, and handle connection events such as open, close, and error.

## Supported Platforms

- Windows 32-bit (7 SP1+)
- Android (API 22+)
- macOS (10.12+)
- iOS (12.2+)

## Installation

Ensure that you have included the `AneAwesomeUtils` ANE in your Adobe AIR project. Update your application descriptor XML to include the extension ID `br.com.redesurftank.aneawesomeutils` and necessary permissions for each platform.

## Initialization

Before using AneAwesomeUtils, check if the extension is supported on the current platform and initialize it only once during your application lifecycle.

### Step-by-Step Initialization

1. **Check if the extension is supported:**

   ```actionscript
   if (AneAwesomeUtils.isSupported) {
       trace("AneAwesomeUtils is supported on this platform.");
   } else {
       trace("AneAwesomeUtils is not supported on this platform.");
   }
   ```

2. **Initialize the extension (do this once, typically in your app startup code):**

   ```actionscript
   var initialized:Boolean = false;
   
   if (AneAwesomeUtils.isSupported) {
       initialized = AneAwesomeUtils.instance.initialize();
       if (initialized) {
           trace("AneAwesomeUtils initialized successfully.");
       } else {
           trace("Failed to initialize AneAwesomeUtils.");
       }
   }
   ```

## Static Host Resolution

You can use the `addStaticHostResolution` method to manually add a list of IP addresses for a specific host. This feature is useful for both URL loading and WebSocket connections.

### Example: Adding Static Host Resolutions

```actionscript
// Ensure AneAwesomeUtils is initialized and ready to use
if (initialized) {
    // Add multiple IPs for a specific host
    AneAwesomeUtils.instance.addStaticHostResolution("example.com", "192.168.0.1");
    AneAwesomeUtils.instance.addStaticHostResolution("example.com", "192.168.0.2");
    AneAwesomeUtils.instance.addStaticHostResolution("example.com", "192.168.0.3");

    trace("Static host resolutions added for example.com.");
}
```

## URL Loading Usage

After initialization, you can use `AneAwesomeUtils` to load URLs with different HTTP methods and configurations.

### Loading a URL

To load a URL, use the `loadUrl` method from the `AneAwesomeUtils` instance.

#### `loadUrl` Method Signature

```actionscript
native public function loadUrl(
    url:String, 
    method:String = "GET", 
    variables:Object = null, 
    headers:Object = null, 
    onResult:Function = null, 
    onError:Function = null, 
    onProgress:Function = null
):void;
```

#### Parameters

- **url**: The URL to load.
- **method**: The HTTP method to use (default is "GET").
- **variables**: An object containing variables to send with the request (optional).
- **headers**: An object containing custom headers to send with the request (optional).
- **onResult**: A callback function that will be called upon a successful response. The response is passed as a `ByteArray`.
- **onError**: A callback function that will be called if an error occurs. The error is passed as an `Error` object.
- **onProgress**: A callback function that will be called to report the progress of the loading. Progress is passed as a `Number` representing the percentage completed.

### Example Code for URL Loading

Below is an example demonstrating how to use `AneAwesomeUtils` to load a URL after the extension has been initialized:

```actionscript
// Initialize AneAwesomeUtils once in your application
var initialized:Boolean = false;

if (AneAwesomeUtils.isSupported) {
    initialized = AneAwesomeUtils.instance.initialize();
    if (initialized) {
        trace("AneAwesomeUtils initialized successfully.");
    } else {
        trace("Failed to initialize AneAwesomeUtils.");
    }
} else {
    trace("AneAwesomeUtils is not supported on this platform.");
}

// Usage example after initialization
if (initialized) {
    // Define the URL, HTTP method, and optional headers and variables
    var url:String = "https://example.com/api/data";
    var method:String = "POST";
    var variables:Object = { key1: "value1", key2: "value2" };
    var headers:Object = { "Content-Type": "application/json" };

    // Load the URL with specified parameters and handle responses
    AneAwesomeUtils.instance.loadUrl(
        url,
        method,
        variables,
        headers,
        function(response:ByteArray):void {
            trace("Success: " + response.toString());
        },
        function(error:Error):void {
            trace("Error: " + error.message);
        },
        function(progress:Number):void {
            trace("Progress: " + progress + "% completed.");
        }
    );
}
```

## WebSocket Usage

AneAwesomeUtils also supports WebSocket connections in binary mode only. You can create, send messages, and handle events using WebSocket functionality.

### WebSocket Example

Below is an example demonstrating how to create and use a WebSocket connection:

```actionscript
// Initialize AneAwesomeUtils once in your application
var initialized:Boolean = false;

if (AneAwesomeUtils.isSupported) {
    initialized = AneAwesomeUtils.instance.initialize();
    if (initialized) {
        trace("AneAwesomeUtils initialized successfully.");
    } else {
        trace("Failed to initialize AneAwesomeUtils.");
    }
}

if (initialized) {
    // Create a new WebSocket connection
    var ws:AneWebSocket = AneAwesomeUtils.instance.createWebSocket();
    
    // Connect to the WebSocket server
    ws.connect("wss://example.com/socket");

    // Prepare binary data to send
    var sendBytes:ByteArray = new ByteArray();
    sendBytes.writeUTFBytes("Hello in binary!");

    // Send the binary message
    ws.sendMessage(WebSocket.fmtBINARY, sendBytes);

    // Handle WebSocket events
    ws.addEventListener("connect", function(event:Event):void {
        trace("Connected to WebSocket server.");
    });
    
    ws.addEventListener("websocketData", function(event:WebSocketEvent):void {
        var data:ByteArray = event.data as ByteArray;
        trace("Received data: " + data.toString());
    });
    
    ws.addEventListener("close", function(event:Event):void {
        trace("WebSocket connection closed.");
    });
    
    ws.addEventListener("ioError", function(event:IOErrorEvent):void {
        trace("WebSocket error: " + event.text);
    });
    
    // Close the WebSocket connection
    ws.close();
}
```

## Notes

- Always check if the extension is supported on the current platform using `AneAwesomeUtils.isSupported`.
- Initialize the extension only once before using other methods.
- Ensure your Adobe AIR application descriptor XML file is correctly configured for each platform with the required permissions and extension ID.

## Troubleshooting

If you encounter issues:

1. **Check platform compatibility**: Ensure the ANE is supported on your target platform.
2. **Review initialization**: Make sure `initialize()` is called and returns `true`.
3. **Inspect callback functions**: Ensure your callback functions handle errors and results appropriately.

For further assistance, consult the official documentation or contact support.
