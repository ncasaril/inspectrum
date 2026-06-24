/*
 *  Copyright (C) 2015, Mike Walters <mike@flomp.net>
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

#include <QMutex>
#include <complex>
#include <memory>
#include "samplesource.h"

template <typename Tin, typename Tout>
class SampleBuffer : public SampleSource<Tout>, public Subscriber
{
protected:
    // Upstream source. Exposed to subclasses so they can override
    // getSamples and pull a different range (e.g. a batched window for
    // non-composable filters like IIR filtfilt) without going through the
    // standard per-tile work() pattern.
    std::shared_ptr<SampleSource<Tin>> src;
    // Protects work() state (and, by extension, anything work() reads).
    // Setters in subclasses that mutate that state should lock this mutex
    // too so they can't race with scans running on worker threads.
    QMutex mutex;

public:
    SampleBuffer(std::shared_ptr<SampleSource<Tin>> src);
    ~SampleBuffer();
    void invalidateEvent();
    virtual std::unique_ptr<Tout[]> getSamples(size_t start, size_t length);
    virtual void work(void *input, void *output, int count, size_t sampleid) = 0;
    // Override to return true when work() carries no mutable per-instance state
    // (only locals + its own parameter snapshot). getSamples() then runs it
    // WITHOUT taking `mutex`, so many tile workers can transform the same shared
    // node concurrently instead of serialising on it. Default false (safe):
    // stateful nodes like FrequencyDemod keep the lock.
    virtual bool workIsReentrant() { return false; }
    virtual size_t count() {
        return src->count();
    };
    double rate() {
        return src->rate();
    };

    float relativeBandwidth() {
        return src->relativeBandwidth();
    }
    // Number of pre-history samples getSamples() fetches and discards so
    // subclasses can warm their internal filters before producing real output.
    // Override if a transform needs more than the default 256-sample lead-in
    // (e.g., a long FIR whose impulse response exceeds it).
    virtual size_t historySize() { return 256; }
};
