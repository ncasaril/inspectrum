/*
 * Tiny latency-trace helper. Enabled via INSPECTRUM_LAT_LOG=1. Each call
 * stamps stderr with monotonic microseconds since the first call and the
 * (truncated) thread id, so a tuner drag produces an easily-readable trace
 * showing where the time goes between mouse event → worker dispatch →
 * worker completion → blit. Header-only so adding marks doesn't drag
 * compile-units; the enabled() check is a single load on the disabled
 * path, which the optimiser inlines into a no-op everywhere.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

class LatencyLog {
public:
    static bool enabled() {
        static const bool e = []() {
            const char *v = std::getenv("INSPECTRUM_LAT_LOG");
            return v && v[0] && std::strcmp(v, "0") != 0;
        }();
        return e;
    }

    // Lightweight tag-only mark — preferred for hot paths.
    static void mark(const char *tag) {
        if (!enabled()) return;
        emit_(tag);
    }

    // printf-style for sites that want to attach values (sample range, etc.).
    static void markf(const char *fmt, ...) __attribute__((format(printf, 1, 2))) {
        if (!enabled()) return;
        char buf[256];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        emit_(buf);
    }

private:
    static void emit_(const char *msg) {
        using namespace std::chrono;
        auto now = steady_clock::now();
        static const auto t0 = now;
        auto us = duration_cast<microseconds>(now - t0).count();
        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        // Single fprintf so concurrent threads don't interleave mid-line.
        std::fprintf(stderr, "[lat %8lld.%03lldms tid=%04x] %s\n",
                     (long long)(us / 1000), (long long)(us % 1000),
                     (unsigned)(tid & 0xffffu), msg);
    }
};
