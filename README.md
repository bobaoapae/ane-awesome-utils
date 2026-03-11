# AneAwesomeUtils

**Extension ID:** `br.com.redesurftank.aneawesomeutils`

AneAwesomeUtils is an Adobe AIR Native Extension (ANE) that provides advanced networking, security, device utilities, and platform-specific features for Windows, Android, macOS, and iOS.

## Features

| Feature | Windows | Android | macOS | iOS |
|---|:---:|:---:|:---:|:---:|
| [HTTP Client](docs/http-client.md) (HTTP/2, TLS 1.3, Happy Eyeballs) | x | x | x | x |
| [WebSocket](docs/websocket.md) (binary mode) | x | x | x | x |
| [mTLS Client Certificates](docs/mtls.md) | x | x | x | x |
| [Static DNS / Host Resolution](docs/dns.md) | x | x | x | x |
| [Compression / Decompression](docs/utilities.md#decompression) | x | x | x | x |
| [File I/O](docs/utilities.md#file-io) | x | x | x | x |
| [XML to Object Mapping](docs/utilities.md#xml-to-object) | x | x | x | x |
| [Device Unique ID](docs/utilities.md#device-unique-id) | x | x | x | x |
| [Emulator / VM Detection](docs/utilities.md#emulator--vm-detection) | x | x | | |
| [Network State Monitoring](docs/platform-android.md#network-state-monitoring) | | x | | |
| [Screen Capture Prevention](docs/platform-windows.md#screen-capture-prevention) | x | | | |
| [Input Filtering (anti-cheat)](docs/platform-windows.md#input-filtering) | x | | | |
| [Speed Hack Detection](docs/platform-windows.md#speed-hack-detection) | x | | | |
| [Audio Safety Hook](docs/platform-windows.md#audio-safety-hook) | x | | | |
| [Battery Optimization](docs/platform-android.md#battery-optimization) | | x | | |
| [Connection Configuration](docs/platform-android.md#connection-configuration) | | x | | |

## Supported Platforms

| Platform | Min Version | Architecture |
|---|---|---|
| Windows | 7 SP1 | x86, x86-64 |
| Android | API 22 (5.1) | arm64-v8a, armeabi-v7a |
| macOS | 10.12 (Sierra) | x86-64 |
| iOS | 12.2 | arm64 |

## Quick Start

```actionscript
// 1. Check support
if (!AneAwesomeUtils.isSupported) return;

// 2. Initialize (once)
var ok:Boolean = AneAwesomeUtils.instance.initialize();

// 3. Use it
AneAwesomeUtils.instance.loadUrl("https://api.example.com/data", "GET",
    null, null,
    function(response:ByteArray):void { trace(response.toString()); },
    function(error:Error):void { trace(error.message); }
);
```

See [Getting Started](docs/getting-started.md) for full installation and setup instructions.

## Documentation

- [Getting Started](docs/getting-started.md) - Installation, app descriptor, initialization
- [HTTP Client](docs/http-client.md) - URL loading with HTTP/2, progress, custom headers
- [WebSocket](docs/websocket.md) - Binary WebSocket connections
- [mTLS Client Certificates](docs/mtls.md) - Mutual TLS authentication
- [Static DNS / Host Resolution](docs/dns.md) - Custom DNS mapping
- [Utilities](docs/utilities.md) - Compression, file I/O, XML parsing, device ID, emulator detection
- [Windows Features](docs/platform-windows.md) - Screen capture, input filtering, speed hack detection, audio hook
- [Android Features](docs/platform-android.md) - Battery optimization, connection config, network monitoring
- [Logging](docs/logging.md) - Custom logging interface

## Networking Highlights

- **Happy Eyeballs (RFC 8305)** - Fast dual-stack IPv4/IPv6 connection racing
- **HTTP/2** with automatic HTTP/1.1 fallback
- **TLS 1.3** for improved security and performance
- **Custom DNS** - Cloudflare + Google DNS by default, configurable static hosts

## Build Output

| File | Description |
|---|---|
| `AneBuild/br.com.redesurftank.aneawesomeutils.ane` | Packaged ANE (all platforms) |
| `AneBuild/windows-32/AneAwesomeUtilsWindows.dll` | Windows x86 native |
| `AneBuild/windows-64/AneAwesomeUtilsWindows.dll` | Windows x64 native |
| `AndroidNative/app/build/outputs/aar/app-debug.aar` | Android native |
| `AppleNative/build/AneAwesomeUtils.framework` | macOS native |
| `AppleNative/build/libAneAwesomeUtils-IOS.a` | iOS native |

## License

Proprietary.
