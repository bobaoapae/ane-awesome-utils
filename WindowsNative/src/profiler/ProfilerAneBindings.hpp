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

// Public functions exposed to AS3:
//   profiler_start(outputPath:String, headerJson:String, timing:Boolean,
//                  memory:Boolean, snapshots:Boolean, maxLive:uint,
//                  snapshotIntervalMs:uint): Boolean
//   profiler_stop(): Boolean
//   profiler_snapshot(label:String): Boolean
//   profiler_marker(name:String, valueJson:String): Boolean
//   profiler_get_status(): Object
//
// Additional probe functions are intentionally internal: compiler-injected
// AS3 can call them, but the public API remains the five methods above.
FREObject profiler_start       (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_stop        (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_get_status  (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_snapshot    (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_marker      (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_probe_enter (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_probe_exit  (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_register_method_table(FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_record_alloc(FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_record_free (FREContext, void*, std::uint32_t, FREObject*);
FREObject profiler_record_realloc(FREContext, void*, std::uint32_t, FREObject*);

// Register the four functions starting at `out_functions[*cursor]`. Bumps
// `*cursor` by the number of entries added.
void register_all(FRENamedFunction* out_functions, int capacity, int* cursor);

// Finalize — called on extension shutdown to tear down any capture in progress
// and uninstall the hook.
void shutdown();

} // namespace ane::profiler::bindings

#endif // ANE_PROFILER_FRE_BINDINGS_HPP
