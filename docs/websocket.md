# WebSocket

The ANE provides binary-mode WebSocket connections with custom headers, connection events, and response header access.

**Platforms:** Windows, Android, macOS, iOS

## Creating a WebSocket

```actionscript
var ws:AneWebSocket = AneAwesomeUtils.instance.createWebSocket();
```

## Connecting

```actionscript
// Simple connection
ws.connect("wss://echo.example.com/ws");

// With custom headers
ws.addHeader("Authorization", "Bearer my-token");
ws.addHeader("X-Custom", "value");
ws.connect("wss://echo.example.com/ws");
```

## Events

| Event | Type | Description |
|---|---|---|
| `"connect"` | `Event` | Connection established |
| `"websocketData"` | `WebSocketEvent` | Binary data received |
| `"close"` | `Event` | Connection closed normally |
| `"ioError"` | `IOErrorEvent` | Connection error |

### Listening for Events

```actionscript
import flash.events.Event;
import flash.events.IOErrorEvent;
import flash.events.WebSocketEvent;
import flash.utils.ByteArray;

ws.addEventListener("connect", function(e:Event):void {
    trace("Connected!");

    // Access response headers after connect
    var headers:Dictionary = ws.getReceivedHeaders();
    trace("Server: " + headers["server"]);
});

ws.addEventListener("websocketData", function(e:WebSocketEvent):void {
    var data:ByteArray = e.data as ByteArray;
    data.position = 0;
    trace("Received " + data.length + " bytes");
});

ws.addEventListener("close", function(e:Event):void {
    trace("Closed with reason: " + ws.closeReason);
    trace("HTTP response code: " + ws.getResponseCode());
});

ws.addEventListener("ioError", function(e:IOErrorEvent):void {
    trace("Error: " + e.text);
});
```

## Sending Messages

WebSocket operates in **binary mode only**. Use `WebSocket.fmtBINARY` as the message type.

```actionscript
import air.net.WebSocket;

// Send binary data
var bytes:ByteArray = new ByteArray();
bytes.writeUTFBytes("Hello, server!");
ws.sendMessage(WebSocket.fmtBINARY, bytes);

// Send structured binary data
var packet:ByteArray = new ByteArray();
packet.writeByte(0x01);           // message type
packet.writeInt(42);              // payload
ws.sendMessage(WebSocket.fmtBINARY, packet);
```

## Closing the Connection

```actionscript
// Normal close (code 1000)
ws.close();

// Close with custom code
ws.close(4000);
```

## Disposing

When you're done with a WebSocket, dispose it to release resources:

```actionscript
ws.dispose();
```

This closes the connection (code 1006) and removes it from the internal registry.

## Managing Headers

```actionscript
// Add headers before connecting
ws.addHeader("Authorization", "Bearer token123");
ws.addHeader("X-App-Version", "2.0");

// Remove a header
ws.removeHeader("X-App-Version");

// Read the current headers dictionary
var myHeaders:Dictionary = ws.headers;
```

## Response Information

After the `"connect"` event:

```actionscript
// Headers received from the server
var serverHeaders:Dictionary = ws.getReceivedHeaders();

// HTTP response code from the handshake
var httpCode:int = ws.getResponseCode();
```

After the `"close"` event:

```actionscript
// WebSocket close reason code
var reason:int = ws.closeReason;
```

## Full Example

```actionscript
if (!AneAwesomeUtils.isSupported) return;
AneAwesomeUtils.instance.initialize();

var ws:AneWebSocket = AneAwesomeUtils.instance.createWebSocket();

ws.addHeader("Authorization", "Bearer my-secret-token");

ws.addEventListener("connect", function(e:Event):void {
    trace("Connected to server");

    var msg:ByteArray = new ByteArray();
    msg.writeUTFBytes('{"action":"subscribe","channel":"updates"}');
    ws.sendMessage(WebSocket.fmtBINARY, msg);
});

ws.addEventListener("websocketData", function(e:WebSocketEvent):void {
    var data:ByteArray = e.data as ByteArray;
    data.position = 0;
    var json:Object = JSON.parse(data.readUTFBytes(data.bytesAvailable));
    trace("Update: " + json.message);
});

ws.addEventListener("close", function(e:Event):void {
    trace("Disconnected: " + ws.closeReason);
});

ws.addEventListener("ioError", function(e:IOErrorEvent):void {
    trace("Connection error: " + e.text);
});

ws.connect("wss://realtime.example.com/ws");
```
