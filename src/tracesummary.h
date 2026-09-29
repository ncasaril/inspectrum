#pragma once

#include "samplesource.h"
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>
#include <algorithm>
#include <type_traits>

// Only screen-sized data survives a read. No allocation depends on view length.
struct TraceBucket {
    float low = std::numeric_limits<float>::infinity();
    float high = -std::numeric_limits<float>::infinity();
    void add(float value) {
        if (std::isfinite(value)) { low = std::min(low, value); high = std::max(high, value); }
    }
};

struct TraceSummary {
    static constexpr size_t chunkSize = 65536;
    int width = 0;
    int channels = 0;
    bool dense = false;
    bool complete = false;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    std::array<std::vector<float>, 2> points;
    std::array<std::vector<TraceBucket>, 2> columns;
};

inline float traceComponent(float value, int) { return value; }
inline float traceComponent(std::complex<float> value, int channel) {
    return channel == 0 ? value.real() : value.imag();
}

// Same floor(x * count / width) boundaries as the old renderer, without overflow.
inline size_t traceBoundary(size_t count, size_t x, size_t width) {
    return (count / width)*x + ((count % width)*x)/width;
}

template<typename T>
TraceSummary summarizeTrace(SampleSource<T> &source, size_t start, size_t count,
                            int width, const std::atomic<bool> &cancel)
{
    TraceSummary result;
    if (width <= 0 || !count || cancel.load()) return result;
    result.width = std::min(width, 32768);
    result.channels = std::is_same<T, float>::value ? 1 : 2;
    result.dense = count <= size_t(result.width)*16;
    for (int c = 0; c < result.channels; ++c) {
        if (result.dense)
            result.points[c].assign(count, std::numeric_limits<float>::quiet_NaN());
        else
            result.columns[c].resize(result.width);
    }
    const size_t total = source.count();
    const size_t available = start < total ? std::min(count, total-start) : 0;
    size_t column = 0;
    size_t columnEnd = traceBoundary(count, 1, result.width);
    for (size_t offset = 0; offset < available;) {
        if (cancel.load()) return result;
        // Align reads to cache blocks; source-level FIR history is retained.
        const size_t length = std::min(available-offset,
            TraceSummary::chunkSize - (start+offset)%TraceSummary::chunkSize);
        auto data = source.getSamples(start+offset, length);
        if (cancel.load()) return result;
        if (data) for (size_t i = 0; i < length; ++i) {
            const size_t index = offset+i;
            if (!result.dense) {
                while (index >= columnEnd && column+1 < size_t(result.width))
                    columnEnd = traceBoundary(count, ++column+1, result.width);
            }
            for (int c = 0; c < result.channels; ++c) {
                const float value = traceComponent(data[i], c);
                if (result.dense) result.points[c][index] = value;
                else result.columns[c][column].add(value);
                if (std::isfinite(value)) {
                    result.minimum = std::min(result.minimum, double(value));
                    result.maximum = std::max(result.maximum, double(value));
                }
            }
        }
        offset += length;
    }
    result.complete = !cancel.load();
    return result;
}
