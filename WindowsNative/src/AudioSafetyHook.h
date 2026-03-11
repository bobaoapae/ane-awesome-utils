#ifndef ANEAWESOMEUTILSWINDOWS_AUDIOSAFETYHOOK_H
#define ANEAWESOMEUTILSWINDOWS_AUDIOSAFETYHOOK_H
#pragma once

// Installs IAT hooks on Adobe AIR.dll to wrap audio functions with SEH,
// preventing crashes when the Windows Audio Service is unavailable (RPC 0x6BA).
// Safe to call from DllMain(DLL_PROCESS_ATTACH) — no loader-lock violations.
void InstallAudioSafetyHook();
void RemoveAudioSafetyHook();

#endif //ANEAWESOMEUTILSWINDOWS_AUDIOSAFETYHOOK_H
