// IDiskMonitor — platform-abstract query for "how much free space is
// available on the volume holding `path`". Used by the writer thread to
// bail out before a capture fills the disk.

#ifndef ANE_PROFILER_I_DISK_MONITOR_HPP
#define ANE_PROFILER_I_DISK_MONITOR_HPP

#include <cstdint>
#include <memory>
#include <string>

namespace ane::profiler {

class IDiskMonitor {
public:
    virtual ~IDiskMonitor() = default;

    // Return the number of bytes free on the volume that contains `path`.
    // On query failure return UINT64_MAX so callers treat it as "unknown,
    // don't block the capture".
    virtual std::uint64_t free_bytes(const std::string& path) = 0;

    // Factory — impl provided by platform file.
    static std::unique_ptr<IDiskMonitor> create();
};

} // namespace ane::profiler

#endif // ANE_PROFILER_I_DISK_MONITOR_HPP
