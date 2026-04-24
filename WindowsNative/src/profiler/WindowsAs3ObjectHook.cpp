#include "profiler/WindowsAs3ObjectHook.hpp"

#include "AirTelemetryRvas.h"
#include "DeepProfilerController.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ane::profiler {

namespace {
std::atomic<DeepProfilerController*> g_controller{nullptr};
std::atomic<std::uint64_t> g_as3_alloc_calls{0};
std::atomic<std::uint64_t> g_as3_free_calls{0};
std::atomic<std::uint64_t> g_generic_alloc_calls{0};
std::atomic<std::uint64_t> g_failed_installs{0};
std::atomic<std::uint32_t> g_last_failure_stage{0};
std::atomic<std::uintptr_t> g_last_core{0};
thread_local bool g_inside_hook = false;

struct LiveAs3Object {
    std::string type_name;
    std::uint64_t size = 0;
};

std::mutex g_live_mu;
std::unordered_map<std::uintptr_t, LiveAs3Object> g_live_as3;
std::mutex g_sampler_alloc_mu;
std::map<std::uintptr_t, std::uint64_t> g_sampler_alloc_sizes;

struct As3ReferenceKey {
    std::uintptr_t owner = 0;
    std::uintptr_t dependent = 0;

    bool operator==(const As3ReferenceKey& other) const noexcept {
        return owner == other.owner && dependent == other.dependent;
    }
};

struct As3ReferenceKeyHash {
    std::size_t operator()(const As3ReferenceKey& key) const noexcept {
        const auto a = static_cast<std::uint64_t>(key.owner);
        const auto b = static_cast<std::uint64_t>(key.dependent);
        return static_cast<std::size_t>(a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2)));
    }
};

std::mutex g_as3_ref_mu;
std::unordered_set<As3ReferenceKey, As3ReferenceKeyHash> g_as3_refs;
std::unordered_map<std::uintptr_t, std::unordered_set<std::uintptr_t>> g_as3_refs_by_owner;
std::unordered_map<std::uintptr_t, std::unordered_set<std::uintptr_t>> g_as3_refs_by_dependent;

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)

#if defined(_M_X64) || defined(__x86_64__)
using AttachSamplerFn = void (*)(void*);
using GetSamplerFn = void* (*)();
using MethodNameWithTraitsFn = std::uintptr_t (*)(void*, void*, bool);

inline constexpr std::uint32_t kVTableOffTraits = 0x28;
inline constexpr std::uint32_t kTraitsOffCore = 0x08;
inline constexpr std::uint32_t kTraitsOffNs = 0x88;
inline constexpr std::uint32_t kTraitsOffName = 0x90;
inline constexpr std::uint32_t kStringOffBuffer = 0x10;
inline constexpr std::uint32_t kStringOffExtra = 0x18;
inline constexpr std::uint32_t kStringOffLength = 0x20;
inline constexpr std::uint32_t kStringOffFlags = 0x24;
inline constexpr std::uint32_t kNamespaceOffUriAndType = 0x18;
inline constexpr std::uint32_t kCallStackOffFunctionId = 0x00;
inline constexpr std::uint32_t kCallStackOffInfo = 0x18;
inline constexpr std::uint32_t kCallStackOffNext = 0x20;
inline constexpr std::uint32_t kCallStackOffFakeName = 0x28;
inline constexpr std::uint32_t kCallStackOffFilename = 0x38;
inline constexpr std::uint32_t kCallStackOffLine = 0x50;
inline constexpr std::uint32_t kMethodFrameOffNext = 0x00;
inline constexpr std::uint32_t kMethodFrameOffEnvOrCodeContext = 0x08;
inline constexpr std::uint32_t kMethodEnvOffMethod = 0x10;
inline constexpr std::uint32_t kMethodInfoOffDeclarer = 0x20;
inline constexpr std::uint32_t kMethodInfoOffPool = 0x30;
inline constexpr std::uint32_t kMethodInfoOffMethodId = 0x40;
inline constexpr std::uint32_t kPoolObjectOffCore = 0x08;

constexpr std::uint8_t kAttachSamplerPrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x05
};
constexpr std::uint8_t kAttachSamplerMask[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
constexpr std::uint8_t kGetSamplerPrologue[] = {
    0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x05
};
constexpr std::uint8_t kGetSamplerMask[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
constexpr std::uint8_t kMethodNameWithTraitsPrologue[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x55
};
constexpr std::uint8_t kMethodNameWithTraitsMask[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

#else
using AttachSamplerFn = void (__cdecl*)(void*);
using GetSamplerFn = void* (__cdecl*)();
using MethodNameWithTraitsFn = std::uintptr_t (__fastcall*)(void*, void*, bool);

inline constexpr std::uint32_t kVTableOffTraits = 0x14;
inline constexpr std::uint32_t kTraitsOffCore = 0x04;
inline constexpr std::uint32_t kTraitsOffNs = 0x44;
inline constexpr std::uint32_t kTraitsOffName = 0x48;
inline constexpr std::uint32_t kStringOffBuffer = 0x08;
inline constexpr std::uint32_t kStringOffExtra = 0x0c;
inline constexpr std::uint32_t kStringOffLength = 0x10;
inline constexpr std::uint32_t kStringOffFlags = 0x14;
inline constexpr std::uint32_t kNamespaceOffUriAndType = 0x0c;
inline constexpr std::uint32_t kCallStackOffFunctionId = 0x00;
inline constexpr std::uint32_t kCallStackOffInfo = 0x10;
inline constexpr std::uint32_t kCallStackOffNext = 0x14;
inline constexpr std::uint32_t kCallStackOffFakeName = 0x18;
inline constexpr std::uint32_t kCallStackOffFilename = 0x20;
inline constexpr std::uint32_t kCallStackOffLine = 0x2c;
inline constexpr std::uint32_t kMethodFrameOffNext = 0x00;
inline constexpr std::uint32_t kMethodFrameOffEnvOrCodeContext = 0x04;
inline constexpr std::uint32_t kMethodEnvOffMethod = 0x08;
inline constexpr std::uint32_t kMethodInfoOffDeclarer = 0x10;
inline constexpr std::uint32_t kMethodInfoOffPool = 0x18;
inline constexpr std::uint32_t kMethodInfoOffMethodId = 0x20;
inline constexpr std::uint32_t kPoolObjectOffCore = 0x04;

constexpr std::uint8_t kAttachSamplerPrologue[] = {
    0xa1, 0x00, 0x00, 0x00, 0x00, 0x85, 0xc0, 0x74, 0x2e
};
constexpr std::uint8_t kAttachSamplerMask[] = {
    0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
};
constexpr std::uint8_t kGetSamplerPrologue[] = {
    0xa1, 0x00, 0x00, 0x00, 0x00, 0x85, 0xc0, 0x74, 0x1e
};
constexpr std::uint8_t kGetSamplerMask[] = {
    0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
};
constexpr std::uint8_t kMethodNameWithTraitsPrologue[] = {
    0x83, 0xec, 0x2c, 0x8b, 0xc1, 0x53, 0x55, 0x56,
    0x8b, 0x48, 0x18, 0x8b, 0xea, 0x8b, 0x58, 0x20
};
constexpr std::uint8_t kMethodNameWithTraitsMask[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
#endif

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

template <typename T>
bool safe_read(const void* address, T& out) {
    if (address == nullptr) return false;
    __try {
        out = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr(const void* base, std::uint32_t offset, std::uintptr_t& out) {
    if (base == nullptr) return false;
    return safe_read(reinterpret_cast<const std::uint8_t*>(base) + offset, out);
}

void append_utf8_codepoint(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else {
        out.push_back(static_cast<char>(0xf0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    }
}

bool avm_string_to_utf8(std::uintptr_t string_ptr, std::string& out, std::size_t max_chars = 512) {
    out.clear();
    if (string_ptr == 0) return false;

    std::uintptr_t buffer = 0;
    std::uintptr_t extra = 0;
    std::int32_t length = 0;
    std::uint32_t flags = 0;
    const auto* str = reinterpret_cast<const void*>(string_ptr);
    if (!read_ptr(str, kStringOffBuffer, buffer) ||
        !read_ptr(str, kStringOffExtra, extra) ||
        !safe_read(reinterpret_cast<const std::uint8_t*>(str) + kStringOffLength, length) ||
        !safe_read(reinterpret_cast<const std::uint8_t*>(str) + kStringOffFlags, flags)) {
        return false;
    }
    if (length < 0 || length > 65536) return false;

    const std::uint32_t type = (flags & 0x6u) >> 1;
    if (type == 2) {
        std::uintptr_t master_buffer = 0;
        if (extra == 0 || !read_ptr(reinterpret_cast<const void*>(extra), kStringOffBuffer, master_buffer)) {
            return false;
        }
        buffer = master_buffer + buffer;
    }
    if (buffer == 0 && length != 0) return false;

    const bool wide = (flags & 0x1u) != 0;
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(length), max_chars);
    out.reserve(std::min<std::size_t>(count * (wide ? 3 : 2), max_chars * 3));
    for (std::size_t i = 0; i < count; ++i) {
        if (wide) {
            std::uint16_t ch = 0;
            if (!safe_read(reinterpret_cast<const std::uint16_t*>(buffer) + i, ch)) return !out.empty();
            if (ch >= 0xd800 && ch <= 0xdbff && i + 1 < count) {
                std::uint16_t lo = 0;
                if (safe_read(reinterpret_cast<const std::uint16_t*>(buffer) + i + 1, lo) &&
                    lo >= 0xdc00 && lo <= 0xdfff) {
                    const auto cp = 0x10000u + (((ch - 0xd800u) << 10) | (lo - 0xdc00u));
                    append_utf8_codepoint(out, cp);
                    ++i;
                    continue;
                }
            }
            append_utf8_codepoint(out, ch);
        } else {
            std::uint8_t ch = 0;
            if (!safe_read(reinterpret_cast<const std::uint8_t*>(buffer) + i, ch)) return !out.empty();
            append_utf8_codepoint(out, ch);
        }
    }
    if (static_cast<std::size_t>(length) > count) out.append("...");
    return true;
}

std::string hex_ptr(std::uintptr_t value) {
    char buf[2 + sizeof(std::uintptr_t) * 2 + 1] = {};
#if defined(_M_X64) || defined(__x86_64__)
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(value));
#else
    std::snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(value));
#endif
    return std::string(buf);
}

std::string namespace_uri(std::uintptr_t ns_ptr) {
    if (ns_ptr == 0) return {};
    std::uintptr_t uri_and_type = 0;
    if (!read_ptr(reinterpret_cast<const void*>(ns_ptr), kNamespaceOffUriAndType, uri_and_type)) return {};
    std::string uri;
    avm_string_to_utf8(uri_and_type & ~static_cast<std::uintptr_t>(7), uri, 256);
    return uri;
}

std::string traits_name(std::uintptr_t traits) {
    if (traits == 0 || (traits & 7u) != 0) return {};
    std::uintptr_t name_ptr = 0;
    std::uintptr_t ns_ptr = 0;
    std::string name;
    std::string uri;
    if (read_ptr(reinterpret_cast<const void*>(traits), kTraitsOffName, name_ptr)) {
        avm_string_to_utf8(name_ptr, name, 256);
    }
    if (read_ptr(reinterpret_cast<const void*>(traits), kTraitsOffNs, ns_ptr)) {
        uri = namespace_uri(ns_ptr);
    }
    if (name.empty()) return {};
    if (!uri.empty()) return uri + "::" + name;
    return name;
}

std::uintptr_t core_from_sot(std::uintptr_t sot) {
    if ((sot & 7u) != 0) return 0;
    const std::uintptr_t vtable = sot & ~static_cast<std::uintptr_t>(7);
    if (vtable == 0) return 0;
    std::uintptr_t traits = 0;
    std::uintptr_t core = 0;
    if (!read_ptr(reinterpret_cast<const void*>(vtable), kVTableOffTraits, traits) || traits == 0) {
        return 0;
    }
    if (!read_ptr(reinterpret_cast<const void*>(traits), kTraitsOffCore, core)) return 0;
    return core;
}

std::string type_from_sot(void* obj, std::uintptr_t sot, const char* generic_type) {
    if (generic_type != nullptr && generic_type[0] != '\0') {
        return std::string(generic_type);
    }

    const auto kind = static_cast<unsigned>(sot & 7u);
    if (kind == 1) return "String";
    if (kind == 2) return "Namespace";
    if (kind == 3) return "<native>";

    const std::uintptr_t vtable = sot & ~static_cast<std::uintptr_t>(7);
    std::uintptr_t traits = 0;
    if (vtable != 0 && read_ptr(reinterpret_cast<const void*>(vtable), kVTableOffTraits, traits) && traits != 0) {
        std::string name = traits_name(traits);
        if (!name.empty()) return name;
    }

    return std::string("Object@") + hex_ptr(reinterpret_cast<std::uintptr_t>(obj));
}

std::string call_stack_nodes_from_core(std::uintptr_t core) {
    if (core == 0) return {};
    std::uintptr_t node = 0;
    if (!read_ptr(reinterpret_cast<const void*>(core), air_rvas::kAvmCoreOffsetCallStack, node) || node == 0) {
        return {};
    }

    std::string out;
    for (std::uint32_t depth = 0; depth < 32 && node != 0; ++depth) {
        std::uint64_t function_id = 0;
        std::uintptr_t info = 0;
        std::uintptr_t fake_name_ptr = 0;
        std::uintptr_t filename_ptr = 0;
        std::uintptr_t next = 0;
        std::int32_t line = 0;
        safe_read(reinterpret_cast<const std::uint8_t*>(node) + kCallStackOffFunctionId, function_id);
        read_ptr(reinterpret_cast<const void*>(node), kCallStackOffInfo, info);
        read_ptr(reinterpret_cast<const void*>(node), kCallStackOffFakeName, fake_name_ptr);
        read_ptr(reinterpret_cast<const void*>(node), kCallStackOffFilename, filename_ptr);
        safe_read(reinterpret_cast<const std::uint8_t*>(node) + kCallStackOffLine, line);
        read_ptr(reinterpret_cast<const void*>(node), kCallStackOffNext, next);

        std::string name;
        std::string filename;
        avm_string_to_utf8(fake_name_ptr, name, 256);
        avm_string_to_utf8(filename_ptr, filename, 256);
        if (name.empty()) {
            if (function_id != 0) {
                name = "external:" + hex_ptr(static_cast<std::uintptr_t>(function_id));
            } else if (info != 0) {
                name = "method@" + hex_ptr(info);
            } else {
                name = "<as3>";
            }
        }

        if (!out.empty()) out.push_back('\n');
        out.push_back('#');
        out += std::to_string(depth);
        out.push_back(' ');
        out += name;
        if (!filename.empty()) {
            out += " (";
            out += filename;
            if (line > 0) {
                out.push_back(':');
                out += std::to_string(line);
            }
            out.push_back(')');
        } else if (line > 0) {
            out += " (:";
            out += std::to_string(line);
            out.push_back(')');
        }
        node = next;
    }
    return out;
}

struct MethodFrameInfo {
    std::uintptr_t method_info = 0;
    std::uintptr_t declaring_traits = 0;
    std::int32_t method_id = -1;
    std::string declaring_type;
    std::string method_name;
};

bool is_aligned_ptr(std::uintptr_t value) {
    return value > 0x10000 && (value & (sizeof(void*) - 1)) == 0;
}

bool looks_like_runtime_method_name(const std::string& name) {
    if (name.empty()) return false;
    if (name.rfind("MethodInfo-", 0) == 0) return false;
    if (name.find('\0') != std::string::npos) return false;
    return true;
}

MethodNameWithTraitsFn g_method_name_with_traits = nullptr;

std::uintptr_t call_method_name_with_traits(MethodNameWithTraitsFn fn,
                                            std::uintptr_t method,
                                            std::uintptr_t declaring_traits) {
    if (fn == nullptr) return 0;
    __try {
        return fn(reinterpret_cast<void*>(method),
                  reinterpret_cast<void*>(declaring_traits),
                  false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::string runtime_method_name(std::uintptr_t method, std::uintptr_t declaring_traits) {
    auto* fn = g_method_name_with_traits;
    if (fn == nullptr || !is_aligned_ptr(method)) return {};

    const std::uintptr_t name_ptr = call_method_name_with_traits(fn, method, declaring_traits);
    std::string name;
    if (name_ptr != 0 && avm_string_to_utf8(name_ptr, name, 512) &&
        looks_like_runtime_method_name(name)) {
        return name;
    }
    return {};
}

bool method_info_from_env_with_layout(std::uintptr_t env,
                                      std::uintptr_t core,
                                      std::uint32_t env_method_off,
                                      std::uint32_t mi_declarer_off,
                                      std::uint32_t mi_pool_off,
                                      std::uint32_t mi_method_id_off,
                                      std::uint32_t pool_core_off,
                                      MethodFrameInfo& out) {
    std::uintptr_t method = 0;
    if (!read_ptr(reinterpret_cast<const void*>(env), env_method_off, method) ||
        !is_aligned_ptr(method)) {
        return false;
    }

    std::int32_t method_id = -1;
    if (!safe_read(reinterpret_cast<const std::uint8_t*>(method) + mi_method_id_off, method_id)) {
        return false;
    }
    if (method_id < -1 || method_id > 10000000) return false;

    std::uintptr_t pool = 0;
    std::uintptr_t pool_core = 0;
    const bool pool_matches_core =
        read_ptr(reinterpret_cast<const void*>(method), mi_pool_off, pool) &&
        is_aligned_ptr(pool) &&
        read_ptr(reinterpret_cast<const void*>(pool), pool_core_off, pool_core) &&
        pool_core == core;

    std::uintptr_t declarer = 0;
    std::string declaring_type;
    if (read_ptr(reinterpret_cast<const void*>(method), mi_declarer_off, declarer) &&
        (declarer & 1u) == 0) {
        declaring_type = traits_name(declarer);
    }

    if (!pool_matches_core && declaring_type.empty()) return false;

    out.method_info = method;
    out.declaring_traits = ((declarer & 1u) == 0) ? declarer : 0;
    out.method_id = method_id;
    out.declaring_type = std::move(declaring_type);
    out.method_name = runtime_method_name(method, out.declaring_traits);
    return true;
}

bool method_info_from_env(std::uintptr_t env, std::uintptr_t core, MethodFrameInfo& out) {
    if (!is_aligned_ptr(env)) return false;
    if (method_info_from_env_with_layout(env,
                                         core,
                                         kMethodEnvOffMethod,
                                         kMethodInfoOffDeclarer,
                                         kMethodInfoOffPool,
                                         kMethodInfoOffMethodId,
                                         kPoolObjectOffCore,
                                         out)) {
        return true;
    }
    return false;
}

std::string method_frame_stack_from_core(std::uintptr_t core) {
    if (core == 0) return {};
    std::uintptr_t frame = 0;
    if (!read_ptr(reinterpret_cast<const void*>(core), air_rvas::kAvmCoreOffsetCurrentMethodFrame, frame) ||
        frame == 0) {
        return {};
    }

    std::string out;
    for (std::uint32_t depth = 0; depth < 32 && is_aligned_ptr(frame); ++depth) {
        std::uintptr_t next = 0;
        std::uintptr_t env_or_cc = 0;
        read_ptr(reinterpret_cast<const void*>(frame), kMethodFrameOffNext, next);
        if (!read_ptr(reinterpret_cast<const void*>(frame), kMethodFrameOffEnvOrCodeContext, env_or_cc)) {
            break;
        }

        std::string name;
        if ((env_or_cc & 1u) != 0) {
            name = "CodeContext@" + hex_ptr(env_or_cc & ~static_cast<std::uintptr_t>(3));
        } else {
            const std::uintptr_t env = env_or_cc & ~static_cast<std::uintptr_t>(3);
            MethodFrameInfo info;
            if (method_info_from_env(env, core, info)) {
                if (!info.method_name.empty()) {
                    name = info.method_name;
                } else if (!info.declaring_type.empty()) {
                    name = info.declaring_type;
                    name += "/";
                    name += "method#";
                    name += std::to_string(info.method_id);
                } else {
                    name = "method#";
                    name += std::to_string(info.method_id);
                }
                name += " ";
                name += hex_ptr(info.method_info);
            } else if (is_aligned_ptr(env)) {
                name = "MethodEnv@" + hex_ptr(env);
            } else {
                name = "<as3>";
            }
        }

        if (!out.empty()) out.push_back('\n');
        out.push_back('#');
        out += std::to_string(depth);
        out.push_back(' ');
        out += name;

        if (next == 0 || next == frame) break;
        frame = next;
    }
    return out;
}

std::string stack_from_core(std::uintptr_t core) {
    std::string stack = call_stack_nodes_from_core(core);
    if (!stack.empty()) return stack;
    return method_frame_stack_from_core(core);
}

void erase_as3_reference_locked(const As3ReferenceKey& edge) {
    if (g_as3_refs.erase(edge) == 0) return;

    auto owner_it = g_as3_refs_by_owner.find(edge.owner);
    if (owner_it != g_as3_refs_by_owner.end()) {
        owner_it->second.erase(edge.dependent);
        if (owner_it->second.empty()) {
            g_as3_refs_by_owner.erase(owner_it);
        }
    }

    auto dependent_it = g_as3_refs_by_dependent.find(edge.dependent);
    if (dependent_it != g_as3_refs_by_dependent.end()) {
        dependent_it->second.erase(edge.owner);
        if (dependent_it->second.empty()) {
            g_as3_refs_by_dependent.erase(dependent_it);
        }
    }
}

void erase_as3_references_for_object(std::uintptr_t key) {
    std::vector<As3ReferenceKey> to_erase;
    {
        std::lock_guard<std::mutex> lock(g_as3_ref_mu);
        auto owner_it = g_as3_refs_by_owner.find(key);
        if (owner_it != g_as3_refs_by_owner.end()) {
            to_erase.reserve(to_erase.size() + owner_it->second.size());
            for (const auto dependent : owner_it->second) {
                to_erase.push_back(As3ReferenceKey{key, dependent});
            }
        }

        auto dependent_it = g_as3_refs_by_dependent.find(key);
        if (dependent_it != g_as3_refs_by_dependent.end()) {
            to_erase.reserve(to_erase.size() + dependent_it->second.size());
            for (const auto owner : dependent_it->second) {
                to_erase.push_back(As3ReferenceKey{owner, key});
            }
        }

        for (const auto& edge : to_erase) {
            erase_as3_reference_locked(edge);
        }
    }
}

std::uint64_t sampler_allocation_size_for_object(std::uintptr_t obj) {
    if (obj == 0) return 0;
#if defined(_M_X64) || defined(__x86_64__)
    constexpr std::uintptr_t kDeltas[] = {0, 16, 8, 24, 32};
#else
    constexpr std::uintptr_t kDeltas[] = {0, 8, 4, 12, 16};
#endif

    std::lock_guard<std::mutex> lock(g_sampler_alloc_mu);
    for (const auto delta : kDeltas) {
        if (obj < delta) continue;
        const auto it = g_sampler_alloc_sizes.find(obj - delta);
        if (it != g_sampler_alloc_sizes.end()) return it->second;
    }

    auto upper = g_sampler_alloc_sizes.upper_bound(obj);
    if (upper == g_sampler_alloc_sizes.begin()) return 0;
    --upper;
    const auto base = upper->first;
    const auto size = upper->second;
    if (size != 0 && obj >= base && obj - base < size) return size;
    return 0;
}

void erase_sampler_allocation_size(std::uintptr_t ptr) {
    if (ptr == 0) return;
#if defined(_M_X64) || defined(__x86_64__)
    constexpr std::uintptr_t kDeltas[] = {0, 16, 8, 24, 32};
#else
    constexpr std::uintptr_t kDeltas[] = {0, 8, 4, 12, 16};
#endif
    std::lock_guard<std::mutex> lock(g_sampler_alloc_mu);
    for (const auto delta : kDeltas) {
        if (ptr >= delta) g_sampler_alloc_sizes.erase(ptr - delta);
        g_sampler_alloc_sizes.erase(ptr + delta);
    }
}

std::uint64_t tracked_as3_object_size(DeepProfilerController* ctrl, std::uintptr_t obj) {
    if (ctrl == nullptr || obj == 0) return 0;
    const auto sampler_size = sampler_allocation_size_for_object(obj);
    if (sampler_size != 0) return sampler_size;
#if defined(_M_X64) || defined(__x86_64__)
    constexpr std::uintptr_t kDeltas[] = {0, 16, 8, 24, 32};
#else
    constexpr std::uintptr_t kDeltas[] = {0, 8, 4, 12, 16};
#endif
    for (const auto delta : kDeltas) {
        if (obj < delta) continue;
        const auto size = ctrl->tracked_allocation_size(reinterpret_cast<void*>(obj - delta));
        if (size != 0) return size;
    }
    return 0;
}

void record_as3_alloc(void* obj,
                      std::uintptr_t sot,
                      const char* generic_type,
                      std::uint64_t explicit_size) {
    if (obj == nullptr || g_inside_hook) return;
    auto* ctrl = g_controller.load(std::memory_order_acquire);
    if (ctrl == nullptr) return;

    g_inside_hook = true;
    const auto key = reinterpret_cast<std::uintptr_t>(obj);
    std::uintptr_t core = core_from_sot(sot);
    if (core != 0) {
        g_last_core.store(core, std::memory_order_release);
    } else {
        core = g_last_core.load(std::memory_order_acquire);
    }

    std::string type_name = type_from_sot(obj, sot, generic_type);
    std::string stack = stack_from_core(core);
    std::uint64_t size = explicit_size != 0
        ? explicit_size
        : tracked_as3_object_size(ctrl, key);
    {
        std::lock_guard<std::mutex> lock(g_live_mu);
        g_live_as3[key] = LiveAs3Object{type_name, size};
    }
    ctrl->record_as3_alloc(key, type_name, size, stack);
    g_as3_alloc_calls.fetch_add(1, std::memory_order_relaxed);
    g_inside_hook = false;
}

void record_as3_free(const void* item, std::uint64_t explicit_size) {
    if (item == nullptr || g_inside_hook) return;
    auto* ctrl = g_controller.load(std::memory_order_acquire);
    if (ctrl == nullptr) return;

    const auto key = reinterpret_cast<std::uintptr_t>(item);
    LiveAs3Object meta;
    {
        std::lock_guard<std::mutex> lock(g_live_mu);
        auto it = g_live_as3.find(key);
        if (it == g_live_as3.end()) return;
        meta = std::move(it->second);
        g_live_as3.erase(it);
    }
    erase_as3_references_for_object(key);
    if (meta.size == 0) meta.size = explicit_size;

    g_inside_hook = true;
    ctrl->record_as3_free(key, meta.type_name, meta.size);
    g_as3_free_calls.fetch_add(1, std::memory_order_relaxed);
    g_inside_hook = false;
}

void record_as3_reference(const void* obj, const void* dep_obj) {
    if (obj == nullptr || dep_obj == nullptr || obj == dep_obj || g_inside_hook) return;
    auto* ctrl = g_controller.load(std::memory_order_acquire);
    if (ctrl == nullptr) return;

    const auto owner = reinterpret_cast<std::uintptr_t>(obj);
    const auto dependent = reinterpret_cast<std::uintptr_t>(dep_obj);
    {
        std::lock_guard<std::mutex> lock(g_as3_ref_mu);
        const auto inserted = g_as3_refs.insert(As3ReferenceKey{owner, dependent}).second;
        if (!inserted) return;
        g_as3_refs_by_owner[owner].insert(dependent);
        g_as3_refs_by_dependent[dependent].insert(owner);
    }

    g_inside_hook = true;
    ctrl->record_as3_reference(owner, dependent);
    g_inside_hook = false;
}

struct IMemorySamplerCompat {
    virtual int getSamplerType() = 0;
    virtual void recordAllocation(const void* item, std::size_t size) = 0;
    virtual void recordDeallocation(const void* item, std::size_t size) = 0;
    virtual void recordNewObjectAllocation(void* obj, std::uintptr_t sot) = 0;
    virtual void recordObjectReallocation(const void* obj) = 0;
    virtual void addDependentObject(const void* obj, const void* dep_obj) = 0;
    virtual void recordNewObjectAllocation(void* obj, const char* object_type, std::size_t size) = 0;
    virtual ~IMemorySamplerCompat() {}
};

class NativeMemorySampler final : public IMemorySamplerCompat {
public:
    int getSamplerType() override {
        return 0x414e45; // "ANE"
    }

    void recordAllocation(const void* item, std::size_t size) override {
        if (item == nullptr || size == 0) return;
        std::lock_guard<std::mutex> lock(g_sampler_alloc_mu);
        g_sampler_alloc_sizes[reinterpret_cast<std::uintptr_t>(item)] =
            static_cast<std::uint64_t>(size);
    }

    void recordDeallocation(const void* item, std::size_t size) override {
        record_as3_free(item, static_cast<std::uint64_t>(size));
        erase_sampler_allocation_size(reinterpret_cast<std::uintptr_t>(item));
    }

    void recordNewObjectAllocation(void* obj, std::uintptr_t sot) override {
        record_as3_alloc(obj, sot, nullptr, 0);
    }

    void recordObjectReallocation(const void*) override {
        // Back-buffer growth is already visible in the native allocation stream.
    }

    void addDependentObject(const void* obj, const void* dep_obj) override {
        record_as3_reference(obj, dep_obj);
    }

    void recordNewObjectAllocation(void* obj, const char* object_type, std::size_t size) override {
        g_generic_alloc_calls.fetch_add(1, std::memory_order_relaxed);
        record_as3_alloc(obj, 3, object_type, static_cast<std::uint64_t>(size));
    }
};

NativeMemorySampler g_sampler;
AttachSamplerFn g_attach_sampler = nullptr;
GetSamplerFn g_get_sampler = nullptr;

bool call_attach_sampler(void* sampler) {
    if (g_attach_sampler == nullptr) return false;
    __try {
        g_attach_sampler(sampler);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* call_get_sampler() {
    if (g_get_sampler == nullptr) return nullptr;
    __try {
        return g_get_sampler();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

#endif
} // namespace

WindowsAs3ObjectHook::~WindowsAs3ObjectHook() {
    uninstall();
}

bool WindowsAs3ObjectHook::install(DeepProfilerController* controller) {
    if (controller == nullptr) return false;
    if (installed_) {
        g_controller.store(controller, std::memory_order_release);
        return true;
    }

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    g_last_failure_stage.store(0, std::memory_order_relaxed);
    HMODULE air = GetModuleHandleA(air_rvas::kDllName);
    if (air == nullptr) {
        g_last_failure_stage.store(1, std::memory_order_relaxed);
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(air);
    auto* attach = reinterpret_cast<void*>(base + air_rvas::kRvaAttachSampler);
    auto* get = reinterpret_cast<void*>(base + air_rvas::kRvaGetSampler);
    auto* method_name_with_traits =
        reinterpret_cast<void*>(base + air_rvas::kRvaMethodInfoGetMethodNameWithTraits);
    if (!matches_signature(attach, kAttachSamplerPrologue, kAttachSamplerMask, sizeof(kAttachSamplerPrologue))) {
        g_last_failure_stage.store(2, std::memory_order_relaxed);
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!matches_signature(get, kGetSamplerPrologue, kGetSamplerMask, sizeof(kGetSamplerPrologue))) {
        g_last_failure_stage.store(3, std::memory_order_relaxed);
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    g_attach_sampler = reinterpret_cast<AttachSamplerFn>(attach);
    g_get_sampler = reinterpret_cast<GetSamplerFn>(get);
    if (matches_signature(method_name_with_traits,
                          kMethodNameWithTraitsPrologue,
                          kMethodNameWithTraitsMask,
                          sizeof(kMethodNameWithTraitsPrologue))) {
        g_method_name_with_traits =
            reinterpret_cast<MethodNameWithTraitsFn>(method_name_with_traits);
    } else {
        g_method_name_with_traits = nullptr;
    }
    void* current = call_get_sampler();
    if (current != nullptr && current != &g_sampler) {
        g_last_failure_stage.store(4, std::memory_order_relaxed);
        g_failed_installs.fetch_add(1, std::memory_order_relaxed);
        g_attach_sampler = nullptr;
        g_get_sampler = nullptr;
        g_method_name_with_traits = nullptr;
        return false;
    }

    g_controller.store(controller, std::memory_order_release);
    if (current != &g_sampler) {
        if (!call_attach_sampler(&g_sampler) || call_get_sampler() != &g_sampler) {
            g_controller.store(nullptr, std::memory_order_release);
            g_last_failure_stage.store(5, std::memory_order_relaxed);
            g_failed_installs.fetch_add(1, std::memory_order_relaxed);
            g_attach_sampler = nullptr;
            g_get_sampler = nullptr;
            g_method_name_with_traits = nullptr;
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_live_mu);
        g_live_as3.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_sampler_alloc_mu);
        g_sampler_alloc_sizes.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_as3_ref_mu);
        g_as3_refs.clear();
        g_as3_refs_by_owner.clear();
        g_as3_refs_by_dependent.clear();
    }
    installed_ = true;
    g_last_failure_stage.store(0, std::memory_order_relaxed);
    return true;
#else
    g_last_failure_stage.store(6, std::memory_order_relaxed);
    g_failed_installs.fetch_add(1, std::memory_order_relaxed);
    return false;
#endif
}

void WindowsAs3ObjectHook::uninstall() {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    if (installed_ && g_get_sampler != nullptr && g_attach_sampler != nullptr) {
        if (call_get_sampler() == &g_sampler) {
            call_attach_sampler(nullptr);
        }
    }
    g_attach_sampler = nullptr;
    g_get_sampler = nullptr;
    g_method_name_with_traits = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_live_mu);
        g_live_as3.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_sampler_alloc_mu);
        g_sampler_alloc_sizes.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_as3_ref_mu);
        g_as3_refs.clear();
        g_as3_refs_by_owner.clear();
        g_as3_refs_by_dependent.clear();
    }
#endif
    installed_ = false;
    g_controller.store(nullptr, std::memory_order_release);
}

std::uint64_t WindowsAs3ObjectHook::as3AllocCalls() const {
    return g_as3_alloc_calls.load(std::memory_order_relaxed);
}

std::uint64_t WindowsAs3ObjectHook::as3FreeCalls() const {
    return g_as3_free_calls.load(std::memory_order_relaxed);
}

std::uint64_t WindowsAs3ObjectHook::genericAllocCalls() const {
    return g_generic_alloc_calls.load(std::memory_order_relaxed);
}

std::uint64_t WindowsAs3ObjectHook::failedInstalls() const {
    return g_failed_installs.load(std::memory_order_relaxed);
}

std::uint32_t WindowsAs3ObjectHook::lastFailureStage() const {
    return g_last_failure_stage.load(std::memory_order_relaxed);
}

} // namespace ane::profiler
