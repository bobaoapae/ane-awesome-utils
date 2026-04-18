#include "profiler/WindowsRuntimeHook.hpp"

#include "CaptureController.hpp"

// winsock2.h must come before windows.h to avoid the legacy winsock.h
// being pulled in transitively and clashing on SOCKET / WSA* definitions.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ane::profiler {

// Two globals drive the hook. The hook function has no closure, so it has
// to reach the live CaptureController via static storage. Only one
// WindowsRuntimeHook instance at a time is supported (there's no reason to
// have more than one — a process has exactly one AIR runtime).
static std::atomic<CaptureController*> g_controller{nullptr};
static std::atomic<void*>              g_original_send{nullptr};

// -- IAT walker --------------------------------------------------------------
//
// Walks Adobe AIR.dll's Import Directory to find the IAT slot for a
// specific imported function. Works identically for PE32 (x86) and PE32+
// (x64) because PIMAGE_THUNK_DATA is typedef'd differently per target at
// compile time.

static void** find_iat_slot(HMODULE module,
                            const char* import_dll,
                            unsigned ordinal,
                            const char* name_fallback) {
    if (module == nullptr) return nullptr;

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const auto& dir = nt->OptionalHeader
                        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;

    auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + dir.VirtualAddress);

    for (; desc->Name != 0; ++desc) {
        const char* dll_name = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dll_name, import_dll) != 0) continue;

        const DWORD origThunkRva = desc->OriginalFirstThunk
                                    ? desc->OriginalFirstThunk
                                    : desc->FirstThunk;
        auto* hint = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + origThunkRva);
        auto* iat  = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        for (; hint->u1.AddressOfData != 0; ++hint, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(hint->u1.Ordinal)) {
                if (static_cast<unsigned>(IMAGE_ORDINAL(hint->u1.Ordinal)) == ordinal) {
                    return reinterpret_cast<void**>(&iat->u1.Function);
                }
            } else if (name_fallback != nullptr) {
                auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    base + hint->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(ibn->Name),
                                name_fallback) == 0) {
                    return reinterpret_cast<void**>(&iat->u1.Function);
                }
            }
        }
        // Module matched but the import wasn't there — stop, no point
        // walking further modules (they won't have this import either).
        return nullptr;
    }
    return nullptr;
}

// -- the hook ---------------------------------------------------------------
//
// ws2_32!send signature: int WSAAPI send(SOCKET, const char*, int, int)
// On x86 WSAAPI is __stdcall; on x64 the ABI is implicit. Both work by
// declaring the same WSAAPI macro as the real import.

static int WSAAPI hook_send(SOCKET s, const char* buf, int len, int flags) {
    using SendFn = int(WSAAPI*)(SOCKET, const char*, int, int);
    auto* real = reinterpret_cast<SendFn>(g_original_send.load(std::memory_order_acquire));

    // Defensive: if the hook is somehow invoked without original bound,
    // make a best effort to behave like send() succeeded.
    if (real == nullptr) return len;

    const int sent = real(s, buf, len, flags);

    if (sent > 0 && buf != nullptr) {
        CaptureController* ctrl = g_controller.load(std::memory_order_acquire);
        if (ctrl != nullptr) {
            // push_bytes checks state internally; when Idle it just bumps
            // a drop counter — near-zero cost for the typical case.
            ctrl->push_bytes(buf, static_cast<std::uint32_t>(sent));
        }
    }
    return sent;
}

// -- public interface -------------------------------------------------------

WindowsRuntimeHook::~WindowsRuntimeHook() {
    uninstall();
}

bool WindowsRuntimeHook::install(CaptureController* controller) {
    if (installed_) {
        // Already installed — just re-point the controller.
        g_controller.store(controller, std::memory_order_release);
        return true;
    }
    if (controller == nullptr) return false;

    HMODULE air = GetModuleHandleW(L"Adobe AIR.dll");
    if (air == nullptr) return false;

    void** slot = find_iat_slot(air, "ws2_32.dll", /*ordinal=*/19, /*name=*/"send");
    if (slot == nullptr) return false;

    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        return false;
    }

    original_send_ = *slot;
    iat_slot_      = slot;
    g_original_send.store(original_send_, std::memory_order_release);
    g_controller.store(controller, std::memory_order_release);

    *slot = reinterpret_cast<void*>(&hook_send);

    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old_prot, &tmp);

    installed_ = true;
    return true;
}

void WindowsRuntimeHook::uninstall() {
    if (!installed_) return;

    if (iat_slot_ != nullptr && original_send_ != nullptr) {
        DWORD old_prot = 0;
        if (VirtualProtect(iat_slot_, sizeof(void*), PAGE_READWRITE, &old_prot)) {
            *iat_slot_ = original_send_;
            DWORD tmp = 0;
            VirtualProtect(iat_slot_, sizeof(void*), old_prot, &tmp);
        }
    }
    g_controller.store(nullptr, std::memory_order_release);
    g_original_send.store(nullptr, std::memory_order_release);
    iat_slot_      = nullptr;
    original_send_ = nullptr;
    installed_     = false;
}

} // namespace ane::profiler

// Factory for IRuntimeHook::create(). On Windows we return a
// WindowsRuntimeHook. Other platforms provide their own create().
namespace ane::profiler {
std::unique_ptr<IRuntimeHook> IRuntimeHook::create() {
    return std::unique_ptr<IRuntimeHook>(new WindowsRuntimeHook());
}
} // namespace ane::profiler
