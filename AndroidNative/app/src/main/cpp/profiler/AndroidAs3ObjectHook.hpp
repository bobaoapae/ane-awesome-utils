// Phase 4c — Android typed AS3 alloc capture (Path B in PROGRESS.md).
//
// Phase 4a Path A (IMemorySampler hook) is permanently blocked: Adobe ships
// the Android libCore.so with DEBUGGER off, so the entire Sampler.cpp +
// MMgc::GC::m_sampler infrastructure is compiled out. Confirmed by reading
// avmplus open-source at C:/AiRSDKs/AIRSDK_51.1.3.12/binary-optimizer/avmplus/.
//
// Phase 4c instead reconstructs class names by walking the AVM2 Traits
// graph at allocation time. Layout offsets recovered from the open-source
// avmplus headers (MPL, same code Adobe builds from):
//
//   class Traits : public MMgc::GCTraceableObject {  // GCTraceableBase has vtable
//       AvmCore* core;                           // offset 8  (after vtable)
//       Traits* base;                            // offset 16
//       Traits* m_paramTraits;                   // offset 24
//       GCMember<Traits> m_supertype_cache;      // offset 32
//       GCHiddenPointer<Traits*> m_neg_cache;    // offset 40
//       Traits* m_primary_supertypes[8];         // offset 48..111
//       GCMember<UnscannedTraitsArray> m_secondary_supertypes;  // offset 112
//       PoolObject* pool;                        // offset 120
//       GCMember<Traits> itraits;                // offset 128
//       GCMember<Namespace> _ns;                 // offset 136
//       GCMember<String> _name;                  // offset 144  ★ THE TARGET
//       ...
//   }
//
//   class String : public AvmPlusScriptableObject {
//       // (base class fields — AvmPlusScriptableObject has vtable + Atom slots)
//       Buffer m_buffer;       // 8 bytes — union { void* pv; uint8_t* p8; wchar* p16; uint32_t offset_bytes; }
//       Extra  m_extra;        // 8 bytes — union { Stringp master; uint32_t index; }
//       int32_t  m_length;     // 4 bytes — character count (NOT bytes if k16)
//       uint32_t m_bitsAndFlags; // 4 bytes
//           // bit 0: width (0=k8 = 1 byte/char, 1=k16 = 2 bytes/char)
//           // bit 3: 7-bit-ASCII flag (set => string is pure ASCII regardless of width)
//           // bit 4: interned flag
//   }
//
// Discovery flow at runtime:
//
//   1. Hook MMgc::GC::Alloc (or its inlined fast paths). On entry get (ptr, size, traits).
//      The 'traits' arg is sometimes a Traits* directly, sometimes a "create flag"
//      and traits has to be recovered from the vtable that gets installed
//      by the constructor immediately after.
//
//   2. Defer-queue (ptr, size, captured_caller_pc) for ~50us. By that time the
//      AS3 constructor has set ptr[0] to the VTable* for this class.
//
//   3. From VTable, walk to Traits (VTable has a `traits` field).
//
//   4. Read traits[+144] = _name (Stringp).
//
//   5. From String, read m_buffer.p8 + m_length, accounting for width bit.
//
//   6. Emit As3Alloc event with name + size + caller_pc.
//
// Caveats:
//   - Concrete offsets above are derived from source. Adobe's release build
//     may have differing struct ordering due to compiler reordering (rare with
//     C++ unless -fclass-reorder is set), or different field sizes due to
//     conditional VMCFG defines. We probe at runtime — see installProbe()
//     which validates the layout against a known seed object before going hot.
//
//   - VTable -> Traits offset is itself version-dependent. Recovered via
//     probe pattern: when we see a freshly-constructed object, the byte at
//     ptr[0..7] is the VTable*, and the first heap-resident pointer in VTable
//     that points to a GCTraceableBase subclass with the expected name field
//     is our Traits.
//
//   - DEFERRED — not yet implemented. Skeleton + offsets only this iteration.

#ifndef ANE_PROFILER_ANDROID_AS3_OBJECT_HOOK_HPP
#define ANE_PROFILER_ANDROID_AS3_OBJECT_HOOK_HPP

#include <atomic>
#include <cstdint>
#include <string>

namespace ane::profiler {

class DeepProfilerController;

class AndroidAs3ObjectHook {
public:
    AndroidAs3ObjectHook() = default;
    ~AndroidAs3ObjectHook();

    // Install hook on MMgc::GC::Alloc entry. Returns false if the offset
    // table doesn't have an entry for the loaded libCore.so build-id, OR
    // if the runtime probe fails to validate the Traits/String layouts.
    bool install(DeepProfilerController* controller);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    // Diagnostic counters
    std::uint64_t allocsObserved() const;
    std::uint64_t namesResolved() const;
    std::uint64_t namesUnresolved() const;

    // Field offsets derived from avmplus source. Subject to runtime
    // verification via installProbe() before the hook goes hot.
    struct LayoutOffsets {
        std::uint32_t traits_name_off = 144;     // Traits._name (Stringp)
        std::uint32_t string_buffer_off = 0;     // String.m_buffer.p8 — TBD via probe
        std::uint32_t string_length_off = 0;     // String.m_length — TBD via probe
        std::uint32_t string_flags_off = 0;      // String.m_bitsAndFlags — TBD via probe
        std::uint32_t vtable_traits_off = 0;     // VTable -> Traits — TBD via probe
    };

private:
    // installProbe(): allocates a sentinel AS3 object via a known synthesizing
    // path, then scans the resulting memory for the expected pattern. Confirms
    // offsets BEFORE shadowhook-installing the production hook.
    bool installProbe();

    bool installed_ = false;
    LayoutOffsets layout_{};
};

} // namespace ane::profiler

#endif // ANE_PROFILER_ANDROID_AS3_OBJECT_HOOK_HPP
