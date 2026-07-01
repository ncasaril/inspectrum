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

#pragma once

#include <atomic>
#include "samplebuffer.h"

class AmplitudeDemod : public SampleBuffer<std::complex<float>, float>
{
public:
    AmplitudeDemod(std::shared_ptr<SampleSource<std::complex<float>>> src);
    void work(void *input, void *output, int count, size_t sampleid) override;
    // work() only reads dbMode_/refDbm_ (snapshotted per call via atomic
    // loads) and never mutates instance state, so it stays lock-free for
    // parallel tile workers.
    bool workIsReentrant() override { return true; }

    // Switch the output between the default linear power (2|x|²-1) and a
    // logarithmic scale: 10·log10(|x|²) + reference. Invalidates so cached
    // trace tiles recompute.
    void setDbMode(bool on);
    // Reference level (dBm) that maps to full scale (|x| = 1). Added to the
    // dB output so the plot reads calibrated dBm; 0 leaves it as dBFS.
    void setReferenceLevelDbm(double dbm);
    bool dbMode() const { return dbMode_.load(std::memory_order_relaxed); }
    double referenceLevelDbm() const { return refDbm_.load(std::memory_order_relaxed); }

private:
    // Lock-free so the reentrant work() can read them without serialising the
    // tile workers; GUI-thread setters store + invalidate().
    std::atomic<bool> dbMode_{false};
    std::atomic<double> refDbm_{0.0};
};
