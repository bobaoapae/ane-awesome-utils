// FREFunction bindings for the profiler subsystem. One helper registers
// them all into the ANE's function table.

#ifndef ANE_PROFILER_FRE_BINDINGS_HPP
#define ANE_PROFILER_FRE_BINDINGS_HPP

// FlashRuntimeExtensions.h references HWND (FRENativeWindow) without
// including windows.h itself, so consumers must have Win32 types in
// scope first.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif
#include <FlashRuntimeExtensions.h>
#include <cstdint>

namespace ane::profiler::bindings {

// Four functions exposed to AS3:
//   profiler_start(outputPath:String, maxBytesMb:uint=900,
//                  compressionLevel:int=6, headerJson:String=""): Boolean
//   profiler_stop(): Boolean
//   profiler_get_status(): Object (state, bytesIn, bytesOut, records, drops, elapsedMs)
//   profiler_take_marker(name:String): Boolean (stored in header JSON at stop)
FREObject profiler_start       (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_stop        (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_get_status  (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_take_marker (FREContext, void*, std::uint32_t, FREObject*);

// Register the four functions starting at `out_functions[*cursor]`. Bumps
// `*cursor` by the number of entries added.
void register_all(FRENamedFunction* out_functions, int capacity, int* cursor);

// Finalize — called on extension shutdown to tear down any capture in progress
// and uninstall the hook.
void shutdown();

} // namespace ane::profiler::bindings

#endif // ANE_PROFILER_FRE_BINDINGS_HPP
