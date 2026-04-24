// Windows AIR/MMgc allocation hook for the .aneprof backend.
//
// This is deliberately narrow: AIR SDK 51.1.3.10 x86/x64 only, guarded by
// byte signatures at the target RVAs. It records alloc/allocLocked returns,
// MMgc frees, and AIR's Kernel32 HeapFree/HeapReAlloc calls into
// DeepProfilerController.

#ifndef ANE_PROFILER_WINDOWS_DEEP_MEMORY_HOOK_HPP
#define ANE_PROFILER_WINDOWS_DEEP_MEMORY_HOOK_HPP

#include <cstdint>

namespace ane::profiler {

class DeepProfilerController;

class WindowsDeepMemoryHook {
public:
    WindowsDeepMemoryHook() = default;
    ~WindowsDeepMemoryHook();

    bool install(DeepProfilerController* controller);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    std::uint64_t allocCalls() const;
    std::uint64_t allocLockedCalls() const;
    std::uint64_t heapAllocCalls() const;
    std::uint64_t freeCalls() const;
    std::uint64_t heapFreeCalls() const;
    std::uint64_t heapReallocCalls() const;
    std::uint64_t failedInstalls() const;
    std::uint32_t lastFailureStage() const;
    bool freeHooksInstalled() const noexcept { return free_hooks_installed_; }
    bool reallocHooksInstalled() const noexcept { return realloc_hooks_installed_; }

private:
    bool installed_ = false;
    bool free_hooks_installed_ = false;
    bool realloc_hooks_installed_ = false;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_WINDOWS_DEEP_MEMORY_HOOK_HPP
