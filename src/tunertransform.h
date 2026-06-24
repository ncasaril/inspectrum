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

#include "samplebuffer.h"
#include <QMutex>
#include <vector>

class TunerTransform : public SampleBuffer<std::complex<float>, std::complex<float>>
{
private:
    // Tuner parameters get a dedicated short-hold mutex distinct from
    // SampleBuffer's `mutex`. The base class holds `mutex` for the entire
    // duration of every work() call (which can be hundreds of ms over a
    // wide sample range), so reusing it for the GUI-thread setters means
    // a tuner drag stalls behind in-flight workers — visible in the
    // latency trace as 300-700 ms tunerMoved hangs. work() snapshots the
    // params under paramMutex_ at the top and drops the lock immediately;
    // setters do the same.
    mutable QMutex paramMutex_;
    float frequency;
    float bandwidth;
    std::vector<float> taps;

public:
    TunerTransform(std::shared_ptr<SampleSource<std::complex<float>>> src);
    void work(void *input, void *output, int count, size_t sampleid) override;
    // work() uses only local NCO/FIR objects + a paramMutex_ snapshot, so it's
    // reentrant — run it lock-free so every derived plot's tile workers can
    // mix+filter the shared tuner output concurrently rather than single-file.
    // Safety relies on liquid-dsp keeping all nco/firfilt/dotprod state
    // per-object (true through >= v1.3.2); revisit if a liquid upgrade adds a
    // shared design cache on the create paths.
    bool workIsReentrant() override { return true; }
    void setFrequency(float frequency);
    void setTaps(std::vector<float> taps);
    void setRelativeBandwith(float bandwidth);
    float relativeBandwidth() override;
    // Notify subscribers (downstream demods, which cascade to their plots)
    // that the mix frequency / filter / bandwidth have changed and any
    // cached data is stale. Called once after a batch of setters.
    void notifyChanged() { invalidate(); }
    // The FIR is rebuilt from zero state each call, so the lead-in must cover
    // at least the tap count or the first output samples will be attenuated
    // filter transient — visible as noise in downstream demods.
    size_t historySize() override;
};
