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
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
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
    // reentrant. getSamples() below is overridden (block cache), so the base
    // SampleBuffer path is never taken — but the block fills call work() lock-free
    // and in parallel, so this reentrancy is load-bearing. Safety relies on
    // liquid-dsp keeping all nco/firfilt/dotprod state per-object (true through
    // >= v1.3.2); revisit if a liquid upgrade adds a shared design cache.
    bool workIsReentrant() override { return true; }
    void setFrequency(float frequency);
    void setTaps(std::vector<float> taps);
    void setRelativeBandwith(float bandwidth);
    float relativeBandwidth() override;
    // Notify subscribers (downstream demods, which cascade to their plots)
    // that the mix frequency / filter / bandwidth have changed and any
    // cached data is stale. Called once after a batch of setters. (The block
    // cache is self-invalidating — see the setters — so this is purely the
    // downstream fan-out now.)
    void notifyChanged() { invalidate(); }
    // The FIR is rebuilt from zero state each call, so the lead-in must cover
    // at least the tap count or the first output samples will be attenuated
    // filter transient — visible as noise in downstream demods.
    size_t historySize() override;

    // Shared block cache of tuned IQ. Every derived plot pulls the tuned output
    // through this one TunerTransform; without a cache each re-runs the NCO mix
    // + FIR over its range, redundantly and on every frame. getSamples() serves
    // BLK-aligned absolute blocks: a hit is a memcpy under a short lock; a miss
    // computes the block lock-free (work() is reentrant) and inserts it. So
    // disjoint consumers fill different blocks in parallel and overlapping ones
    // (and pans) share warm blocks. Invalidated by bumping cacheEpoch_ (atomic,
    // non-blocking); the map is lazily dropped when its epoch goes stale.
    std::unique_ptr<std::complex<float>[]> getSamples(size_t start, size_t length) override;
    void invalidateEvent() override;

private:
    using Block = std::shared_ptr<const std::vector<std::complex<float>>>;
    static constexpr size_t kBlock = 65536;       // samples per cache block
    static constexpr size_t kMaxBlocks = 128;     // ~8.4M samples / ~64 MB cap
    static constexpr size_t kBypassBlocks = 96;   // larger requests skip the cache

    QMutex                cacheMutex_;             // guards blocks_/lru_/mapEpoch_ only
    std::atomic<uint64_t> cacheEpoch_{1};
    uint64_t              mapEpoch_ = 0;           // epoch the current map belongs to
    std::unordered_map<size_t, Block> blocks_;     // blockIndex -> data
    std::list<size_t>     lru_;                     // front = most-recently-used

    void bumpEpoch();
    // Pull upstream IQ with a FIR-history lead-in and run work() into `out`;
    // [start, start+length). Lock-free (work() reentrant). Returns false on a
    // null upstream read (out-of-range).
    bool computeInto(size_t start, size_t length, std::complex<float> *out);
    std::unique_ptr<std::complex<float>[]> computeRange(size_t start, size_t length);
    Block computeBlock(size_t blockIdx);
    void touchLocked(size_t blockIdx);             // assumes cacheMutex_ held
};
