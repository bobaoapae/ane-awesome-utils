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

    // Called from AndroidDeepMemoryHook::proxy_FixedAlloc after a successful
    // allocation. Pushes (ptr, size) into a global ring buffer; the worker
    // thread drains entries that are >kDeferDelayUs old and walks the
    // VTable→Traits→name chain to emit a typed As3Alloc event.
    //
    // Thread-safe; lock-free via atomic indices on the ring buffer. Returns
    // false if the queue is full (caller continues without As3 emission —
    // we never block the alloc hot path).
    bool recordAllocPending(void* ptr, std::size_t size);

    // Diagnostic counters
    std::uint64_t allocsObserved() const;
    std::uint64_t namesResolved() const;
    std::uint64_t namesUnresolved() const;
    std::uint64_t allocsQueued() const;
    std::uint64_t allocsDropped() const;

    // Field offsets derived from avmplus source. Each pointer field scales
    // with sizeof(void*): 8 bytes on AArch64, 4 bytes on ARMv7. Subject to
    // runtime verification via installProbe() before going hot.
    //
    // Inheritance chain for any AS3 ScriptObject (V = sizeof(void*)):
    //   GCTraceableBase (virtual → vtable* @0, V bytes)
    //     ↓
    //   GCFinalizedObject (no fields)
    //     ↓
    //   RCObject (uint32_t composite @V; on ARM64 also 4 bytes padding)
    //     ↓
    //   AvmPlusScriptableObject (DEBUGGER-off → no fields)
    //     ↓
    //   ScriptObject (VTable* vtable @2V on AArch64 / @V+4 on ARMv7,
    //                 GCMember<ScriptObject> delegate @+V)
    struct LayoutOffsets {
        // ScriptObject layout depends on architecture:
        //   AArch64: vtable*(8) + composite(4)+pad(4) + VTable*(8) → VTable@16
        //   ARMv7:   vtable*(4) + composite(4)         + VTable*(4) → VTable@8
        // String inherits the SAME RCObject base (vtable + composite uint32
        // + pad on 64-bit), NOT a zero-byte base. Earlier versions of this
        // struct treated AvmPlusScriptableObject as 0 bytes and put
        // m_buffer at +V instead of +V+composite — that read garbage for
        // _name even when the rest of the resolution succeeded. Source-
        // verified against core/StringObject.h:46-496 by the phase4c-ra
        // source-analyst pass: every String field gets +V (4 ARMv7 / 8
        // AArch64) for the RCObject composite + pad.
#if defined(__aarch64__)
        std::uint32_t scriptobject_vtable_off = 16;
        // VTable->Traits: source-derived V*5 (= 40) does not match the
        // shipping libCore.so layout. ARMv7 (V=4) auto-discovery probe
        // proved the actual offset is V*8 (= 32 on ARMv7) — 3 extra
        // pointer-sized fields beyond the open-source avmplus VTable.h
        // layout are present in Adobe's build. The same 3 extra fields
        // should apply on AArch64 by the same logic, scaling V to 8 →
        // traits_off = 8 * 8 = 64. Run the auto-discovery probe on a
        // OnePlus 15 ARM64 build-id 7dde220f... to confirm.
        std::uint32_t vtable_traits_off       = 64;  // V*8 = 8*8 (extrapolated from ARMv7 discovery)
        std::uint32_t traits_name_off         = 144; // V*18 = 8*18 (core+base+param+cache+neg+8*primary+sec+pool+itraits+ns)
        std::uint32_t string_buffer_off       = 16;  // vtable(8) + composite(4) + pad(4)
        std::uint32_t string_extra_off        = 24;  // + V
        std::uint32_t string_length_off       = 32;  // + V
        std::uint32_t string_flags_off        = 36;  // + 4 (uint32 m_bitsAndFlags)
#elif defined(__arm__)
        std::uint32_t scriptobject_vtable_off = 8;   // vtable(4) + composite(4)
        // VTable->Traits offset: source-derived value (V*5 = 20) does NOT
        // match the shipping libCore.so layout. Auto-discovery probe on
        // build-id 582a8f65... (Galaxy A10 ARMv7 51.1.3.10) reported 35
        // resolved-name hits for offset 32 vs zero for offset 20 — Adobe's
        // release build adds 3 extra pointer-sized fields to VTable beyond
        // the open-source avmplus VTable.h layout. Source had:
        //   GCTraceableObject vtable*(0), _toplevel(V), init(2V), base(3V),
        //   ivtable(4V), traits(5V) → traits_off = 5V = 20.
        // Shipping build appears to have added 3 more V-sized fields before
        // traits (possibly cached prototype/ctor/native-ID slots that were
        // gated behind a build flag in the OSS sources).
        std::uint32_t vtable_traits_off       = 32;  // V*8 = 4*8 — discovery-validated
        std::uint32_t traits_name_off         = 72;  // V*18 = 4*18
        std::uint32_t string_buffer_off       = 8;   // vtable(4) + composite(4)
        std::uint32_t string_extra_off        = 12;  // + V
        std::uint32_t string_length_off       = 16;  // + V
        std::uint32_t string_flags_off        = 20;  // + 4 (uint32 m_bitsAndFlags)
#else
#  error "Unsupported architecture for AndroidAs3ObjectHook"
#endif
    };

    // String.m_bitsAndFlags bit definitions (from StringObject.h)
    static constexpr std::uint32_t kStringWidthMask    = 0x00000001;
    static constexpr std::uint32_t k7BitAsciiFlag      = 0x00000008;
    static constexpr std::uint32_t kInternedFlag       = 0x00000010;

    // Drain delay: we wait ~50us after the alloc before reading ptr[0]. By
    // then the AS3 constructor has set up the VTable.
    static constexpr std::uint64_t kDeferDelayNs = 50'000;  // 50us

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
