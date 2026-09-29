/* Copyright (C) 2026 inspectrum contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

struct SpectrumPeak {
    int bin;
    float powerDb;
};

// Narrow, resolved local maxima only. Power stays on the input FFT-bin scale.
// Reject peaks below the median + threshold or with insufficient prominence
// within four bins on each side. Keep at most 16, separated by >= 3 bins.
inline std::vector<SpectrumPeak> detectSpectrumPeaks(const std::vector<float> &line,
                                                    int firstBin, float thresholdDb,
                                                    float rangeDb = 60)
{
    std::vector<SpectrumPeak> candidates, peaks;
    if (firstBin < 0 || firstBin >= int(line.size()) || !std::isfinite(thresholdDb) || thresholdDb < 0 ||
        !std::isfinite(rangeDb) || rangeDb < 0)
        return peaks;
    std::vector<float> floorValues;
    float strongest = -std::numeric_limits<float>::infinity();
    for (int i = firstBin; i < int(line.size()); ++i)
        if (std::isfinite(line[i]) || line[i] == -std::numeric_limits<float>::infinity()) {
            floorValues.push_back(line[i]);
            strongest = std::max(strongest, line[i]);
        }
    if (floorValues.empty()) return peaks;
    const auto mid = floorValues.begin() + floorValues.size()/2;
    std::nth_element(floorValues.begin(), mid, floorValues.end());
    const float floor = *mid;
    for (int i = firstBin+1; i+1 < int(line.size()); ++i) {
        const float p = line[i];
        if (!std::isfinite(p) || !(p > line[i-1] && p > line[i+1]) || p < floor+thresholdDb || p < strongest-rangeDb)
            continue;
        float left = p, right = p;
        for (int j = std::max(firstBin, i-4); j < i; ++j) left = std::min(left, line[j]);
        for (int j = i+1; j <= std::min(int(line.size())-1, i+4); ++j) right = std::min(right, line[j]);
        if (p - std::max(left, right) >= thresholdDb) candidates.push_back({i, p});
    }
    std::sort(candidates.begin(), candidates.end(), [](const SpectrumPeak &a, const SpectrumPeak &b) {
        return a.powerDb != b.powerDb ? a.powerDb > b.powerDb : a.bin < b.bin;
    });
    for (const auto &candidate : candidates) {
        bool separated = true;
        for (const auto &peak : peaks) if (std::abs(peak.bin-candidate.bin) < 3) separated = false;
        if (separated) peaks.push_back(candidate);
        if (peaks.size() == 16) break;
    }
    return peaks;
}
