#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <functional>
#include <thread>
#include <typeinfo>
#ifdef __linux__
#include <unistd.h>
#include <sys/syscall.h>
#endif

// Diagnostic only: estimates requested buffer sizes, not live allocation totals.
// Log before fetching samples so a failed allocation still has an evidence trail.
namespace MemoryTrace {
inline bool enabled()
{
    static const bool on = [] {
        const char *value = std::getenv("INSPECTRUM_MEMORY_LOG");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return on;
}

template<typename Source>
inline void request(const char *operation, const Source *source, size_t start,
                    size_t count, long double estimatedBytes)
{
    if (!enabled() || estimatedBytes < 1024*1024) return;
#ifdef __linux__
    const auto tid = static_cast<unsigned long>(syscall(SYS_gettid));
#else
    const auto tid = static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::fprintf(stderr,
        "[memory ms=%lld tid=%lu] %s source=%p type=%s start=%zu count=%zu estimated_bytes=%.0Lf\n",
        static_cast<long long>(ms), tid, operation, static_cast<const void*>(source),
        typeid(*source).name(), start, count, estimatedBytes);
    std::fflush(stderr);
}
}
