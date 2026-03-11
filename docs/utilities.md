# Utilities

Cross-platform utility functions available on all platforms.

## Decompression

Decompress data (zlib/deflate) from one ByteArray into another.

**Platforms:** Windows, Android, macOS, iOS

```actionscript
public function decompressByteArray(source:ByteArray, target:ByteArray):void
```

### Parameters

| Parameter | Type | Description |
|---|---|---|
| `source` | `ByteArray` | Compressed data |
| `target` | `ByteArray` | ByteArray to receive decompressed data (length will be set automatically) |

### Example

```actionscript
var compressed:ByteArray = getCompressedData(); // from server, file, etc.
var decompressed:ByteArray = new ByteArray();

AneAwesomeUtils.instance.decompressByteArray(compressed, decompressed);

decompressed.position = 0;
trace("Decompressed size: " + decompressed.length);
```

---

## File I/O

Read a file from the filesystem directly into a ByteArray, bypassing AIR's file APIs.

**Platforms:** Windows, Android, macOS, iOS

```actionscript
public function readFileToByteArray(path:String, target:ByteArray):void
```

### Parameters

| Parameter | Type | Description |
|---|---|---|
| `path` | `String` | Full filesystem path to the file |
| `target` | `ByteArray` | ByteArray to receive the file contents (position will be reset to 0) |

### Example

```actionscript
import flash.filesystem.File;

var file:File = File.applicationStorageDirectory.resolvePath("data.bin");
var bytes:ByteArray = new ByteArray();

AneAwesomeUtils.instance.readFileToByteArray(file.nativePath, bytes);

trace("File size: " + bytes.length + " bytes");
```

---

## XML to Object

Parse an XML string into a native ActionScript object tree. Attributes become properties, repeated child elements become arrays, and scalar values are automatically typed (Boolean, int, uint, Number, String).

**Platforms:** Windows, Android, macOS, iOS

```actionscript
public function mapXmlToObject(xmlString:String):Object
```

### Parameters

| Parameter | Type | Description |
|---|---|---|
| `xmlString` | `String` | XML document as a string |

### Example

```actionscript
var xml:String = '<user name="John" age="30" active="true">' +
                 '  <role>admin</role>' +
                 '  <role>editor</role>' +
                 '</user>';

var obj:Object = AneAwesomeUtils.instance.mapXmlToObject(xml);

trace(obj.name);    // "John"
trace(obj.age);     // 30 (Number)
trace(obj.active);  // true (Boolean)
trace(obj.role);    // Array with 2 elements
```

### Type Conversion

The parser automatically converts scalar values:

| XML Value | ActionScript Type |
|---|---|
| `"true"` / `"false"` | `Boolean` |
| `"123"` (fits int32) | `int` |
| `"4294967295"` (fits uint32) | `uint` |
| `"3.14"` | `Number` |
| Anything else | `String` |

### Array Handling

When multiple child elements share the same tag name, they are grouped into an Array:

```xml
<config>
    <server host="a.example.com" port="8080"/>
    <server host="b.example.com" port="8081"/>
</config>
```

```actionscript
var cfg:Object = AneAwesomeUtils.instance.mapXmlToObject(xmlString);
trace(cfg.server.length);       // 2
trace(cfg.server[0].host);      // "a.example.com"
trace(cfg.server[1].port);      // 8081
```

---

## Device Unique ID

Returns a hardware-based unique identifier for the device.

**Platforms:** Windows, Android, macOS, iOS

```actionscript
public function getDeviceUniqueId():String
```

### Example

```actionscript
var deviceId:String = AneAwesomeUtils.instance.getDeviceUniqueId();
trace("Device ID: " + deviceId);
```

### Implementation Details

| Platform | Source |
|---|---|
| Windows | Processor serial, disk ID, and other hardware identifiers (hashed) |
| Android | Combination of device properties |
| macOS/iOS | System-based unique identifier |

---

## Emulator / VM Detection

Detect if the application is running inside an emulator or virtual machine.

**Platforms:** Windows (VM detection), Android (emulator detection)

### Synchronous

```actionscript
public function isRunningOnEmulator():Boolean
```

Returns `true` if a VM/emulator is detected. Returns `false` on macOS/iOS (not implemented).

```actionscript
if (AneAwesomeUtils.instance.isRunningOnEmulator()) {
    trace("Running in a virtual environment!");
}
```

### Asynchronous (Android only)

```actionscript
public function isRunningOnEmulatorAsync(callback:Function):void
```

Performs asynchronous emulator detection. On non-Android platforms, the callback fires immediately with `false`.

```actionscript
AneAwesomeUtils.instance.isRunningOnEmulatorAsync(function(isEmulator:Boolean):void {
    if (isEmulator) {
        trace("Emulator detected");
    }
});
```
