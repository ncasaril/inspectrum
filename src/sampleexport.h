/* Copyright (C) 2026 inspectrum contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "samplesource.h"
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstring>
#include <vector>

// Output k is centred on input start+k*decim, independent of I/O chunking.
// A symmetric Blackman-windowed sinc suppresses aliases without shifting
// timestamps. Read the FIR halo from the capture; zero-pad only at file edges.
// Decimation 1 is a byte-for-byte copy. The callback returns false to cancel.
template<typename T>
bool writeSampleRange(QIODevice &out, SampleSource<T> &src, size_t start,
                      size_t count, int decim, QString *error,
                      std::function<bool(size_t, size_t)> progress = {},
                      size_t chunkSize = 65536)
{
    auto fail = [&](const QString &message) { *error = message; return false; };
    if (decim < 1 || decim > 65536 || !chunkSize)
        return fail("Invalid decimation or chunk size (decimation must be 1–65536).");
    const size_t total = src.count();
    if (!count || start > total || count > total - start)
        return fail("Empty or out-of-range sample selection.");
    const size_t d = size_t(decim);
    const size_t radius = decim == 1 ? 0 : 32 * d;
    std::vector<float> taps(2 * radius + 1, 1.0f);
    if (radius) {
        const double pi = std::acos(-1.0), cutoff = 0.45 / d;
        double sum = 0;
        for (size_t i = 0; i < taps.size(); ++i) {
            double x = double(i) - radius;
            double window = 0.42 + 0.5 * std::cos(pi * x / radius)
                                  + 0.08 * std::cos(2 * pi * x / radius);
            taps[i] = window * (x == 0 ? 2 * cutoff : std::sin(2 * pi * cutoff * x) / (pi * x));
            sum += taps[i];
        }
        for (auto &tap : taps) tap /= sum;
    }
    const size_t outputCount = 1 + (count - 1) / d;
    const size_t blockOutputs = std::max<size_t>(1, std::min<size_t>(chunkSize, 1u << 20) / d);
    std::vector<T> output;
    try {
        for (size_t k = 0; k < outputCount;) {
            if (progress && !progress(k * d, count)) return fail("canceled");
            const size_t n = std::min(blockOutputs, outputCount - k);
            const size_t first = start + k * d;
            const size_t last = first + (n - 1) * d;
            const size_t readStart = first - std::min(first, radius);
            const size_t readEnd = last + 1 + std::min(radius, total - last - 1);
            auto input = src.getSamples(readStart, readEnd - readStart);
            if (!input) return fail("Could not read source samples.");
            output.assign(n, T{});
            if (decim == 1) {
                std::memcpy(output.data(), input.get(), n * sizeof(T));
            } else {
                for (size_t j = 0; j < n; ++j) {
                    const size_t centre = first + j * d;
                    const size_t left = std::min(centre, radius);
                    const size_t right = std::min(radius, total - centre - 1);
                    const size_t begin = centre - left - readStart;
                    for (size_t t = 0; t <= left + right; ++t)
                        output[j] += input[begin + t] * taps[radius - left + t];
                }
            }
            const qint64 bytes = qint64(output.size() * sizeof(T));
            if (out.write(reinterpret_cast<const char*>(output.data()), bytes) != bytes)
                return fail("Failed writing samples: " + out.errorString());
            k += n;
        }
    } catch (const std::exception &e) {
        return fail(QString("Sample export failed: %1").arg(e.what()));
    }
    if (progress && !progress(count, count)) return fail("canceled");
    return true;
}
