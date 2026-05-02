// Native-side helpers for memory probe FRE functions.
//
// Two responsibilities:
//
// 1. Stream-parse /proc/self/maps line-by-line in C++ (instead of Java
//    Files.readAllLines / BufferedReader → List<String>) so a process with
//    60–100k VMA entries doesn't materialize a 5–10 MB List<String> on the
//    Java GC heap every tick. We allocate one ~4 KB stack buffer for the
//    line and emit only an aggregate count vector.
//
// 2. Thin wrapper around bionic mallopt() — the Java side has no API for it
//    and we want to call M_PURGE_ALL (-104) on demand to force scudo to
//    munmap idle :secondary regions, recovering VMA budget without changing
//    AS3 code.
//
// Both methods are exported through System.loadLibrary("emulatordetector")
// alongside the existing emulator-detection JNI; the lib is already loaded
// at process start by RuntimeStatsCollector's static initializer.

#include <jni.h>
#include <malloc.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace {

// Map category index → counter slot in the output jlongArray.
// Keep index order stable; Java side reads by index.
enum {
    SLOT_TOTAL          = 0,  // total mapping count (every line in maps)
    SLOT_SCUDO_SECONDARY,     // [anon:scudo:secondary*]    one mmap per >=64KB alloc
    SLOT_SCUDO_PRIMARY,       // [anon:scudo:primary*]      pool slabs (excludes _reserve)
    SLOT_STACKS,              // [anon:stack_and_tls:*]     thread stacks
    SLOT_DALVIK,              // [anon:dalvik-*]            ART heap / linear alloc
    SLOT_OTHER,               // anything else
    SLOT_TOTAL_KB,            // sum of (end-start)/1024 across all lines
    SLOT_SCUDO_SECONDARY_KB,  // sum of secondary VMA sizes only — leak signal
    SLOT_COUNT                // == 8
};

// Best-effort: parse the address range "start-end" prefix, return size in KB.
// Returns 0 on parse failure (don't fail the whole tick).
static inline uint64_t parseLineSizeKb(const char* line) {
    const char* dash = std::strchr(line, '-');
    if (!dash) return 0;
    uint64_t start = 0, end = 0;
    // hex parse without scanf overhead
    for (const char* p = line; p < dash; ++p) {
        char c = *p;
        uint8_t d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        start = (start << 4) | d;
    }
    const char* p = dash + 1;
    while (*p && *p != ' ') {
        char c = *p++;
        uint8_t d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        end = (end << 4) | d;
    }
    if (end <= start) return 0;
    return (end - start) / 1024ULL;
}

// Returns SLOT_* enum value for the given /proc/self/maps line.
static inline int classifyLine(const char* line) {
    // The "name" column in /proc/self/maps is at the end of each line, after
    // the perms/offset/dev/inode columns. Substring search over the whole
    // line is correct because the prefixes we look for don't appear in any
    // earlier column (they're unique to anon-region names).
    if (std::strstr(line, "[anon:scudo:secondary"))                return SLOT_SCUDO_SECONDARY;
    // Match :primary but exclude :primary_reserve — _reserve is uncommitted
    // address space reservation, not a per-allocation mapping.
    if (const char* p = std::strstr(line, "[anon:scudo:primary")) {
        if (std::strncmp(p + 19, "_reserve", 8) != 0)              return SLOT_SCUDO_PRIMARY;
    }
    if (std::strstr(line, "[anon:stack_and_tls"))                  return SLOT_STACKS;
    if (std::strstr(line, "[anon:dalvik-"))                        return SLOT_DALVIK;
    return SLOT_OTHER;
}

} // namespace

extern "C" {

// jlong[8] returned: see SLOT_* enum above for index meaning.
// Returns null on a hard /proc read failure (caller treats as missing data).
JNIEXPORT jlongArray JNICALL
Java_br_com_redesurftank_aneawesomeutils_ProbeNative_nativeProbeMaps(
        JNIEnv* env, jclass /*clazz*/) {
    int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;

    // Buffered line reader over the raw fd. /proc/self/maps lines are usually
    // 80–250 bytes; 4 KB is generous and keeps the buffer in one TLB page.
    char    buf[4096];
    size_t  bufLen   = 0;
    char    line[2048];
    size_t  lineLen  = 0;

    int64_t counts[SLOT_COUNT];
    std::memset(counts, 0, sizeof(counts));

    for (;;) {
        ssize_t n = ::read(fd, buf + bufLen, sizeof(buf) - bufLen);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return nullptr;
        }
        if (n == 0 && bufLen == 0) break;
        size_t avail = bufLen + (size_t)n;
        size_t i = 0;
        while (i < avail) {
            char c = buf[i++];
            if (c == '\n') {
                line[lineLen] = '\0';
                if (lineLen > 0) {
                    int slot = classifyLine(line);
                    uint64_t sizeKb = parseLineSizeKb(line);
                    counts[SLOT_TOTAL]++;
                    counts[slot]++;
                    counts[SLOT_TOTAL_KB] += (int64_t)sizeKb;
                    if (slot == SLOT_SCUDO_SECONDARY) {
                        counts[SLOT_SCUDO_SECONDARY_KB] += (int64_t)sizeKb;
                    }
                }
                lineLen = 0;
            } else if (lineLen + 1 < sizeof(line)) {
                line[lineLen++] = c;
            }
            // overflow lines are silently truncated — preserves classification
            // (prefix stays intact, only path tail at end gets clipped, which
            // doesn't affect any of our substring checks).
        }
        // If we didn't see a newline, stash the partial line at start of buf
        // and continue reading. Cap at half the buffer to avoid pathological
        // single-line inputs. Empty fall-through resets bufLen for next read.
        if (n == 0) {
            // EOF — flush any pending line without trailing newline
            if (lineLen > 0) {
                line[lineLen] = '\0';
                int slot = classifyLine(line);
                uint64_t sizeKb = parseLineSizeKb(line);
                counts[SLOT_TOTAL]++;
                counts[slot]++;
                counts[SLOT_TOTAL_KB] += (int64_t)sizeKb;
                if (slot == SLOT_SCUDO_SECONDARY) {
                    counts[SLOT_SCUDO_SECONDARY_KB] += (int64_t)sizeKb;
                }
                lineLen = 0;
            }
            break;
        }
        bufLen = 0;  // we consumed everything (lines split mid-read are stashed in `line`)
    }

    ::close(fd);

    jlongArray out = env->NewLongArray(SLOT_COUNT);
    if (!out) return nullptr;
    env->SetLongArrayRegion(out, 0, SLOT_COUNT, counts);
    return out;
}

// Stream-parse /proc/self/maps and return a JSON string with per-path
// aggregates: {"ts":N,"totalCount":N,"totalSizeKb":N,"byPath":{"/path":{"count":N,"sizeKb":N},...}}
//
// Path is the trailing "name" column of each maps line — the actual file path
// for backed mappings, or `[anon:cookie]`/`[stack]`/`[heap]` for anonymous
// ones, or empty (we emit it as `_anon_unnamed`). Identical paths are summed.
//
// This is the diff signal for cycle leak detection: snapshot before and after
// a workload (e.g., a PVP battle), JSON-diff per path → exact lib/cookie that
// grew. Cheaper and more targeted than running a host-side `cat /proc/<pid>/maps`
// poller because it stays inside the AS3 thread tick (no adb round trip).
JNIEXPORT jstring JNICALL
Java_br_com_redesurftank_aneawesomeutils_ProbeNative_nativeProbeMapsByPath(
        JNIEnv* env, jclass /*clazz*/) {
    int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;

    struct Bucket {
        int64_t count = 0;
        int64_t sizeKb = 0;
    };
    std::unordered_map<std::string, Bucket> byPath;
    byPath.reserve(256);
    int64_t totalCount = 0;
    int64_t totalSizeKb = 0;

    char    buf[4096];
    size_t  bufLen   = 0;
    char    line[2048];
    size_t  lineLen  = 0;

    auto flushLine = [&]() {
        if (lineLen == 0) return;
        line[lineLen] = '\0';
        uint64_t sizeKb = parseLineSizeKb(line);
        // Tokenize: addrRange perms offset dev inode <path...>
        // The "path" column starts after the 5th whitespace boundary; find it
        // by scanning past the first 5 token starts. Lines without a path
        // (anonymous unnamed) yield an empty path → "_anon_unnamed".
        const char* p = line;
        const char* end = line + lineLen;
        // Skip address range (until first space)
        while (p < end && *p != ' ') ++p;
        // Now at start of perms; skip 4 more tokens (perms, offset, dev, inode)
        for (int t = 0; t < 4 && p < end; ++t) {
            while (p < end && *p == ' ') ++p;
            while (p < end && *p != ' ') ++p;
        }
        while (p < end && *p == ' ') ++p;
        std::string path;
        if (p < end) {
            // Trim trailing whitespace just in case
            const char* q = end;
            while (q > p && (q[-1] == ' ' || q[-1] == '\t')) --q;
            path.assign(p, q - p);
        }
        if (path.empty()) path = "_anon_unnamed";

        Bucket& b = byPath[path];
        b.count++;
        b.sizeKb += (int64_t)sizeKb;
        totalCount++;
        totalSizeKb += (int64_t)sizeKb;
    };

    for (;;) {
        ssize_t n = ::read(fd, buf + bufLen, sizeof(buf) - bufLen);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return nullptr;
        }
        if (n == 0 && bufLen == 0) break;
        size_t avail = bufLen + (size_t)n;
        size_t i = 0;
        while (i < avail) {
            char c = buf[i++];
            if (c == '\n') {
                flushLine();
                lineLen = 0;
            } else if (lineLen + 1 < sizeof(line)) {
                line[lineLen++] = c;
            }
        }
        if (n == 0) {
            flushLine();
            break;
        }
        bufLen = 0;
    }
    ::close(fd);

    // Estimate output size: each entry ~80-150 bytes. Reserve generously.
    std::string out;
    out.reserve(byPath.size() * 120 + 128);
    out.append("{\"totalCount\":");
    char numbuf[32];
    snprintf(numbuf, sizeof(numbuf), "%lld", (long long)totalCount);
    out.append(numbuf);
    out.append(",\"totalSizeKb\":");
    snprintf(numbuf, sizeof(numbuf), "%lld", (long long)totalSizeKb);
    out.append(numbuf);
    out.append(",\"byPath\":{");
    bool first = true;
    for (const auto& kv : byPath) {
        if (!first) out.append(",");
        first = false;
        out.append("\"");
        // Escape backslashes and quotes in path
        for (char c : kv.first) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        out.append("\":{\"count\":");
        snprintf(numbuf, sizeof(numbuf), "%lld", (long long)kv.second.count);
        out.append(numbuf);
        out.append(",\"sizeKb\":");
        snprintf(numbuf, sizeof(numbuf), "%lld", (long long)kv.second.sizeKb);
        out.append(numbuf);
        out.append("}");
    }
    out.append("}}");

    return env->NewStringUTF(out.c_str());
}

// Thin wrapper around bionic mallopt(). Returns the rc directly.
//
// Useful values for `param`:
//   M_PURGE      = -101  (recommended cleanup, returns idle to kernel)
//   M_PURGE_ALL  = -104  (aggressive purge, including allocator caches)
//   M_DECAY_TIME = -100
//
// We resolve the symbol via dlsym(RTLD_DEFAULT) instead of linking directly:
// some NDK toolchain configs build with hidden mallopt visibility, which
// causes the static linker to refuse the reference even though bionic
// definitely exports it at runtime. Looking it up at first call is robust
// across NDK / minSdk combinations and returns -1 only if mallopt is truly
// absent on the target Android version (it was added in API 23).
typedef int (*mallopt_fn)(int, int);

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_ProbeNative_nativeMallopt(
        JNIEnv* /*env*/, jclass /*clazz*/, jint param, jint value) {
    static mallopt_fn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        fn = (mallopt_fn) dlsym(RTLD_DEFAULT, "mallopt");
        resolved = true;
    }
    if (!fn) return -1;
    return (jint) fn(param, value);
}

} // extern "C"
