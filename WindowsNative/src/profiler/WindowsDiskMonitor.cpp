// Windows impl of IDiskMonitor via GetDiskFreeSpaceExW.

#include "IDiskMonitor.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace ane::profiler {

namespace {

class WindowsDiskMonitor : public IDiskMonitor {
public:
    std::uint64_t free_bytes(const std::string& path) override {
        if (path.empty()) return UINT64_MAX;
        // Convert UTF-8 path to wide.
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return UINT64_MAX;
        std::wstring wpath(static_cast<std::size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

        // GetDiskFreeSpaceExW accepts a directory or any file on the
        // volume — we pass the volume root derived from the path's drive
        // letter when available, otherwise the full path.
        ULARGE_INTEGER avail{};
        ULARGE_INTEGER total{};
        ULARGE_INTEGER free{};
        if (GetDiskFreeSpaceExW(wpath.c_str(), &avail, &total, &free)) {
            return static_cast<std::uint64_t>(avail.QuadPart);
        }
        return UINT64_MAX;
    }
};

} // namespace

std::unique_ptr<IDiskMonitor> IDiskMonitor::create() {
    return std::unique_ptr<IDiskMonitor>(new WindowsDiskMonitor());
}

} // namespace ane::profiler
