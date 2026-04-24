#include "profiler/WindowsDeepMemoryHook.hpp"

#include "AirTelemetryRvas.h"
#include "DeepProfilerController.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ane::profiler {

namespace {
std::atomic<DeepProfilerController*> g_controller{nullptr};
std::atomic<std::uint64_t> g_alloc_calls{0};
std::atomic<std::uint64_t> g_alloc_locked_calls{0};
std::atomic<std::uint64_t> g_heap_alloc_calls{0};
std::atomic<std::uint64_t> g_free_calls{0};
std::atomic<std::uint64_t> g_heap_free_calls{0};
std::atomic<std::uint64_t> g_heap_realloc_calls{0};
std::atomic<std::uint64_t> g_failed_installs{0};
std::atomic<std::uint32_t> g_last_failure_stage{0};
thread_local bool g_inside_hook = false;

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)

using HeapAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapFreeFn = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID);
using HeapReAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);

#if defined(_M_X64) || defined(__x86_64__)

#define ANE_X86_CDECL
#define ANE_X86_FASTCALL
using AllocSmallFn = void* (*)(std::size_t, int);
using AllocLockedFn = void* (*)(void*, std::size_t, int);
using MMgcFreeFn = void (*)(void*, void*);
inline constexpr std::size_t kJumpSize = 14;

constexpr std::array<std::uint8_t, 15> kAllocSmallPrologue = {
    0x48, 0x89, 0x5c, 0x24, 0x08,
    0x48, 0x89, 0x6c, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18,
};
constexpr std::array<std::uint8_t, 17> kAllocLockedPrologue = {
    0x48, 0x89, 0x5c, 0x24, 0x08,
    0x48, 0x89, 0x74, 0x24, 0x10,
    0x48, 0x89, 0x7c, 0x24, 0x18,
    0x41, 0x56,
};
constexpr std::array<std::uint8_t, 15> kMMgcFreeHookBodyPrologue = {
    0x48, 0x89, 0x5c, 0x24, 0x08,
    0x48, 0x89, 0x6c, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18,
};
constexpr std::array<std::uint8_t, 15> kAllocSmallMask = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
constexpr std::array<std::uint8_t, 17> kAllocLockedMask = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff,
};
constexpr std::array<std::uint8_t, 15> kMMgcFreeHookBodyMask = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
inline constexpr std::size_t kAllocSmallStolen = kAllocSmallPrologue.size();
inline constexpr std::size_t kAllocLockedStolen = kAllocLockedPrologue.size();
inline constexpr std::size_t kMMgcFreeStolen = kMMgcFreeHookBodyPrologue.size();

#elif defined(_M_IX86) || defined(__i386__)

#define ANE_X86_CDECL __cdecl
#define ANE_X86_FASTCALL __fastcall
using AllocSmallFn = void* (__cdecl*)(std::size_t, int);
using AllocLockedFn = void* (__fastcall*)(void*, std::size_t, int);
using MMgcFreeFn = void (__fastcall*)(void*, void*);
inline constexpr std::size_t kJumpSize = 6;

constexpr std::array<std::uint8_t, 12> kAllocSmallPrologue = {
    0x51, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x33, 0xc4, 0x89, 0x04, 0x24, 0x53,
};
constexpr std::array<std::uint8_t, 12> kAllocSmallMask = {
    0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
constexpr std::array<std::uint8_t, 12> kAllocLockedPrologue = {
    0x51, 0x51, 0x53, 0x55, 0x56, 0x57,
    0x81, 0xfa, 0xf0, 0x07, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 12> kAllocLockedMask = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
constexpr std::array<std::uint8_t, 14> kMMgcFreeHookBodyPrologue = {
    0x53, 0x8b, 0xda, 0x56, 0x8b, 0xf1, 0x85, 0xdb,
    0x0f, 0x84, 0xdf, 0x00, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 14> kMMgcFreeHookBodyMask = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
inline constexpr std::size_t kAllocSmallStolen = 6;
inline constexpr std::size_t kAllocLockedStolen = 6;
inline constexpr std::size_t kMMgcFreeStolen = 6;

#endif

struct Detour {
    void* target = nullptr;
    void* hook = nullptr;
    void* trampoline = nullptr;
    std::array<std::uint8_t, 32> original{};
    std::size_t stolen = 0;
};

struct IatPatch {
    void** slot = nullptr;
    void* original = nullptr;
    void* hook = nullptr;
};

Detour g_alloc_small;
Detour g_alloc_locked;
Detour g_mmgc_free;
AllocSmallFn g_original_alloc_small = nullptr;
AllocLockedFn g_original_alloc_locked = nullptr;
MMgcFreeFn g_original_mmgc_free = nullptr;
IatPatch g_heap_free_iat;
IatPatch g_heap_alloc_iat;
IatPatch g_heap_realloc_iat;
HeapAllocFn g_original_heap_alloc = nullptr;
HeapFreeFn g_original_heap_free = nullptr;
HeapReAllocFn g_original_heap_realloc = nullptr;

void write_jump(std::uint8_t* dst, void* target) {
#if defined(_M_X64) || defined(__x86_64__)
    // jmp qword ptr [rip+0]; <absolute address>
    dst[0] = 0xff;
    dst[1] = 0x25;
    dst[2] = 0x00;
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x00;
    std::memcpy(dst + 6, &target, sizeof(target));
#else
    // push imm32; ret. This is absolute and does not depend on rel32 reach.
    const auto target32 = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target));
    dst[0] = 0x68;
    std::memcpy(dst + 1, &target32, sizeof(target32));
    dst[5] = 0xc3;
#endif
}

bool matches_signature(const void* target,
                       const std::uint8_t* expected,
                       const std::uint8_t* mask,
                       std::size_t size) {
    if (target == nullptr || expected == nullptr || mask == nullptr) return false;
    const auto* actual = static_cast<const std::uint8_t*>(target);
    for (std::size_t i = 0; i < size; ++i) {
        if ((actual[i] & mask[i]) != (expected[i] & mask[i])) return false;
    }
    return true;
}

bool install_detour(Detour& d,
                    void* target,
                    void* hook,
                    const std::uint8_t* expected,
                    const std::uint8_t* mask,
                    std::size_t expected_size,
                    std::size_t stolen) {
    if (target == nullptr || hook == nullptr ||
        stolen < kJumpSize || stolen > d.original.size() || expected_size < stolen) {
        return false;
    }
    if (!matches_signature(target, expected, mask, expected_size)) {
        return false;
    }

    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, stolen + kJumpSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) return false;

    std::memcpy(d.original.data(), target, stolen);
    std::memcpy(trampoline, target, stolen);
    void* return_addr = static_cast<std::uint8_t*>(target) + stolen;
    write_jump(trampoline + stolen, return_addr);

    DWORD old_prot = 0;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old_prot)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    std::uint8_t patch[32] = {};
    std::memset(patch, 0x90, sizeof(patch));
    write_jump(patch, hook);
    std::memcpy(target, patch, stolen);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);

    DWORD tmp = 0;
    VirtualProtect(target, stolen, old_prot, &tmp);

    d.target = target;
    d.hook = hook;
    d.trampoline = trampoline;
    d.stolen = stolen;
    return true;
}

void uninstall_detour(Detour& d) {
    if (d.target == nullptr || d.stolen == 0) return;
    DWORD old_prot = 0;
    if (VirtualProtect(d.target, d.stolen, PAGE_EXECUTE_READWRITE, &old_prot)) {
        std::memcpy(d.target, d.original.data(), d.stolen);
        FlushInstructionCache(GetCurrentProcess(), d.target, d.stolen);
        DWORD tmp = 0;
        VirtualProtect(d.target, d.stolen, old_prot, &tmp);
    }
    if (d.trampoline != nullptr) {
        VirtualFree(d.trampoline, 0, MEM_RELEASE);
    }
    d = Detour{};
}

template <typename T>
T* ptr_from_rva(HMODULE module, DWORD rva) {
    return reinterpret_cast<T*>(reinterpret_cast<std::uint8_t*>(module) + rva);
}

void** find_iat_slot(HMODULE module, const char* dll_name, const char* function_name) {
    if (module == nullptr || dll_name == nullptr || function_name == nullptr) return nullptr;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<std::uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;

    auto* desc = ptr_from_rva<IMAGE_IMPORT_DESCRIPTOR>(module, dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* imported_dll = ptr_from_rva<char>(module, desc->Name);
        if (lstrcmpiA(imported_dll, dll_name) != 0) continue;

        auto* names = ptr_from_rva<IMAGE_THUNK_DATA>(
            module, desc->OriginalFirstThunk != 0 ? desc->OriginalFirstThunk : desc->FirstThunk);
        auto* iat = ptr_from_rva<IMAGE_THUNK_DATA>(module, desc->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* by_name = ptr_from_rva<IMAGE_IMPORT_BY_NAME>(
                module, static_cast<DWORD>(names->u1.AddressOfData));
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function_name) == 0) {
                return reinterpret_cast<void**>(&iat->u1.Function);
            }
        }
    }
    return nullptr;
}

bool install_iat_patch(IatPatch& p,
                       HMODULE module,
                       const char* function_name,
                       void* hook,
                       std::uint32_t expected_rva) {
    if (p.slot != nullptr || module == nullptr || hook == nullptr) return false;
    void** slot = find_iat_slot(module, "KERNEL32.dll", function_name);
    if (slot == nullptr) return false;

    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto slot_rva = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(slot) - base);
    if (expected_rva != 0 && slot_rva != expected_rva) return false;

    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        return false;
    }
    p.slot = slot;
    p.original = *slot;
    p.hook = hook;
    *slot = hook;
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old_prot, &tmp);
    return p.original != nullptr;
}

void uninstall_iat_patch(IatPatch& p) {
    if (p.slot == nullptr) return;
    DWORD old_prot = 0;
    if (VirtualProtect(p.slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        *p.slot = p.original;
        FlushInstructionCache(GetCurrentProcess(), p.slot, sizeof(void*));
        DWORD tmp = 0;
        VirtualProtect(p.slot, sizeof(void*), old_prot, &tmp);
    }
    p = IatPatch{};
}

void* ANE_X86_CDECL hook_alloc_small(std::size_t size, int flags) {
    auto* real = g_original_alloc_small;
    if (real == nullptr) return nullptr;
    void* ptr = real(size, flags);
    g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
    if (ptr != nullptr && !g_inside_hook) {
        g_inside_hook = true;
        if (auto* ctrl = g_controller.load(std::memory_order_acquire)) {
            ctrl->record_alloc_if_untracked(ptr, static_cast<std::uint64_t>(size));
        }
        g_inside_hook = false;
    }
    return ptr;
}

void* ANE_X86_FASTCALL hook_alloc_locked(void* heap, std::size_t size, int flags) {
    auto* real = g_original_alloc_locked;
    if (real == nullptr) return nullptr;
    void* ptr = real(heap, size, flags);
    g_alloc_locked_calls.fetch_add(1, std::memory_order_relaxed);
    if (ptr != nullptr && !g_inside_hook) {
        g_inside_hook = true;
        if (auto* ctrl = g_controller.load(std::memory_order_acquire)) {
            ctrl->record_alloc_if_untracked(ptr, static_cast<std::uint64_t>(size));
        }
        g_inside_hook = false;
    }
    return ptr;
}

void ANE_X86_FASTCALL hook_mmgc_free(void* heap, void* ptr) {
    auto* real = g_original_mmgc_free;
    if (real != nullptr) {
        real(heap, ptr);
    }
    g_free_calls.fetch_add(1, std::memory_order_relaxed);
    if (ptr != nullptr && !g_inside_hook) {
        g_inside_hook = true;
        if (auto* ctrl = g_controller.load(std::memory_order_acquire)) {
            ctrl->record_free_if_tracked(ptr);
        }
        g_inside_hook = false;
    }
}

LPVOID WINAPI hook_heap_alloc(HANDLE heap, DWORD flags, SIZE_T bytes) {
    auto* real = g_original_heap_alloc;
    if (real == nullptr) return nullptr;
    LPVOID ptr = real(heap, flags, bytes);
    g_heap_alloc_calls.fetch_add(1, std::memory_order_relaxed);
    if (ptr != nullptr && !g_inside_hook) {
        g_inside_hook = true;
        if (auto* ctrl = g_controller.load(std::memory_order_acquire)) {
            ctrl->record_alloc_if_untracked(ptr, static_cast<std::uint64_t>(bytes));
        }
        g_inside_hook = false;
    }
    return ptr;
}

BOOL WINAPI hook_heap_free(HANDLE heap, DWORD flags, LPVOID mem) {
    auto* real = g_original_heap_free;
    if (real == nullptr) return FALSE;
    const BOOL ok = real(heap, flags, mem);
    g_heap_free_calls.fetch_add(1, std::memory_order_relaxed);
    if (ok && mem != nullptr && !g_inside_hook) {
        g_inside_hook = true;
        if (auto* ctrl = g_controller.load(std::memory_order_acquire)) {
            ctrl->record_free_if_tracked(mem);
        }
        g_inside_hook = false;
    }
    return ok;
}

LPVOID WINAPI hook_heap_realloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T bytes) {
    auto* real = g_original_heap_realloc;
    if (real == nullptr) return nullptr;
    LPVOID ptr = real(heap, flags, mem, bytes);
    g_heap_realloc_calls.fetch_add(1, std::memory_order_relaxed);
    if (ptr != nullptr && mem != nullptr && !g_inside_hook) {
        g_inside_hook = true;
        if (auto* ctrl = g_controller.load(std::memory_order_acquire)) {
            ctrl->record_realloc_if_tracked(mem, ptr, static_cast<std::uint64_t>(bytes));
        }
        g_inside_hook = false;
    }
    return ptr;
}

#endif
} // namespace

WindowsDeepMemoryHook::~WindowsDeepMemoryHook() {
    uninstall();
}

bool WindowsDeepMemoryHook::install(DeepProfilerController* controller) {
    if (controller == nullptr) return false;
    g_controller.store(controller, std::memory_order_release);
    if (installed_) return true;

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    g_last_failure_stage.store(0, std::memory_order_relaxed);
    HMODULE air = GetModuleHandleA(air_rvas::kDllName);
    if (air == nullptr) {
        g_last_failure_stage.store(1, std::memory_order_relaxed);
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(air);
    void* alloc_small = reinterpret_cast<void*>(base + air_rvas::kRvaMMgcAllocSmall);
    void* alloc_locked = reinterpret_cast<void*>(base + air_rvas::kRvaMMgcAllocLocked);
    void* mmgc_free = reinterpret_cast<void*>(base + air_rvas::kRvaMMgcFreeHookBody);

    if (!install_detour(g_alloc_small,
                        alloc_small,
                        reinterpret_cast<void*>(&hook_alloc_small),
                        kAllocSmallPrologue.data(),
                        kAllocSmallMask.data(),
                        kAllocSmallPrologue.size(),
                        kAllocSmallStolen)) {
        g_last_failure_stage.store(2, std::memory_order_relaxed);
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        g_controller.store(nullptr, std::memory_order_release);
        return false;
    }
    g_original_alloc_small = reinterpret_cast<AllocSmallFn>(g_alloc_small.trampoline);

    if (!install_detour(g_alloc_locked,
                        alloc_locked,
                        reinterpret_cast<void*>(&hook_alloc_locked),
                        kAllocLockedPrologue.data(),
                        kAllocLockedMask.data(),
                        kAllocLockedPrologue.size(),
                        kAllocLockedStolen)) {
        g_last_failure_stage.store(3, std::memory_order_relaxed);
        uninstall_detour(g_alloc_small);
        g_original_alloc_small = nullptr;
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        g_controller.store(nullptr, std::memory_order_release);
        return false;
    }
    g_original_alloc_locked = reinterpret_cast<AllocLockedFn>(g_alloc_locked.trampoline);

    if (!install_detour(g_mmgc_free,
                        mmgc_free,
                        reinterpret_cast<void*>(&hook_mmgc_free),
                        kMMgcFreeHookBodyPrologue.data(),
                        kMMgcFreeHookBodyMask.data(),
                        kMMgcFreeHookBodyPrologue.size(),
                        kMMgcFreeStolen)) {
        g_last_failure_stage.store(4, std::memory_order_relaxed);
        uninstall_detour(g_alloc_locked);
        uninstall_detour(g_alloc_small);
        g_original_alloc_locked = nullptr;
        g_original_alloc_small = nullptr;
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        g_controller.store(nullptr, std::memory_order_release);
        return false;
    }
    g_original_mmgc_free = reinterpret_cast<MMgcFreeFn>(g_mmgc_free.trampoline);

    if (!install_iat_patch(g_heap_alloc_iat,
                           air,
                           "HeapAlloc",
                           reinterpret_cast<void*>(&hook_heap_alloc),
                           air_rvas::kRvaIatKernel32HeapAlloc)) {
        g_last_failure_stage.store(5, std::memory_order_relaxed);
        uninstall_detour(g_mmgc_free);
        uninstall_detour(g_alloc_locked);
        uninstall_detour(g_alloc_small);
        g_original_mmgc_free = nullptr;
        g_original_alloc_locked = nullptr;
        g_original_alloc_small = nullptr;
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        g_controller.store(nullptr, std::memory_order_release);
        return false;
    }
    g_original_heap_alloc = reinterpret_cast<HeapAllocFn>(g_heap_alloc_iat.original);

    if (!install_iat_patch(g_heap_free_iat,
                           air,
                           "HeapFree",
                           reinterpret_cast<void*>(&hook_heap_free),
                           air_rvas::kRvaIatKernel32HeapFree)) {
        g_last_failure_stage.store(6, std::memory_order_relaxed);
        uninstall_iat_patch(g_heap_alloc_iat);
        uninstall_detour(g_mmgc_free);
        uninstall_detour(g_alloc_locked);
        uninstall_detour(g_alloc_small);
        g_original_heap_alloc = nullptr;
        g_original_mmgc_free = nullptr;
        g_original_alloc_locked = nullptr;
        g_original_alloc_small = nullptr;
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        g_controller.store(nullptr, std::memory_order_release);
        return false;
    }
    g_original_heap_free = reinterpret_cast<HeapFreeFn>(g_heap_free_iat.original);

    if (!install_iat_patch(g_heap_realloc_iat,
                           air,
                           "HeapReAlloc",
                           reinterpret_cast<void*>(&hook_heap_realloc),
                           air_rvas::kRvaIatKernel32HeapReAlloc)) {
        g_last_failure_stage.store(7, std::memory_order_relaxed);
        uninstall_iat_patch(g_heap_free_iat);
        uninstall_iat_patch(g_heap_alloc_iat);
        uninstall_detour(g_mmgc_free);
        uninstall_detour(g_alloc_locked);
        uninstall_detour(g_alloc_small);
        g_original_heap_free = nullptr;
        g_original_heap_alloc = nullptr;
        g_original_mmgc_free = nullptr;
        g_original_alloc_locked = nullptr;
        g_original_alloc_small = nullptr;
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        g_controller.store(nullptr, std::memory_order_release);
        return false;
    }
    g_original_heap_realloc = reinterpret_cast<HeapReAllocFn>(g_heap_realloc_iat.original);
    free_hooks_installed_ = true;
    realloc_hooks_installed_ = true;
    installed_ = true;
    g_last_failure_stage.store(0, std::memory_order_relaxed);
    return true;
#else
    g_last_failure_stage.store(8, std::memory_order_relaxed);
    g_failed_installs.fetch_add(1, std::memory_order_relaxed);
    g_controller.store(nullptr, std::memory_order_release);
    return false;
#endif
}

void WindowsDeepMemoryHook::uninstall() {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    if (installed_) {
        uninstall_iat_patch(g_heap_realloc_iat);
        uninstall_iat_patch(g_heap_free_iat);
        uninstall_iat_patch(g_heap_alloc_iat);
        uninstall_detour(g_mmgc_free);
        uninstall_detour(g_alloc_locked);
        uninstall_detour(g_alloc_small);
    }
    g_original_heap_realloc = nullptr;
    g_original_heap_free = nullptr;
    g_original_heap_alloc = nullptr;
    g_original_mmgc_free = nullptr;
    g_original_alloc_locked = nullptr;
    g_original_alloc_small = nullptr;
#endif
    installed_ = false;
    free_hooks_installed_ = false;
    realloc_hooks_installed_ = false;
    g_controller.store(nullptr, std::memory_order_release);
}

std::uint64_t WindowsDeepMemoryHook::allocCalls() const {
    return g_alloc_calls.load(std::memory_order_relaxed);
}

std::uint64_t WindowsDeepMemoryHook::allocLockedCalls() const {
    return g_alloc_locked_calls.load(std::memory_order_relaxed);
}

std::uint64_t WindowsDeepMemoryHook::heapAllocCalls() const {
    return g_heap_alloc_calls.load(std::memory_order_relaxed);
}

std::uint64_t WindowsDeepMemoryHook::freeCalls() const {
    return g_free_calls.load(std::memory_order_relaxed);
}

std::uint64_t WindowsDeepMemoryHook::heapFreeCalls() const {
    return g_heap_free_calls.load(std::memory_order_relaxed);
}

std::uint64_t WindowsDeepMemoryHook::heapReallocCalls() const {
    return g_heap_realloc_calls.load(std::memory_order_relaxed);
}

std::uint64_t WindowsDeepMemoryHook::failedInstalls() const {
    return g_failed_installs.load(std::memory_order_relaxed);
}

std::uint32_t WindowsDeepMemoryHook::lastFailureStage() const {
    return g_last_failure_stage.load(std::memory_order_relaxed);
}

} // namespace ane::profiler
