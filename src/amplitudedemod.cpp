/*
 *  Copyright (C) 2016, Mike Walters <mike@flomp.net>
 *
 *  This file is part of inspectrum.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "amplitudedemod.h"
#include <algorithm>
#include <cmath>

AmplitudeDemod::AmplitudeDemod(std::shared_ptr<SampleSource<std::complex<float>>> src) : SampleBuffer(src)
{

}

void AmplitudeDemod::work(void *input, void *output, int count, size_t sampleid)
{
    auto in = static_cast<std::complex<float>*>(input);
    auto out = static_cast<float*>(output);

    // Snapshot the display mode once per call so a mid-flight setter can't
    // make the tile half-linear / half-dB.
    const bool db = dbMode_.load(std::memory_order_relaxed);
    if (!db) {
        std::transform(in, in + count, out,
                       [](std::complex<float> s) { return std::norm(s) * 2.0f - 1.0f; });
        return;
    }

    // dB scale: 10·log10(|x|²) = 20·log10(|x|), plus the full-scale reference
    // so the trace reads in dBFS (ref 0) or calibrated dBm (ref set). Silence
    // (|x| = 0) is floored instead of producing -inf, which would poison the
    // autoscale and leave a gap in the trace.
    const float ref = static_cast<float>(refDbm_.load(std::memory_order_relaxed));
    constexpr float kFloorDb = -120.0f;
    for (int i = 0; i < count; i++) {
        const float p = std::norm(in[i]);
        float dbv = (p > 0.0f) ? 10.0f * std::log10(p) : kFloorDb;
        if (dbv < kFloorDb) dbv = kFloorDb;
        out[i] = dbv + ref;
    }
}

void AmplitudeDemod::setDbMode(bool on)
{
    if (dbMode_.exchange(on, std::memory_order_relaxed) == on)
        return;
    invalidate();
}

void AmplitudeDemod::setReferenceLevelDbm(double dbm)
{
    if (refDbm_.exchange(dbm, std::memory_order_relaxed) == dbm)
        return;
    invalidate();
}
