#include "profiler/WindowsRenderHook.hpp"

#include "DeepProfilerController.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ane::profiler {

namespace {
std::atomic<DeepProfilerController*> g_controller{nullptr};
std::atomic<bool> g_render_capture_active{false};
std::atomic<std::uint64_t> g_hook_installs{0};
std::atomic<std::uint64_t> g_device_hook_installs{0};
std::atomic<std::uint64_t> g_texture_hook_installs{0};
std::atomic<std::uint64_t> g_failed_installs{0};
std::atomic<std::uint32_t> g_last_failure_stage{0};
std::atomic<std::uint64_t> g_present_calls{0};
std::atomic<std::uint64_t> g_draw_calls_total{0};
std::atomic<std::uint64_t> g_texture_creates_total{0};
std::atomic<std::uint64_t> g_texture_updates_total{0};
std::atomic<std::uint64_t> g_texture_upload_bytes_total{0};
std::atomic<std::uint64_t> g_render_frames_total{0};
std::atomic<std::uint64_t> g_primitives_total{0};
std::atomic<std::uint64_t> g_texture_create_bytes_total{0};

std::atomic<std::uint64_t> g_frame_index{0};
std::atomic<std::uint64_t> g_last_present_end_ns{0};
std::atomic<std::uint64_t> g_acc_draw_calls{0};
std::atomic<std::uint64_t> g_acc_primitives{0};
std::atomic<std::uint64_t> g_acc_texture_upload_bytes{0};
std::atomic<std::uint64_t> g_acc_texture_create_bytes{0};
std::atomic<std::uint32_t> g_acc_texture_create_count{0};
std::atomic<std::uint32_t> g_acc_texture_update_count{0};
std::atomic<std::uint32_t> g_acc_set_texture_count{0};
std::atomic<std::uint32_t> g_acc_render_target_change_count{0};
std::atomic<std::uint32_t> g_acc_clear_count{0};

thread_local bool g_inside_hook = false;

HMODULE g_d3d9_module = nullptr;
HMODULE g_d3d11_module = nullptr;
HWND g_dummy_window = nullptr;
std::atomic<std::uint32_t> g_last_device_create_hr{0};

constexpr std::uint32_t kMaxVtablePatches = 128;
constexpr std::uint32_t kSlotD3D9CreateDevice = 16;
constexpr std::uint32_t kSlotD3D9ExCreateDeviceEx = 20;
constexpr std::uint32_t kSlotIUnknownRelease = 2;
constexpr std::uint32_t kSlotDeviceCreateAdditionalSwapChain = 13;
constexpr std::uint32_t kSlotDeviceGetSwapChain = 14;
constexpr std::uint32_t kSlotDevicePresent = 17;
constexpr std::uint32_t kSlotDeviceGetBackBuffer = 18;
constexpr std::uint32_t kSlotDeviceCreateTexture = 23;
constexpr std::uint32_t kSlotDeviceCreateRenderTarget = 28;
constexpr std::uint32_t kSlotDeviceUpdateSurface = 30;
constexpr std::uint32_t kSlotDeviceUpdateTexture = 31;
constexpr std::uint32_t kSlotDeviceStretchRect = 34;
constexpr std::uint32_t kSlotDeviceColorFill = 35;
constexpr std::uint32_t kSlotDeviceCreateOffscreenPlainSurface = 36;
constexpr std::uint32_t kSlotDeviceSetRenderTarget = 37;
constexpr std::uint32_t kSlotDeviceClear = 43;
constexpr std::uint32_t kSlotDeviceSetTexture = 65;
constexpr std::uint32_t kSlotDeviceDrawPrimitive = 81;
constexpr std::uint32_t kSlotDeviceDrawIndexedPrimitive = 82;
constexpr std::uint32_t kSlotDeviceDrawPrimitiveUP = 83;
constexpr std::uint32_t kSlotDeviceDrawIndexedPrimitiveUP = 84;
constexpr std::uint32_t kSlotDevicePresentEx = 121;
constexpr std::uint32_t kSlotSwapChainPresent = 3;
constexpr std::uint32_t kSlotSurfaceGetDesc = 12;
constexpr std::uint32_t kSlotSurfaceLockRect = 13;
constexpr std::uint32_t kSlotSurfaceUnlockRect = 14;
constexpr std::uint32_t kSlotTextureLockRect = 19;
constexpr std::uint32_t kSlotTextureUnlockRect = 20;
constexpr std::uint32_t kSlotDxgiSwapChainPresent = 8;
constexpr std::uint32_t kSlotDxgiSwapChain1Present1 = 22;
constexpr std::uint32_t kSlotD3D11DeviceCreateTexture2D = 5;
constexpr std::uint32_t kSlotD3D11ContextDrawIndexed = 12;
constexpr std::uint32_t kSlotD3D11ContextDraw = 13;
constexpr std::uint32_t kSlotD3D11ContextDrawIndexedInstanced = 20;
constexpr std::uint32_t kSlotD3D11ContextDrawInstanced = 21;
constexpr std::uint32_t kSlotD3D11ContextOMSetRenderTargets = 33;
constexpr std::uint32_t kSlotD3D11ContextDrawAuto = 38;
constexpr std::uint32_t kSlotD3D11ContextUpdateSubresource = 48;
constexpr std::uint32_t kSlotD3D11ContextClearRenderTargetView = 50;

struct VtablePatchRecord {
    std::atomic<void**> slot{nullptr};
    std::atomic<void*> original{nullptr};
    std::uint32_t slot_index = 0;
};

std::array<VtablePatchRecord, kMaxVtablePatches> g_vtable_patches;
std::mutex g_patch_mu;

struct TextureMeta {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    std::uint64_t total_bytes = 0;
};

struct SurfaceMeta {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    std::uint64_t total_bytes = 0;
};

struct TextureLockKey {
    void* texture = nullptr;
    std::uint32_t level = 0;

    bool operator==(const TextureLockKey& other) const noexcept {
        return texture == other.texture && level == other.level;
    }
};

struct TextureLockKeyHash {
    std::size_t operator()(const TextureLockKey& key) const noexcept {
        const auto a = reinterpret_cast<std::uintptr_t>(key.texture);
        return static_cast<std::size_t>(a ^ (static_cast<std::uintptr_t>(key.level) << 4));
    }
};

std::mutex g_texture_mu;
std::unordered_map<void*, TextureMeta> g_textures;
std::unordered_map<TextureLockKey, std::uint64_t, TextureLockKeyHash> g_texture_locks;
std::unordered_map<void*, SurfaceMeta> g_surfaces;
std::unordered_map<void*, std::uint64_t> g_surface_locks;
std::mutex g_runtime_device_mu;
std::unordered_map<void*, bool> g_runtime_devices;

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using Direct3DCreate9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(
    IDXGIAdapter*,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL*,
    UINT,
    UINT,
    const DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**,
    ID3D11Device**,
    D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);
using D3D9CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using D3D9ExCreateDeviceExFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using DevicePresentFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using DevicePresentExFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using DeviceCreateAdditionalSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**);
using DeviceGetSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, IDirect3DSwapChain9**);
using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using DeviceGetBackBufferFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9**);
using DeviceCreateTextureFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using DeviceCreateRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using DeviceUpdateSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const POINT*);
using DeviceUpdateTextureFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
using DeviceStretchRectFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using DeviceColorFillFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
using DeviceCreateOffscreenPlainSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**, HANDLE*);
using DeviceSetTextureFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DeviceSetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using DeviceClearFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using DeviceDrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DeviceDrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DeviceDrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DeviceDrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using TextureReleaseFn = ULONG(STDMETHODCALLTYPE*)(IDirect3DTexture9*);
using TextureLockRectFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using TextureUnlockRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT);
using SurfaceReleaseFn = ULONG(STDMETHODCALLTYPE*)(IDirect3DSurface9*);
using SurfaceLockRectFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD);
using SurfaceUnlockRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*);
using DxgiPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using DxgiPresent1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using D3D11CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using D3D11DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using D3D11DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using D3D11DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using D3D11DrawInstancedFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using D3D11OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using D3D11DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using D3D11UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using D3D11ClearRenderTargetViewFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);

std::uint64_t now_ns() {
    static const long long freq_value = [] {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    if (freq_value <= 0) return 0;
    return static_cast<std::uint64_t>(
        (static_cast<long double>(counter.QuadPart) * 1000000000.0L) /
        static_cast<long double>(freq_value));
}

bool read_vtable_slot(void* object, std::uint32_t slot_index, void*** out_slot) {
    if (object == nullptr || out_slot == nullptr) return false;
    __try {
        auto** vtable = *reinterpret_cast<void***>(object);
        if (vtable == nullptr || vtable[slot_index] == nullptr) return false;
        *out_slot = &vtable[slot_index];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* original_for(void* self, std::uint32_t slot_index) {
    void** slot = nullptr;
    if (!read_vtable_slot(self, slot_index, &slot)) return nullptr;
    for (auto& record : g_vtable_patches) {
        void** patched_slot = record.slot.load(std::memory_order_acquire);
        if (patched_slot == nullptr) continue;
        if (patched_slot == slot && record.slot_index == slot_index) {
            return record.original.load(std::memory_order_acquire);
        }
    }
    return nullptr;
}

bool patch_vtable_slot(void* object, std::uint32_t slot_index, void* hook) {
    if (object == nullptr || hook == nullptr) return false;
    void** slot = nullptr;
    if (!read_vtable_slot(object, slot_index, &slot)) return false;

    std::lock_guard<std::mutex> lock(g_patch_mu);
    if (*slot == hook) return true;
    for (auto& record : g_vtable_patches) {
        if (record.slot.load(std::memory_order_acquire) == slot) return true;
    }

    VtablePatchRecord* free_record = nullptr;
    for (auto& record : g_vtable_patches) {
        if (record.slot.load(std::memory_order_acquire) == nullptr) {
            free_record = &record;
            break;
        }
    }
    if (free_record == nullptr) return false;

    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_prot)) {
        return false;
    }
    void* original = *slot;
    *slot = hook;
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old_prot, &tmp);

    free_record->slot_index = slot_index;
    free_record->original.store(original, std::memory_order_release);
    free_record->slot.store(slot, std::memory_order_release);
    g_hook_installs.fetch_add(1, std::memory_order_relaxed);
    return original != nullptr;
}

void restore_vtable_patches() {
    std::lock_guard<std::mutex> lock(g_patch_mu);
    for (auto& record : g_vtable_patches) {
        void** slot = record.slot.load(std::memory_order_acquire);
        if (slot == nullptr) continue;
        void* original = record.original.load(std::memory_order_acquire);
        DWORD old_prot = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_prot)) {
            *slot = original;
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            DWORD tmp = 0;
            VirtualProtect(slot, sizeof(void*), old_prot, &tmp);
        }
        record.slot.store(nullptr, std::memory_order_release);
        record.original.store(nullptr, std::memory_order_release);
        record.slot_index = 0;
    }
}

std::uint64_t dxt_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t block_bytes) {
    const std::uint64_t bw = std::max<std::uint32_t>(1, (width + 3u) / 4u);
    const std::uint64_t bh = std::max<std::uint32_t>(1, (height + 3u) / 4u);
    return bw * bh * block_bytes;
}

std::uint64_t level_bytes(std::uint32_t width, std::uint32_t height, D3DFORMAT format) {
    switch (format) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
        case D3DFMT_G16R16:
            return static_cast<std::uint64_t>(width) * height * 4ull;
        case D3DFMT_R8G8B8:
            return static_cast<std::uint64_t>(width) * height * 3ull;
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_A8L8:
        case D3DFMT_L16:
            return static_cast<std::uint64_t>(width) * height * 2ull;
        case D3DFMT_A8:
        case D3DFMT_L8:
            return static_cast<std::uint64_t>(width) * height;
        case D3DFMT_DXT1:
            return dxt_bytes(width, height, 8);
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:
            return dxt_bytes(width, height, 16);
        default:
            return 0;
    }
}

std::uint32_t normalized_levels(std::uint32_t width, std::uint32_t height, std::uint32_t levels);

std::uint64_t dxgi_level_bytes(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return static_cast<std::uint64_t>(width) * height * 16ull;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
            return static_cast<std::uint64_t>(width) * height * 8ull;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
            return static_cast<std::uint64_t>(width) * height * 4ull;
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
            return static_cast<std::uint64_t>(width) * height * 2ull;
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
            return static_cast<std::uint64_t>(width) * height;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            return dxt_bytes(width, height, 8);
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return dxt_bytes(width, height, 16);
        default:
            return 0;
    }
}

std::uint64_t estimate_d3d11_texture_bytes(const D3D11_TEXTURE2D_DESC& desc) {
    if (desc.Width == 0 || desc.Height == 0) return 0;
    const std::uint32_t levels = desc.MipLevels == 0
        ? normalized_levels(desc.Width, desc.Height, 0)
        : desc.MipLevels;
    std::uint64_t total = 0;
    std::uint32_t width = desc.Width;
    std::uint32_t height = desc.Height;
    for (std::uint32_t i = 0; i < levels; ++i) {
        total += dxgi_level_bytes(width, height, desc.Format);
        width = std::max<std::uint32_t>(1, width / 2);
        height = std::max<std::uint32_t>(1, height / 2);
    }
    return total * std::max<UINT>(1, desc.ArraySize);
}

std::uint64_t d3d11_update_bytes(ID3D11Resource* resource,
                                 const D3D11_BOX* box,
                                 UINT src_row_pitch,
                                 UINT src_depth_pitch) {
    if (box != nullptr && src_row_pitch != 0) {
        const UINT rows = box->bottom > box->top ? box->bottom - box->top : 1;
        const UINT depth = box->back > box->front ? box->back - box->front : 1;
        return static_cast<std::uint64_t>(src_row_pitch) * rows * depth;
    }
    if (src_depth_pitch != 0) return src_depth_pitch;
    if (resource == nullptr) return 0;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        auto* texture = static_cast<ID3D11Texture2D*>(resource);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        return estimate_d3d11_texture_bytes(desc);
    }
    return 0;
}

std::uint32_t normalized_levels(std::uint32_t width, std::uint32_t height, std::uint32_t levels) {
    if (levels != 0) return levels;
    std::uint32_t out = 1;
    while ((width > 1 || height > 1) && out < 32) {
        width = std::max<std::uint32_t>(1, width / 2);
        height = std::max<std::uint32_t>(1, height / 2);
        ++out;
    }
    return out;
}

std::uint64_t estimate_texture_bytes(std::uint32_t width,
                                     std::uint32_t height,
                                     std::uint32_t levels,
                                     D3DFORMAT format) {
    if (width == 0 || height == 0) return 0;
    const std::uint32_t level_count = normalized_levels(width, height, levels);
    std::uint64_t total = 0;
    std::uint32_t w = width;
    std::uint32_t h = height;
    for (std::uint32_t i = 0; i < level_count; ++i) {
        total += level_bytes(w, h, format);
        w = std::max<std::uint32_t>(1, w / 2);
        h = std::max<std::uint32_t>(1, h / 2);
    }
    return total;
}

std::uint64_t estimate_texture_level_bytes(const TextureMeta& meta, std::uint32_t level) {
    std::uint32_t w = meta.width;
    std::uint32_t h = meta.height;
    const std::uint32_t level_count = normalized_levels(meta.width, meta.height, meta.levels);
    if (level >= level_count) return 0;
    for (std::uint32_t i = 0; i < level; ++i) {
        w = std::max<std::uint32_t>(1, w / 2);
        h = std::max<std::uint32_t>(1, h / 2);
    }
    return level_bytes(w, h, meta.format);
}

std::uint64_t texture_bytes(void* texture) {
    if (texture == nullptr) return 0;
    std::lock_guard<std::mutex> lock(g_texture_mu);
    auto it = g_textures.find(texture);
    return it == g_textures.end() ? 0 : it->second.total_bytes;
}

std::uint64_t surface_region_bytes(const SurfaceMeta& meta, const RECT* rect) {
    std::uint32_t width = meta.width;
    std::uint32_t height = meta.height;
    if (rect != nullptr && rect->right > rect->left && rect->bottom > rect->top) {
        width = static_cast<std::uint32_t>(rect->right - rect->left);
        height = static_cast<std::uint32_t>(rect->bottom - rect->top);
    }
    return level_bytes(width, height, meta.format);
}

std::uint64_t surface_bytes(void* surface, const RECT* rect = nullptr) {
    if (surface == nullptr) return 0;
    std::lock_guard<std::mutex> lock(g_texture_mu);
    auto it = g_surfaces.find(surface);
    if (it == g_surfaces.end()) return 0;
    return rect == nullptr ? it->second.total_bytes : surface_region_bytes(it->second, rect);
}

bool render_capture_active() {
    return g_render_capture_active.load(std::memory_order_relaxed);
}

void add_texture_upload(std::uint64_t bytes) {
    if (!render_capture_active()) return;
    g_acc_texture_update_count.fetch_add(1, std::memory_order_relaxed);
    g_texture_updates_total.fetch_add(1, std::memory_order_relaxed);
    if (bytes != 0) {
        g_acc_texture_upload_bytes.fetch_add(bytes, std::memory_order_relaxed);
        g_texture_upload_bytes_total.fetch_add(bytes, std::memory_order_relaxed);
    }
}

void add_texture_create(std::uint64_t bytes) {
    if (!render_capture_active()) return;
    g_acc_texture_create_count.fetch_add(1, std::memory_order_relaxed);
    g_texture_creates_total.fetch_add(1, std::memory_order_relaxed);
    if (bytes != 0) {
        g_acc_texture_create_bytes.fetch_add(bytes, std::memory_order_relaxed);
        g_texture_create_bytes_total.fetch_add(bytes, std::memory_order_relaxed);
    }
}

void add_draw_call(std::uint64_t primitives) {
    if (!render_capture_active()) return;
    g_acc_draw_calls.fetch_add(1, std::memory_order_relaxed);
    g_acc_primitives.fetch_add(primitives, std::memory_order_relaxed);
    g_draw_calls_total.fetch_add(1, std::memory_order_relaxed);
    g_primitives_total.fetch_add(primitives, std::memory_order_relaxed);
}

bool patch_device_vtable(IDirect3DDevice9* device);
bool patch_device_ex_vtable(IDirect3DDevice9Ex* device);
bool patch_swapchain_vtable(IDirect3DSwapChain9* swap_chain);
bool patch_texture_vtable(IDirect3DTexture9* texture);
bool patch_surface_vtable(IDirect3DSurface9* surface);
bool patch_dxgi_swapchain_vtable(IDXGISwapChain* swap_chain);
bool patch_dxgi_swapchain1_vtable(IDXGISwapChain1* swap_chain);
bool patch_d3d11_device_vtable(ID3D11Device* device);
bool patch_d3d11_context_vtable(ID3D11DeviceContext* context);

void remember_surface(IDirect3DSurface9* surface) {
    if (surface == nullptr) return;
    if (!render_capture_active()) return;
    D3DSURFACE_DESC desc{};
    if (FAILED(surface->GetDesc(&desc))) return;
    const auto total_bytes = estimate_texture_bytes(desc.Width, desc.Height, 1, desc.Format);
    {
        std::lock_guard<std::mutex> lock(g_texture_mu);
        g_surfaces[surface] = SurfaceMeta{
            desc.Width,
            desc.Height,
            desc.Format,
            total_bytes,
        };
    }
    patch_surface_vtable(surface);
}

void patch_runtime_device_once(IDirect3DDevice9* device) {
    if (device == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(g_runtime_device_mu);
        if (g_runtime_devices.find(device) != g_runtime_devices.end()) return;
        g_runtime_devices[device] = true;
    }
    patch_device_vtable(device);
}

void record_present_frame(const char* label,
                          std::uint64_t start_ns,
                          std::uint64_t end_ns,
                          std::uint32_t result) {
    if (!render_capture_active()) return;
    const std::uint64_t present_ns = end_ns >= start_ns ? end_ns - start_ns : 0;
    g_present_calls.fetch_add(1, std::memory_order_relaxed);

    if (g_inside_hook) return;
    auto* ctrl = g_controller.load(std::memory_order_acquire);
    if (ctrl == nullptr) return;

    const std::uint64_t previous_end =
        g_last_present_end_ns.exchange(end_ns, std::memory_order_acq_rel);
    const std::uint64_t interval_ns =
        previous_end == 0 || end_ns < previous_end ? present_ns : end_ns - previous_end;
    const std::uint64_t cpu_ns = interval_ns > present_ns ? interval_ns - present_ns : 0;
    const std::uint64_t draw_calls =
        g_acc_draw_calls.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t primitive_count =
        g_acc_primitives.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t upload_bytes =
        g_acc_texture_upload_bytes.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t create_bytes =
        g_acc_texture_create_bytes.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t create_count =
        g_acc_texture_create_count.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t update_count =
        g_acc_texture_update_count.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t set_texture_count =
        g_acc_set_texture_count.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t target_changes =
        g_acc_render_target_change_count.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t clear_count =
        g_acc_clear_count.exchange(0, std::memory_order_acq_rel);

    g_inside_hook = true;
    ctrl->record_render_frame(g_frame_index.fetch_add(1, std::memory_order_relaxed),
                              interval_ns,
                              cpu_ns,
                              present_ns,
                              draw_calls,
                              primitive_count,
                              upload_bytes,
                              create_bytes,
                              create_count,
                              update_count,
                              set_texture_count,
                              target_changes,
                              clear_count,
                              result,
                              label == nullptr ? "" : label);
    g_inside_hook = false;
    g_render_frames_total.fetch_add(1, std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE hook_d3d9_create_device(IDirect3D9* self,
                                                  UINT adapter,
                                                  D3DDEVTYPE device_type,
                                                  HWND focus_window,
                                                  DWORD behavior_flags,
                                                  D3DPRESENT_PARAMETERS* presentation_parameters,
                                                  IDirect3DDevice9** returned_device) {
    auto* real = reinterpret_cast<D3D9CreateDeviceFn>(
        original_for(self, kSlotD3D9CreateDevice));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self,
                            adapter,
                            device_type,
                            focus_window,
                            behavior_flags,
                            presentation_parameters,
                            returned_device);
    if (SUCCEEDED(hr) && returned_device != nullptr && *returned_device != nullptr) {
        patch_device_vtable(*returned_device);
        IDirect3DSwapChain9* swap_chain = nullptr;
        if (SUCCEEDED((*returned_device)->GetSwapChain(0, &swap_chain)) && swap_chain != nullptr) {
            patch_swapchain_vtable(swap_chain);
            swap_chain->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_d3d9ex_create_device_ex(IDirect3D9Ex* self,
                                                       UINT adapter,
                                                       D3DDEVTYPE device_type,
                                                       HWND focus_window,
                                                       DWORD behavior_flags,
                                                       D3DPRESENT_PARAMETERS* presentation_parameters,
                                                       D3DDISPLAYMODEEX* fullscreen_display_mode,
                                                       IDirect3DDevice9Ex** returned_device) {
    auto* real = reinterpret_cast<D3D9ExCreateDeviceExFn>(
        original_for(self, kSlotD3D9ExCreateDeviceEx));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self,
                            adapter,
                            device_type,
                            focus_window,
                            behavior_flags,
                               presentation_parameters,
                               fullscreen_display_mode,
                               returned_device);
    if (SUCCEEDED(hr) && returned_device != nullptr && *returned_device != nullptr) {
        patch_device_ex_vtable(*returned_device);
        IDirect3DSwapChain9* swap_chain = nullptr;
        if (SUCCEEDED((*returned_device)->GetSwapChain(0, &swap_chain)) && swap_chain != nullptr) {
            patch_swapchain_vtable(swap_chain);
            swap_chain->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_present(IDirect3DDevice9* self,
                                       const RECT* source_rect,
                                       const RECT* dest_rect,
                                       HWND dest_window_override,
                                       const RGNDATA* dirty_region) {
    auto* real = reinterpret_cast<DevicePresentFn>(original_for(self, kSlotDevicePresent));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const std::uint64_t start_ns = now_ns();
    const HRESULT hr = real(self, source_rect, dest_rect, dest_window_override, dirty_region);
    const std::uint64_t end_ns = now_ns();
    record_present_frame("d3d9.device.present", start_ns, end_ns, static_cast<std::uint32_t>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_present_ex(IDirect3DDevice9Ex* self,
                                          const RECT* source_rect,
                                          const RECT* dest_rect,
                                          HWND dest_window_override,
                                          const RGNDATA* dirty_region,
                                          DWORD flags) {
    auto* real = reinterpret_cast<DevicePresentExFn>(original_for(self, kSlotDevicePresentEx));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const std::uint64_t start_ns = now_ns();
    const HRESULT hr = real(self, source_rect, dest_rect, dest_window_override, dirty_region, flags);
    const std::uint64_t end_ns = now_ns();
    record_present_frame("d3d9.device.present_ex", start_ns, end_ns, static_cast<std::uint32_t>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_swapchain_present(IDirect3DSwapChain9* self,
                                                const RECT* source_rect,
                                                const RECT* dest_rect,
                                                HWND dest_window_override,
                                                const RGNDATA* dirty_region,
                                                DWORD flags) {
    auto* real = reinterpret_cast<SwapChainPresentFn>(
        original_for(self, kSlotSwapChainPresent));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    IDirect3DDevice9* device = nullptr;
    if (SUCCEEDED(self->GetDevice(&device)) && device != nullptr) {
        patch_runtime_device_once(device);
        device->Release();
    }
    const std::uint64_t start_ns = now_ns();
    const HRESULT hr = real(self, source_rect, dest_rect, dest_window_override, dirty_region, flags);
    const std::uint64_t end_ns = now_ns();
    record_present_frame("d3d9.swapchain.present", start_ns, end_ns, static_cast<std::uint32_t>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_additional_swap_chain(IDirect3DDevice9* self,
                                                           D3DPRESENT_PARAMETERS* presentation_parameters,
                                                           IDirect3DSwapChain9** swap_chain) {
    auto* real = reinterpret_cast<DeviceCreateAdditionalSwapChainFn>(
        original_for(self, kSlotDeviceCreateAdditionalSwapChain));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self, presentation_parameters, swap_chain);
    if (SUCCEEDED(hr) && swap_chain != nullptr && *swap_chain != nullptr) {
        patch_swapchain_vtable(*swap_chain);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_get_swap_chain(IDirect3DDevice9* self,
                                             UINT swap_chain_index,
                                             IDirect3DSwapChain9** swap_chain) {
    auto* real = reinterpret_cast<DeviceGetSwapChainFn>(
        original_for(self, kSlotDeviceGetSwapChain));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self, swap_chain_index, swap_chain);
    if (SUCCEEDED(hr) && swap_chain != nullptr && *swap_chain != nullptr) {
        patch_swapchain_vtable(*swap_chain);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_dxgi_present(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    auto* real = reinterpret_cast<DxgiPresentFn>(
        original_for(self, kSlotDxgiSwapChainPresent));
    if (real == nullptr) return DXGI_ERROR_INVALID_CALL;
    const std::uint64_t start_ns = now_ns();
    const HRESULT hr = real(self, sync_interval, flags);
    const std::uint64_t end_ns = now_ns();
    record_present_frame("dxgi.present", start_ns, end_ns, static_cast<std::uint32_t>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_dxgi_present1(IDXGISwapChain1* self,
                                             UINT sync_interval,
                                             UINT present_flags,
                                             const DXGI_PRESENT_PARAMETERS* present_parameters) {
    auto* real = reinterpret_cast<DxgiPresent1Fn>(
        original_for(self, kSlotDxgiSwapChain1Present1));
    if (real == nullptr) return DXGI_ERROR_INVALID_CALL;
    const std::uint64_t start_ns = now_ns();
    const HRESULT hr = real(self, sync_interval, present_flags, present_parameters);
    const std::uint64_t end_ns = now_ns();
    record_present_frame("dxgi.present1", start_ns, end_ns, static_cast<std::uint32_t>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_texture(IDirect3DDevice9* self,
                                              UINT width,
                                              UINT height,
                                              UINT levels,
                                              DWORD usage,
                                              D3DFORMAT format,
                                              D3DPOOL pool,
                                              IDirect3DTexture9** texture,
                                              HANDLE* shared_handle) {
    auto* real = reinterpret_cast<DeviceCreateTextureFn>(
        original_for(self, kSlotDeviceCreateTexture));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self, width, height, levels, usage, format, pool, texture, shared_handle);
    if (SUCCEEDED(hr) && texture != nullptr && *texture != nullptr) {
        const auto total_bytes = estimate_texture_bytes(width, height, levels, format);
        {
            std::lock_guard<std::mutex> lock(g_texture_mu);
            g_textures[*texture] = TextureMeta{
                width,
                height,
                levels,
                format,
                total_bytes,
            };
        }
        patch_texture_vtable(*texture);
        add_texture_create(total_bytes);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_get_back_buffer(IDirect3DDevice9* self,
                                              UINT swap_chain,
                                              UINT back_buffer,
                                              D3DBACKBUFFER_TYPE type,
                                              IDirect3DSurface9** surface) {
    auto* real = reinterpret_cast<DeviceGetBackBufferFn>(
        original_for(self, kSlotDeviceGetBackBuffer));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self, swap_chain, back_buffer, type, surface);
    if (SUCCEEDED(hr) && surface != nullptr && *surface != nullptr) {
        remember_surface(*surface);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_render_target(IDirect3DDevice9* self,
                                                   UINT width,
                                                   UINT height,
                                                   D3DFORMAT format,
                                                   D3DMULTISAMPLE_TYPE multi_sample,
                                                   DWORD multisample_quality,
                                                   BOOL lockable,
                                                   IDirect3DSurface9** surface,
                                                   HANDLE* shared_handle) {
    auto* real = reinterpret_cast<DeviceCreateRenderTargetFn>(
        original_for(self, kSlotDeviceCreateRenderTarget));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self,
                            width,
                            height,
                            format,
                            multi_sample,
                            multisample_quality,
                            lockable,
                            surface,
                            shared_handle);
    if (SUCCEEDED(hr) && surface != nullptr && *surface != nullptr) {
        remember_surface(*surface);
        add_texture_create(surface_bytes(*surface));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_offscreen_plain_surface(IDirect3DDevice9* self,
                                                             UINT width,
                                                             UINT height,
                                                             D3DFORMAT format,
                                                             D3DPOOL pool,
                                                             IDirect3DSurface9** surface,
                                                             HANDLE* shared_handle) {
    auto* real = reinterpret_cast<DeviceCreateOffscreenPlainSurfaceFn>(
        original_for(self, kSlotDeviceCreateOffscreenPlainSurface));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self, width, height, format, pool, surface, shared_handle);
    if (SUCCEEDED(hr) && surface != nullptr && *surface != nullptr) {
        remember_surface(*surface);
        add_texture_create(surface_bytes(*surface));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_update_surface(IDirect3DDevice9* self,
                                             IDirect3DSurface9* source_surface,
                                             const RECT* source_rect,
                                             IDirect3DSurface9* dest_surface,
                                             const POINT* dest_point) {
    auto* real = reinterpret_cast<DeviceUpdateSurfaceFn>(
        original_for(self, kSlotDeviceUpdateSurface));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    remember_surface(source_surface);
    remember_surface(dest_surface);
    const HRESULT hr = real(self, source_surface, source_rect, dest_surface, dest_point);
    if (SUCCEEDED(hr)) {
        std::uint64_t bytes = surface_bytes(source_surface, source_rect);
        if (bytes == 0) bytes = surface_bytes(dest_surface, nullptr);
        add_texture_upload(bytes);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_stretch_rect(IDirect3DDevice9* self,
                                           IDirect3DSurface9* source_surface,
                                           const RECT* source_rect,
                                           IDirect3DSurface9* dest_surface,
                                           const RECT* dest_rect,
                                           D3DTEXTUREFILTERTYPE filter) {
    auto* real = reinterpret_cast<DeviceStretchRectFn>(
        original_for(self, kSlotDeviceStretchRect));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    remember_surface(source_surface);
    remember_surface(dest_surface);
    const HRESULT hr = real(self, source_surface, source_rect, dest_surface, dest_rect, filter);
    if (SUCCEEDED(hr)) {
        add_draw_call(2);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_color_fill(IDirect3DDevice9* self,
                                         IDirect3DSurface9* surface,
                                         const RECT* rect,
                                         D3DCOLOR color) {
    auto* real = reinterpret_cast<DeviceColorFillFn>(
        original_for(self, kSlotDeviceColorFill));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    remember_surface(surface);
    const HRESULT hr = real(self, surface, rect, color);
    if (SUCCEEDED(hr) && render_capture_active()) {
        g_acc_clear_count.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_update_texture(IDirect3DDevice9* self,
                                             IDirect3DBaseTexture9* source_texture,
                                             IDirect3DBaseTexture9* dest_texture) {
    auto* real = reinterpret_cast<DeviceUpdateTextureFn>(
        original_for(self, kSlotDeviceUpdateTexture));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self, source_texture, dest_texture);
    if (SUCCEEDED(hr)) {
        add_texture_upload(texture_bytes(dest_texture));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_set_texture(IDirect3DDevice9* self,
                                          DWORD stage,
                                          IDirect3DBaseTexture9* texture) {
    auto* real = reinterpret_cast<DeviceSetTextureFn>(original_for(self, kSlotDeviceSetTexture));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    if (render_capture_active()) {
        g_acc_set_texture_count.fetch_add(1, std::memory_order_relaxed);
    }
    return real(self, stage, texture);
}

HRESULT STDMETHODCALLTYPE hook_set_render_target(IDirect3DDevice9* self,
                                                DWORD render_target_index,
                                                IDirect3DSurface9* render_target) {
    auto* real = reinterpret_cast<DeviceSetRenderTargetFn>(
        original_for(self, kSlotDeviceSetRenderTarget));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    if (render_capture_active()) {
        g_acc_render_target_change_count.fetch_add(1, std::memory_order_relaxed);
    }
    return real(self, render_target_index, render_target);
}

HRESULT STDMETHODCALLTYPE hook_clear(IDirect3DDevice9* self,
                                    DWORD count,
                                    const D3DRECT* rects,
                                    DWORD flags,
                                    D3DCOLOR color,
                                    float z,
                                    DWORD stencil) {
    auto* real = reinterpret_cast<DeviceClearFn>(original_for(self, kSlotDeviceClear));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    if (render_capture_active()) {
        g_acc_clear_count.fetch_add(1, std::memory_order_relaxed);
    }
    return real(self, count, rects, flags, color, z, stencil);
}

HRESULT STDMETHODCALLTYPE hook_draw_primitive(IDirect3DDevice9* self,
                                             D3DPRIMITIVETYPE primitive_type,
                                             UINT start_vertex,
                                             UINT primitive_count) {
    auto* real = reinterpret_cast<DeviceDrawPrimitiveFn>(
        original_for(self, kSlotDeviceDrawPrimitive));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    add_draw_call(primitive_count);
    return real(self, primitive_type, start_vertex, primitive_count);
}

HRESULT STDMETHODCALLTYPE hook_draw_indexed_primitive(IDirect3DDevice9* self,
                                                     D3DPRIMITIVETYPE primitive_type,
                                                     INT base_vertex_index,
                                                     UINT min_vertex_index,
                                                     UINT num_vertices,
                                                     UINT start_index,
                                                     UINT primitive_count) {
    auto* real = reinterpret_cast<DeviceDrawIndexedPrimitiveFn>(
        original_for(self, kSlotDeviceDrawIndexedPrimitive));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    add_draw_call(primitive_count);
    return real(self,
                primitive_type,
                base_vertex_index,
                min_vertex_index,
                num_vertices,
                start_index,
                primitive_count);
}

HRESULT STDMETHODCALLTYPE hook_draw_primitive_up(IDirect3DDevice9* self,
                                                D3DPRIMITIVETYPE primitive_type,
                                                UINT primitive_count,
                                                const void* vertex_stream_zero_data,
                                                UINT vertex_stream_zero_stride) {
    auto* real = reinterpret_cast<DeviceDrawPrimitiveUPFn>(
        original_for(self, kSlotDeviceDrawPrimitiveUP));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    add_draw_call(primitive_count);
    return real(self,
                primitive_type,
                primitive_count,
                vertex_stream_zero_data,
                vertex_stream_zero_stride);
}

HRESULT STDMETHODCALLTYPE hook_draw_indexed_primitive_up(IDirect3DDevice9* self,
                                                        D3DPRIMITIVETYPE primitive_type,
                                                        UINT min_vertex_index,
                                                        UINT num_vertices,
                                                        UINT primitive_count,
                                                        const void* index_data,
                                                        D3DFORMAT index_data_format,
                                                        const void* vertex_stream_zero_data,
                                                        UINT vertex_stream_zero_stride) {
    auto* real = reinterpret_cast<DeviceDrawIndexedPrimitiveUPFn>(
        original_for(self, kSlotDeviceDrawIndexedPrimitiveUP));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    add_draw_call(primitive_count);
    return real(self,
                primitive_type,
                min_vertex_index,
                num_vertices,
                primitive_count,
                index_data,
                index_data_format,
                vertex_stream_zero_data,
                vertex_stream_zero_stride);
}

HRESULT STDMETHODCALLTYPE hook_d3d11_create_texture_2d(ID3D11Device* self,
                                                       const D3D11_TEXTURE2D_DESC* desc,
                                                       const D3D11_SUBRESOURCE_DATA* initial_data,
                                                       ID3D11Texture2D** texture) {
    auto* real = reinterpret_cast<D3D11CreateTexture2DFn>(
        original_for(self, kSlotD3D11DeviceCreateTexture2D));
    if (real == nullptr) return E_FAIL;
    const HRESULT hr = real(self, desc, initial_data, texture);
    if (SUCCEEDED(hr) && desc != nullptr) {
        const std::uint64_t bytes = estimate_d3d11_texture_bytes(*desc);
        add_texture_create(bytes);
        if (initial_data != nullptr) {
            add_texture_upload(bytes);
        }
    }
    return hr;
}

void STDMETHODCALLTYPE hook_d3d11_draw_indexed(ID3D11DeviceContext* self,
                                               UINT index_count,
                                               UINT start_index_location,
                                               INT base_vertex_location) {
    auto* real = reinterpret_cast<D3D11DrawIndexedFn>(
        original_for(self, kSlotD3D11ContextDrawIndexed));
    if (real == nullptr) return;
    add_draw_call(index_count / 3u);
    real(self, index_count, start_index_location, base_vertex_location);
}

void STDMETHODCALLTYPE hook_d3d11_draw(ID3D11DeviceContext* self,
                                       UINT vertex_count,
                                       UINT start_vertex_location) {
    auto* real = reinterpret_cast<D3D11DrawFn>(
        original_for(self, kSlotD3D11ContextDraw));
    if (real == nullptr) return;
    add_draw_call(vertex_count / 3u);
    real(self, vertex_count, start_vertex_location);
}

void STDMETHODCALLTYPE hook_d3d11_draw_indexed_instanced(ID3D11DeviceContext* self,
                                                        UINT index_count_per_instance,
                                                        UINT instance_count,
                                                        UINT start_index_location,
                                                        INT base_vertex_location,
                                                        UINT start_instance_location) {
    auto* real = reinterpret_cast<D3D11DrawIndexedInstancedFn>(
        original_for(self, kSlotD3D11ContextDrawIndexedInstanced));
    if (real == nullptr) return;
    const std::uint64_t primitives =
        (static_cast<std::uint64_t>(index_count_per_instance) * instance_count) / 3u;
    add_draw_call(primitives);
    real(self,
         index_count_per_instance,
         instance_count,
         start_index_location,
         base_vertex_location,
         start_instance_location);
}

void STDMETHODCALLTYPE hook_d3d11_draw_instanced(ID3D11DeviceContext* self,
                                                UINT vertex_count_per_instance,
                                                UINT instance_count,
                                                UINT start_vertex_location,
                                                UINT start_instance_location) {
    auto* real = reinterpret_cast<D3D11DrawInstancedFn>(
        original_for(self, kSlotD3D11ContextDrawInstanced));
    if (real == nullptr) return;
    const std::uint64_t primitives =
        (static_cast<std::uint64_t>(vertex_count_per_instance) * instance_count) / 3u;
    add_draw_call(primitives);
    real(self,
         vertex_count_per_instance,
         instance_count,
         start_vertex_location,
         start_instance_location);
}

void STDMETHODCALLTYPE hook_d3d11_om_set_render_targets(ID3D11DeviceContext* self,
                                                       UINT num_views,
                                                       ID3D11RenderTargetView* const* views,
                                                       ID3D11DepthStencilView* depth_stencil_view) {
    auto* real = reinterpret_cast<D3D11OMSetRenderTargetsFn>(
        original_for(self, kSlotD3D11ContextOMSetRenderTargets));
    if (real == nullptr) return;
    if (render_capture_active()) {
        g_acc_render_target_change_count.fetch_add(1, std::memory_order_relaxed);
    }
    real(self, num_views, views, depth_stencil_view);
}

void STDMETHODCALLTYPE hook_d3d11_draw_auto(ID3D11DeviceContext* self) {
    auto* real = reinterpret_cast<D3D11DrawAutoFn>(
        original_for(self, kSlotD3D11ContextDrawAuto));
    if (real == nullptr) return;
    add_draw_call(0);
    real(self);
}

void STDMETHODCALLTYPE hook_d3d11_update_subresource(ID3D11DeviceContext* self,
                                                    ID3D11Resource* dst_resource,
                                                    UINT dst_subresource,
                                                    const D3D11_BOX* dst_box,
                                                    const void* src_data,
                                                    UINT src_row_pitch,
                                                    UINT src_depth_pitch) {
    auto* real = reinterpret_cast<D3D11UpdateSubresourceFn>(
        original_for(self, kSlotD3D11ContextUpdateSubresource));
    if (real == nullptr) return;
    if (src_data != nullptr) {
        add_texture_upload(d3d11_update_bytes(dst_resource, dst_box, src_row_pitch, src_depth_pitch));
    }
    real(self, dst_resource, dst_subresource, dst_box, src_data, src_row_pitch, src_depth_pitch);
}

void STDMETHODCALLTYPE hook_d3d11_clear_render_target_view(ID3D11DeviceContext* self,
                                                          ID3D11RenderTargetView* view,
                                                          const FLOAT color[4]) {
    auto* real = reinterpret_cast<D3D11ClearRenderTargetViewFn>(
        original_for(self, kSlotD3D11ContextClearRenderTargetView));
    if (real == nullptr) return;
    if (render_capture_active()) {
        g_acc_clear_count.fetch_add(1, std::memory_order_relaxed);
    }
    real(self, view, color);
}

ULONG STDMETHODCALLTYPE hook_texture_release(IDirect3DTexture9* self) {
    auto* real = reinterpret_cast<TextureReleaseFn>(original_for(self, kSlotIUnknownRelease));
    if (real == nullptr) return 0;
    const ULONG refs = real(self);
    if (refs == 0 && render_capture_active()) {
        std::lock_guard<std::mutex> lock(g_texture_mu);
        g_textures.erase(self);
        for (auto it = g_texture_locks.begin(); it != g_texture_locks.end();) {
            if (it->first.texture == self) {
                it = g_texture_locks.erase(it);
            } else {
                ++it;
            }
        }
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE hook_texture_lock_rect(IDirect3DTexture9* self,
                                                UINT level,
                                                D3DLOCKED_RECT* locked_rect,
                                                const RECT* rect,
                                                DWORD flags) {
    auto* real = reinterpret_cast<TextureLockRectFn>(original_for(self, kSlotTextureLockRect));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self, level, locked_rect, rect, flags);
    if (SUCCEEDED(hr) && render_capture_active() && (flags & D3DLOCK_READONLY) == 0) {
        std::uint64_t bytes = 0;
        {
            std::lock_guard<std::mutex> lock(g_texture_mu);
            auto it = g_textures.find(self);
            if (it != g_textures.end()) {
                bytes = estimate_texture_level_bytes(it->second, level);
            }
            g_texture_locks[TextureLockKey{self, level}] = bytes;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_texture_unlock_rect(IDirect3DTexture9* self, UINT level) {
    auto* real = reinterpret_cast<TextureUnlockRectFn>(original_for(self, kSlotTextureUnlockRect));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self, level);
    if (SUCCEEDED(hr) && render_capture_active()) {
        std::uint64_t bytes = 0;
        {
            std::lock_guard<std::mutex> lock(g_texture_mu);
            const TextureLockKey key{self, level};
            auto it = g_texture_locks.find(key);
            if (it != g_texture_locks.end()) {
                bytes = it->second;
                g_texture_locks.erase(it);
            }
        }
        add_texture_upload(bytes);
    }
    return hr;
}

ULONG STDMETHODCALLTYPE hook_surface_release(IDirect3DSurface9* self) {
    auto* real = reinterpret_cast<SurfaceReleaseFn>(original_for(self, kSlotIUnknownRelease));
    if (real == nullptr) return 0;
    const ULONG refs = real(self);
    if (refs == 0 && render_capture_active()) {
        std::lock_guard<std::mutex> lock(g_texture_mu);
        g_surfaces.erase(self);
        g_surface_locks.erase(self);
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE hook_surface_lock_rect(IDirect3DSurface9* self,
                                                D3DLOCKED_RECT* locked_rect,
                                                const RECT* rect,
                                                DWORD flags) {
    auto* real = reinterpret_cast<SurfaceLockRectFn>(
        original_for(self, kSlotSurfaceLockRect));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self, locked_rect, rect, flags);
    if (SUCCEEDED(hr) && render_capture_active() && (flags & D3DLOCK_READONLY) == 0) {
        const std::uint64_t bytes = surface_bytes(self, rect);
        std::lock_guard<std::mutex> lock(g_texture_mu);
        g_surface_locks[self] = bytes;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_surface_unlock_rect(IDirect3DSurface9* self) {
    auto* real = reinterpret_cast<SurfaceUnlockRectFn>(
        original_for(self, kSlotSurfaceUnlockRect));
    if (real == nullptr) return D3DERR_INVALIDCALL;
    const HRESULT hr = real(self);
    if (SUCCEEDED(hr) && render_capture_active()) {
        std::uint64_t bytes = 0;
        {
            std::lock_guard<std::mutex> lock(g_texture_mu);
            auto it = g_surface_locks.find(self);
            if (it != g_surface_locks.end()) {
                bytes = it->second;
                g_surface_locks.erase(it);
            }
        }
        add_texture_upload(bytes);
    }
    return hr;
}

bool patch_d3d9_vtable(IDirect3D9* d3d) {
    return patch_vtable_slot(d3d, kSlotD3D9CreateDevice, reinterpret_cast<void*>(&hook_d3d9_create_device));
}

bool patch_d3d9ex_vtable(IDirect3D9Ex* d3d) {
    bool ok = patch_vtable_slot(d3d, kSlotD3D9CreateDevice, reinterpret_cast<void*>(&hook_d3d9_create_device));
    ok = patch_vtable_slot(d3d, kSlotD3D9ExCreateDeviceEx, reinterpret_cast<void*>(&hook_d3d9ex_create_device_ex)) || ok;
    return ok;
}

bool patch_device_vtable(IDirect3DDevice9* device) {
    if (device == nullptr) return false;
    bool ok = false;
    ok = patch_vtable_slot(device,
                           kSlotDeviceCreateAdditionalSwapChain,
                           reinterpret_cast<void*>(&hook_create_additional_swap_chain)) || ok;
    ok = patch_vtable_slot(device,
                           kSlotDeviceGetSwapChain,
                           reinterpret_cast<void*>(&hook_get_swap_chain)) || ok;
    ok = patch_vtable_slot(device, kSlotDevicePresent, reinterpret_cast<void*>(&hook_present)) || ok;
    ok = patch_vtable_slot(device, kSlotDeviceCreateTexture, reinterpret_cast<void*>(&hook_create_texture)) || ok;
    ok = patch_vtable_slot(device, kSlotDeviceUpdateTexture, reinterpret_cast<void*>(&hook_update_texture)) || ok;
    ok = patch_vtable_slot(device, kSlotDeviceSetRenderTarget, reinterpret_cast<void*>(&hook_set_render_target)) || ok;
    ok = patch_vtable_slot(device, kSlotDeviceClear, reinterpret_cast<void*>(&hook_clear)) || ok;
    ok = patch_vtable_slot(device, kSlotDeviceSetTexture, reinterpret_cast<void*>(&hook_set_texture)) || ok;
    ok = patch_vtable_slot(device, kSlotDeviceDrawPrimitive, reinterpret_cast<void*>(&hook_draw_primitive)) || ok;
    ok = patch_vtable_slot(device, kSlotDeviceDrawIndexedPrimitive, reinterpret_cast<void*>(&hook_draw_indexed_primitive)) || ok;
    ok = patch_vtable_slot(device, kSlotDeviceDrawPrimitiveUP, reinterpret_cast<void*>(&hook_draw_primitive_up)) || ok;
    ok = patch_vtable_slot(device, kSlotDeviceDrawIndexedPrimitiveUP, reinterpret_cast<void*>(&hook_draw_indexed_primitive_up)) || ok;
    IDirect3DSwapChain9* swap_chain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &swap_chain)) && swap_chain != nullptr) {
        ok = patch_swapchain_vtable(swap_chain) || ok;
        swap_chain->Release();
    }
    if (ok) g_device_hook_installs.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool patch_device_ex_vtable(IDirect3DDevice9Ex* device) {
    if (device == nullptr) return false;
    bool ok = patch_device_vtable(reinterpret_cast<IDirect3DDevice9*>(device));
    ok = patch_vtable_slot(device, kSlotDevicePresentEx, reinterpret_cast<void*>(&hook_present_ex)) || ok;
    if (ok) g_device_hook_installs.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool patch_swapchain_vtable(IDirect3DSwapChain9* swap_chain) {
    if (swap_chain == nullptr) return false;
    return patch_vtable_slot(swap_chain,
                             kSlotSwapChainPresent,
                             reinterpret_cast<void*>(&hook_swapchain_present));
}

bool patch_texture_vtable(IDirect3DTexture9* texture) {
    if (texture == nullptr) return false;
    bool ok = false;
    ok = patch_vtable_slot(texture, kSlotIUnknownRelease, reinterpret_cast<void*>(&hook_texture_release)) || ok;
    ok = patch_vtable_slot(texture, kSlotTextureLockRect, reinterpret_cast<void*>(&hook_texture_lock_rect)) || ok;
    ok = patch_vtable_slot(texture, kSlotTextureUnlockRect, reinterpret_cast<void*>(&hook_texture_unlock_rect)) || ok;
    if (ok) g_texture_hook_installs.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool patch_surface_vtable(IDirect3DSurface9* surface) {
    if (surface == nullptr) return false;
    bool ok = false;
    ok = patch_vtable_slot(surface, kSlotIUnknownRelease, reinterpret_cast<void*>(&hook_surface_release)) || ok;
    ok = patch_vtable_slot(surface, kSlotSurfaceLockRect, reinterpret_cast<void*>(&hook_surface_lock_rect)) || ok;
    ok = patch_vtable_slot(surface, kSlotSurfaceUnlockRect, reinterpret_cast<void*>(&hook_surface_unlock_rect)) || ok;
    if (ok) g_texture_hook_installs.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool patch_dxgi_swapchain_vtable(IDXGISwapChain* swap_chain) {
    if (swap_chain == nullptr) return false;
    return patch_vtable_slot(swap_chain,
                             kSlotDxgiSwapChainPresent,
                             reinterpret_cast<void*>(&hook_dxgi_present));
}

bool patch_dxgi_swapchain1_vtable(IDXGISwapChain1* swap_chain) {
    if (swap_chain == nullptr) return false;
    return patch_vtable_slot(swap_chain,
                             kSlotDxgiSwapChain1Present1,
                             reinterpret_cast<void*>(&hook_dxgi_present1));
}

bool patch_d3d11_device_vtable(ID3D11Device* device) {
    if (device == nullptr) return false;
    const bool ok = patch_vtable_slot(device,
                                      kSlotD3D11DeviceCreateTexture2D,
                                      reinterpret_cast<void*>(&hook_d3d11_create_texture_2d));
    if (ok) g_device_hook_installs.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool patch_d3d11_context_vtable(ID3D11DeviceContext* context) {
    if (context == nullptr) return false;
    bool ok = false;
    ok = patch_vtable_slot(context,
                           kSlotD3D11ContextDrawIndexed,
                           reinterpret_cast<void*>(&hook_d3d11_draw_indexed)) || ok;
    ok = patch_vtable_slot(context,
                           kSlotD3D11ContextDraw,
                           reinterpret_cast<void*>(&hook_d3d11_draw)) || ok;
    ok = patch_vtable_slot(context,
                           kSlotD3D11ContextDrawIndexedInstanced,
                           reinterpret_cast<void*>(&hook_d3d11_draw_indexed_instanced)) || ok;
    ok = patch_vtable_slot(context,
                           kSlotD3D11ContextDrawInstanced,
                           reinterpret_cast<void*>(&hook_d3d11_draw_instanced)) || ok;
    ok = patch_vtable_slot(context,
                           kSlotD3D11ContextOMSetRenderTargets,
                           reinterpret_cast<void*>(&hook_d3d11_om_set_render_targets)) || ok;
    ok = patch_vtable_slot(context,
                           kSlotD3D11ContextDrawAuto,
                           reinterpret_cast<void*>(&hook_d3d11_draw_auto)) || ok;
    ok = patch_vtable_slot(context,
                           kSlotD3D11ContextUpdateSubresource,
                           reinterpret_cast<void*>(&hook_d3d11_update_subresource)) || ok;
    ok = patch_vtable_slot(context,
                           kSlotD3D11ContextClearRenderTargetView,
                           reinterpret_cast<void*>(&hook_d3d11_clear_render_target_view)) || ok;
    if (ok) g_device_hook_installs.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

LRESULT CALLBACK dummy_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

HWND create_dummy_window() {
    HINSTANCE instance = GetModuleHandleA(nullptr);
    const char* class_name = "AneProfilerD3D9DummyWindow";
    WNDCLASSA wc{};
    wc.lpfnWndProc = dummy_window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    RegisterClassA(&wc);
    return CreateWindowExA(0,
                           class_name,
                           "aneprof-d3d9",
                           WS_OVERLAPPED,
                           0,
                           0,
                           1,
                           1,
                           nullptr,
                           nullptr,
                           instance,
                           nullptr);
}

struct EnumWindowData {
    DWORD pid = 0;
    HWND hwnd = nullptr;
};

BOOL CALLBACK enum_process_window(HWND hwnd, LPARAM param) {
    auto* data = reinterpret_cast<EnumWindowData*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != data->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    data->hwnd = hwnd;
    return FALSE;
}

HWND find_process_window() {
    EnumWindowData data{};
    data.pid = GetCurrentProcessId();
    EnumWindows(enum_process_window, reinterpret_cast<LPARAM>(&data));
    return data.hwnd;
}

bool create_and_patch_dummy_device(IDirect3D9* d3d, HWND hwnd) {
    if (d3d == nullptr || hwnd == nullptr) return false;
    D3DDISPLAYMODE mode{};
    if (FAILED(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode))) {
        mode.Format = D3DFMT_X8R8G8B8;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = 1;
    pp.BackBufferHeight = 1;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    const D3DDEVTYPE device_types[] = {D3DDEVTYPE_HAL, D3DDEVTYPE_NULLREF, D3DDEVTYPE_REF};
    const DWORD behavior_flags[] = {
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED,
        D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED,
    };
    for (D3DDEVTYPE device_type : device_types) {
        for (DWORD flags : behavior_flags) {
            IDirect3DDevice9* device = nullptr;
            const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT,
                                                 device_type,
                                                 hwnd,
                                                 flags,
                                                 &pp,
                                                 &device);
            g_last_device_create_hr.store(static_cast<std::uint32_t>(hr), std::memory_order_relaxed);
            if (SUCCEEDED(hr) && device != nullptr) {
                const bool ok = patch_device_vtable(device);
                device->Release();
                if (ok) return true;
            }
        }
    }
    return false;
}

bool create_and_patch_dummy_device_ex(IDirect3D9Ex* d3d, HWND hwnd) {
    if (d3d == nullptr || hwnd == nullptr) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 1;
    pp.BackBufferHeight = 1;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    const D3DDEVTYPE device_types[] = {D3DDEVTYPE_HAL, D3DDEVTYPE_NULLREF, D3DDEVTYPE_REF};
    const DWORD behavior_flags[] = {
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED,
        D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED,
    };
    for (D3DDEVTYPE device_type : device_types) {
        for (DWORD flags : behavior_flags) {
            IDirect3DDevice9Ex* device = nullptr;
            const HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT,
                                                   device_type,
                                                   hwnd,
                                                   flags,
                                                   &pp,
                                                   nullptr,
                                                   &device);
            g_last_device_create_hr.store(static_cast<std::uint32_t>(hr), std::memory_order_relaxed);
            if (SUCCEEDED(hr) && device != nullptr) {
                const bool ok = patch_device_ex_vtable(device);
                device->Release();
                if (ok) return true;
            }
        }
    }
    return false;
}

bool create_and_patch_d3d11_dummy(HWND hwnd) {
    if (hwnd == nullptr) return false;
    if (g_d3d11_module == nullptr) {
        g_d3d11_module = LoadLibraryA("d3d11.dll");
    }
    if (g_d3d11_module == nullptr) return false;

    auto* create_device = reinterpret_cast<D3D11CreateDeviceAndSwapChainFn>(
        GetProcAddress(g_d3d11_module, "D3D11CreateDeviceAndSwapChain"));
    if (create_device == nullptr) return false;

    DXGI_SWAP_CHAIN_DESC swap_desc{};
    swap_desc.BufferDesc.Width = 1;
    swap_desc.BufferDesc.Height = 1;
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 1;
    swap_desc.OutputWindow = hwnd;
    swap_desc.Windowed = TRUE;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_1,
    };
    const D3D_DRIVER_TYPE driver_types[] = {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };

    for (D3D_DRIVER_TYPE driver_type : driver_types) {
        IDXGISwapChain* swap_chain = nullptr;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_9_1;
        const HRESULT hr = create_device(nullptr,
                                         driver_type,
                                         nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         feature_levels,
                                         static_cast<UINT>(sizeof(feature_levels) / sizeof(feature_levels[0])),
                                         D3D11_SDK_VERSION,
                                         &swap_desc,
                                         &swap_chain,
                                         &device,
                                         &feature_level,
                                         &context);
        g_last_device_create_hr.store(static_cast<std::uint32_t>(hr), std::memory_order_relaxed);
        if (FAILED(hr)) continue;

        bool ok = false;
        ok = patch_dxgi_swapchain_vtable(swap_chain) || ok;
        IDXGISwapChain1* swap_chain1 = nullptr;
        if (swap_chain != nullptr &&
            SUCCEEDED(swap_chain->QueryInterface(__uuidof(IDXGISwapChain1),
                                                 reinterpret_cast<void**>(&swap_chain1))) &&
            swap_chain1 != nullptr) {
            ok = patch_dxgi_swapchain1_vtable(swap_chain1) || ok;
            swap_chain1->Release();
        }

        if (context != nullptr) context->Release();
        if (device != nullptr) device->Release();
        if (swap_chain != nullptr) swap_chain->Release();
        if (ok) return true;
    }
    return false;
}

void reset_frame_accumulators() {
    g_frame_index.store(0, std::memory_order_relaxed);
    g_last_present_end_ns.store(0, std::memory_order_relaxed);
    g_acc_draw_calls.store(0, std::memory_order_relaxed);
    g_acc_primitives.store(0, std::memory_order_relaxed);
    g_acc_texture_upload_bytes.store(0, std::memory_order_relaxed);
    g_acc_texture_create_bytes.store(0, std::memory_order_relaxed);
    g_acc_texture_create_count.store(0, std::memory_order_relaxed);
    g_acc_texture_update_count.store(0, std::memory_order_relaxed);
    g_acc_set_texture_count.store(0, std::memory_order_relaxed);
    g_acc_render_target_change_count.store(0, std::memory_order_relaxed);
    g_acc_clear_count.store(0, std::memory_order_relaxed);
}

} // namespace

WindowsRenderHook::~WindowsRenderHook() {
    uninstall();
}

bool WindowsRenderHook::install(DeepProfilerController* controller) {
    if (controller == nullptr) return false;
    g_controller.store(controller, std::memory_order_release);
    if (installed_) {
        reset_frame_accumulators();
        g_render_capture_active.store(true, std::memory_order_release);
        return true;
    }

    g_last_failure_stage.store(0, std::memory_order_relaxed);
    reset_frame_accumulators();

    HWND process_window = find_process_window();
    g_dummy_window = create_dummy_window();
    HWND primary_window = process_window != nullptr ? process_window : g_dummy_window;
    bool installed_any = false;

    if (g_d3d9_module == nullptr) {
        g_d3d9_module = LoadLibraryA("d3d9.dll");
    }
    if (g_d3d9_module == nullptr) {
        g_last_failure_stage.store(1, std::memory_order_relaxed);
    } else {
        auto* create9 = reinterpret_cast<Direct3DCreate9Fn>(
            GetProcAddress(g_d3d9_module, "Direct3DCreate9"));
        if (create9 == nullptr) {
            g_last_failure_stage.store(2, std::memory_order_relaxed);
        } else {
            IDirect3D9* d3d = create9(D3D_SDK_VERSION);
            if (d3d != nullptr) {
                installed_any = patch_d3d9_vtable(d3d) || installed_any;
                installed_any = create_and_patch_dummy_device(d3d, primary_window) || installed_any;
                if (primary_window != g_dummy_window) {
                    installed_any = create_and_patch_dummy_device(d3d, g_dummy_window) || installed_any;
                }
                d3d->Release();
            }
        }

        auto* create9ex = reinterpret_cast<Direct3DCreate9ExFn>(
            GetProcAddress(g_d3d9_module, "Direct3DCreate9Ex"));
        if (create9ex != nullptr) {
            IDirect3D9Ex* d3d_ex = nullptr;
            if (SUCCEEDED(create9ex(D3D_SDK_VERSION, &d3d_ex)) && d3d_ex != nullptr) {
                installed_any = patch_d3d9ex_vtable(d3d_ex) || installed_any;
                installed_any = create_and_patch_dummy_device_ex(d3d_ex, primary_window) || installed_any;
                if (primary_window != g_dummy_window) {
                    installed_any = create_and_patch_dummy_device_ex(d3d_ex, g_dummy_window) || installed_any;
                }
                d3d_ex->Release();
            }
        }
    }

    installed_any = create_and_patch_d3d11_dummy(primary_window) || installed_any;
    if (primary_window != g_dummy_window) {
        installed_any = create_and_patch_d3d11_dummy(g_dummy_window) || installed_any;
    }

    if (!installed_any) {
        g_last_failure_stage.store(3, std::memory_order_relaxed);
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        uninstall();
        return false;
    }

    installed_ = true;
    g_render_capture_active.store(true, std::memory_order_release);
    g_last_failure_stage.store(0, std::memory_order_relaxed);
    return true;
}

void WindowsRenderHook::pause() {
    g_render_capture_active.store(false, std::memory_order_release);
    g_controller.store(nullptr, std::memory_order_release);
    reset_frame_accumulators();
    {
        std::lock_guard<std::mutex> lock(g_texture_mu);
        g_textures.clear();
        g_texture_locks.clear();
        g_surfaces.clear();
        g_surface_locks.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_runtime_device_mu);
        g_runtime_devices.clear();
    }
}

void WindowsRenderHook::uninstall() {
    g_render_capture_active.store(false, std::memory_order_release);
    g_controller.store(nullptr, std::memory_order_release);
    restore_vtable_patches();
    {
        std::lock_guard<std::mutex> lock(g_texture_mu);
        g_textures.clear();
        g_texture_locks.clear();
        g_surfaces.clear();
        g_surface_locks.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_runtime_device_mu);
        g_runtime_devices.clear();
    }
    if (g_dummy_window != nullptr) {
        DestroyWindow(g_dummy_window);
        g_dummy_window = nullptr;
    }
    // Do not FreeLibrary D3D modules here. AIR can continue rendering after a
    // capture stops, and some renderer paths keep COM vtables/function pointers
    // whose module lifetime is not obvious from the ANE side. Restoring vtable
    // slots removes hook overhead; keeping the modules loaded avoids invalidating
    // a renderer that is still alive.
    installed_ = false;
}

std::uint64_t WindowsRenderHook::hookInstalls() const {
    return g_hook_installs.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::deviceHookInstalls() const {
    return g_device_hook_installs.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::textureHookInstalls() const {
    return g_texture_hook_installs.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::failedInstalls() const {
    return g_failed_installs.load(std::memory_order_relaxed);
}

std::uint32_t WindowsRenderHook::lastFailureStage() const {
    return g_last_failure_stage.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::patchedSlots() const {
    std::uint64_t count = 0;
    for (auto& record : g_vtable_patches) {
        if (record.slot.load(std::memory_order_relaxed) != nullptr) {
            ++count;
        }
    }
    return count;
}

std::uint64_t WindowsRenderHook::renderFrames() const {
    return g_render_frames_total.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::presentCalls() const {
    return g_present_calls.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::drawCalls() const {
    return g_draw_calls_total.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::primitiveCount() const {
    return g_primitives_total.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::textureCreates() const {
    return g_texture_creates_total.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::textureUpdates() const {
    return g_texture_updates_total.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::textureUploadBytes() const {
    return g_texture_upload_bytes_total.load(std::memory_order_relaxed);
}

std::uint64_t WindowsRenderHook::textureCreateBytes() const {
    return g_texture_create_bytes_total.load(std::memory_order_relaxed);
}

} // namespace ane::profiler
