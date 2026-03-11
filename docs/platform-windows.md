# Windows-Specific Features

Features available only on Windows. Calling these methods on other platforms is safe — they return `false` or do nothing.

## Screen Capture Prevention

Prevents the application window from being captured by screen recording, screenshots, or screen sharing tools.

```actionscript
public function preventCaptureScreen():Boolean
public function isPreventCaptureEnabled():Boolean
```

### Implementation

| Windows Version | Method |
|---|---|
| Windows 10 Build 17134+ | `WDA_EXCLUDEFROMCAPTURE` (window appears black to capture tools) |
| Older Windows | `WDA_MONITOR` (window content hidden) |

### Example

```actionscript
var success:Boolean = AneAwesomeUtils.instance.preventCaptureScreen();
if (success) {
    trace("Screen capture prevention enabled");
}

// Check status later
if (AneAwesomeUtils.instance.isPreventCaptureEnabled()) {
    trace("Capture prevention is active");
}
```

---

## Input Filtering

Block injected keyboard and mouse inputs (from tools like AutoHotkey, macros, or cheat software). Only inputs flagged as "injected" by Windows are blocked — real physical inputs pass through normally.

```actionscript
public function filterWindowsInputs(filteredKeys:Array = null):Boolean
public function stopWindowsFilterInputs():Boolean
```

### Parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `filteredKeys` | `Array` | `null` | Array of virtual key codes to filter. If `null`, **all** injected inputs are blocked. |

### Block All Injected Inputs

```actionscript
AneAwesomeUtils.instance.filterWindowsInputs();
```

### Block Specific Keys Only

```actionscript
import aneAwesomeUtils.VirtualKeyCodes;

// Only block injected Space, Enter, and arrow keys
AneAwesomeUtils.instance.filterWindowsInputs([
    VirtualKeyCodes.VK_SPACE,
    VirtualKeyCodes.VK_RETURN,
    VirtualKeyCodes.VK_LEFT,
    VirtualKeyCodes.VK_RIGHT,
    VirtualKeyCodes.VK_UP,
    VirtualKeyCodes.VK_DOWN
]);
```

### Stop Filtering

```actionscript
AneAwesomeUtils.instance.stopWindowsFilterInputs();
```

### VirtualKeyCodes Reference

The `aneAwesomeUtils.VirtualKeyCodes` class provides constants for all Windows virtual key codes:

| Constant | Value | Description |
|---|---|---|
| `VK_RETURN` | `0x0D` | Enter key |
| `VK_SHIFT` | `0x10` | Shift key |
| `VK_CONTROL` | `0x11` | Ctrl key |
| `VK_MENU` | `0x12` | Alt key |
| `VK_ESCAPE` | `0x1B` | Esc key |
| `VK_SPACE` | `0x20` | Spacebar |
| `VK_LEFT/UP/RIGHT/DOWN` | `0x25-0x28` | Arrow keys |
| `VK_KEY_A` to `VK_KEY_Z` | `0x41-0x5A` | Letter keys |
| `VK_KEY_0` to `VK_KEY_9` | `0x30-0x39` | Number keys |
| `VK_F1` to `VK_F24` | `0x70-0x87` | Function keys |

See `src/aneAwesomeUtils/VirtualKeyCodes.as` for the complete list.

---

## Mouse Leave Blocking

Prevents the mouse cursor from being programmatically moved outside the application window by subclassing the main window procedure.

```actionscript
public function blockWindowsLeaveMouseEvent():Boolean
```

### Example

```actionscript
AneAwesomeUtils.instance.blockWindowsLeaveMouseEvent();
```

---

## Speed Hack Detection

Detects if Cheat Engine (or similar tools) has hooked the `GetTickCount` or `QueryPerformanceCounter` Windows APIs, which is a common technique for speed hacking games.

```actionscript
public function isCheatEngineSpeedHackDetected():Boolean
```

### Example

```actionscript
if (AneAwesomeUtils.instance.isCheatEngineSpeedHackDetected()) {
    trace("Speed hack detected!");
}
```

### How It Works

The function checks if the entry points of `kernel32.dll!GetTickCount` and `kernel32.dll!QueryPerformanceCounter` have been modified (hooked). Hooks on these functions are a strong indicator of speed manipulation tools.

---

## Force Blue Screen (BSOD)

Forces a Windows Blue Screen of Death. **Requires administrator privileges.**

```actionscript
public function forceBlueScreenOfDead():void
```

> **Warning:** This will immediately crash the system. Use only as an extreme anti-tamper measure.

---

## Audio Safety Hook

Automatically installed when the DLL loads. Prevents crashes when the Windows Audio Service (Audiosrv) is unavailable by hooking `waveOutOpen` in the Adobe AIR.dll IAT.

This is **transparent** — no ActionScript API is needed. When the audio service is down, `waveOutOpen` returns `MMSYSERR_NODRIVER` instead of crashing with RPC exception `0x6BA`. AIR handles this gracefully and continues without audio.
