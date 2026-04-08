#ifndef ANEAWESOMEUTILSWINDOWS_SAMPLERSAFETYHOOK_H
#define ANEAWESOMEUTILSWINDOWS_SAMPLERSAFETYHOOK_H
#pragma once

// Installs a Vectored Exception Handler that catches ACCESS_VIOLATION crashes
// inside Adobe AIR.dll's sampler record parser (avmplus).
// Safe to call from DllMain(DLL_PROCESS_ATTACH).
void InstallSamplerSafetyHook();
void RemoveSamplerSafetyHook();

#endif //ANEAWESOMEUTILSWINDOWS_SAMPLERSAFETYHOOK_H
