# POC v1 validation — 2026-04-18

**Status: PASS (all gates)**

## What this POC validates

1. The **injection mechanism** works: launch target CREATE_SUSPENDED, do
   LoadLibrary in the remote process via CreateRemoteThread, resume.
2. `LdrRegisterDllNotification` from our DllMain catches the exact moment
   `Adobe AIR.dll` is mapped, *before* any Player/Telemetry code runs.
3. **IAT hook** on `Adobe AIR.dll!imports.ws2_32.send` (the `send` import
   from WS2_32) captures every byte the runtime pushes through its TCP
   socket — including the complete Scout wire stream.
4. Captured bytes are **byte-identical** to what a real Scout listener sees.
5. The captured bytes parse cleanly through `flash-profiler-core`'s AMF3
   decoder with zero errors.

## Evidence

```
hook output  (raw_capture.bin):       32792 bytes
listener    (scout_from_listener):    32792 bytes
listener leads by 0 bytes
BYTE-MATCH: hook output == listener[skew..end]  -> hook is faithful

analyze_dump: 3168 AMF3 values, 0 parse errors, 32792/32792 bytes consumed
```

## Notable deviation from the earlier plan

The original RE report (`docs/profiler-rva-51-1-3-10.md`) stated that
PlatformSocketWrapper::vftable slot 11 (RVA `0x180ecb828 + 0x58`) was the
`send_bytes` thunk. **This turned out to be wrong.** Static disassembly
(see scripts in this repo's git history) showed the slot 11 thunk jumps
to `0x492fc8`, which is `SocketTransport::close_1` per the RE table. The
true `send_bytes` body at `0x493060` has *zero* references anywhere in
`.text` or `.rdata`; it is reached through a heap-held function pointer
or indirect chain that cannot be resolved from the image alone.

Switching to an **IAT hook on the `ws2_32!send` import** works because
it catches the one and only kernel-transition point where bytes leave the
process, which by construction includes every Scout packet. It has the
added benefit of being a single qword write — no thunks, no vtables, no
object-layout dependencies.

## RVA used

```
RVA_IAT_WS2_SEND = 0x00b05630   # IAT slot for ws2_32!send (ordinal #19)
                                 # in Adobe AIR.dll 51.1.3.10 x64.
                                 # Static import, always at this RVA
                                 # for this DLL build.
```

## Next steps

Consider the "vtable hook" path (B) a research dead-end for this runtime
build unless someone wants to run a proper debugger and follow the
actual indirect call from `SocketTransport::send_bytes` to its target.
Production ANE can safely start from this IAT-hook approach, and later
add a complementary hook at the send_bytes body if we want the bytes
captured *before* the win32 layer (e.g. to inspect the pre-socket
buffering).

## Files

- `profiler_poc_hook.cpp` / `.dll` — the injected hook
- `profiler_poc_inject.cpp` / `profiler_inject.exe` — the launcher/injector
- `build.bat` — vcvars + ninja build script
- `run_poc.ps1` — end-to-end runner with listener + byte diff
- `raw_capture.bin`    — this run's captured hook output
- `raw_capture.bin.log` — DLL log (patches, counts)
- `scout_from_listener.bin` — real TCP-side capture for comparison
