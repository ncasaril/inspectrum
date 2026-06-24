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

#include "tunertransform.h"
#include <liquid/liquid.h>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "util.h"

TunerTransform::TunerTransform(std::shared_ptr<SampleSource<std::complex<float>>> src) : SampleBuffer(src), frequency(0), bandwidth(1.), taps{1.0f}
{

}

void TunerTransform::work(void *input, void *output, int count, size_t sampleid)
{
    auto out = static_cast<std::complex<float>*>(output);
    auto temp = std::make_unique<std::complex<float>[]>(count);

    // Snapshot parameters under the short-hold paramMutex_ and drop it
    // before the heavy NCO+FIR loop. The base class's `mutex` is not held
    // here (getSamples is overridden), and the GUI-thread setters use
    // paramMutex_, so they aren't blocked by this work() running.
    float freqLocal;
    std::vector<float> tapsLocal;
    {
        QMutexLocker ml(&paramMutex_);
        freqLocal = frequency;
        tapsLocal = taps;
    }

    // Mix down
    nco_crcf mix = nco_crcf_create(LIQUID_NCO);
    nco_crcf_set_phase(mix, fmodf(freqLocal * sampleid, Tau));
    nco_crcf_set_frequency(mix, freqLocal);
    nco_crcf_mix_block_down(mix,
                            static_cast<std::complex<float>*>(input),
                            temp.get(),
                            count);
    nco_crcf_destroy(mix);

    // Filter
    firfilt_crcf filter = firfilt_crcf_create(tapsLocal.data(), tapsLocal.size());
    for (int i = 0; i < count; i++)
    {
        firfilt_crcf_push(filter, temp[i]);
        firfilt_crcf_execute(filter, &out[i]);
    }
    firfilt_crcf_destroy(filter);
}

void TunerTransform::setFrequency(float frequency)
{
    QMutexLocker ml(&paramMutex_);
    if (this->frequency == frequency)
        return;
    this->frequency = frequency;
    // Self-invalidate the block cache so we don't depend on the caller pairing
    // every setter with notifyChanged(). bumpEpoch() is a non-blocking atomic
    // and never touches cacheMutex_; cacheMutex_ and paramMutex_ are never held
    // simultaneously anywhere (getSamples computes blocks OUTSIDE cacheMutex_),
    // so there is no deadlock.
    bumpEpoch();
}

void TunerTransform::setTaps(std::vector<float> taps)
{
    QMutexLocker ml(&paramMutex_);
    this->taps = std::move(taps);
    bumpEpoch();
}

float TunerTransform::relativeBandwidth() {
    QMutexLocker ml(&paramMutex_);
    return bandwidth;
}

void TunerTransform::setRelativeBandwith(float bandwidth)
{
    // Bandwidth never enters the mix/FIR in work() — it's only reported via
    // relativeBandwidth() — so it does NOT invalidate the tuned-IQ cache.
    // (A future resampler in work() would have to change this.)
    QMutexLocker ml(&paramMutex_);
    this->bandwidth = bandwidth;
}

size_t TunerTransform::historySize()
{
    QMutexLocker ml(&paramMutex_);
    return std::max(static_cast<size_t>(256), taps.size());
}

void TunerTransform::bumpEpoch()
{
    cacheEpoch_.fetch_add(1, std::memory_order_release);
}

void TunerTransform::invalidateEvent()
{
    // Upstream changed (new file, etc.): drop the cached blocks. Bump the epoch
    // BEFORE forwarding so any re-request the fan-out triggers already sees the
    // advanced epoch and refills.
    bumpEpoch();
    SampleBuffer::invalidateEvent();
}

bool TunerTransform::computeInto(size_t start, size_t length, std::complex<float> *out)
{
    // Mirror SampleBuffer::getSamples but lock-free: pull a FIR-history lead-in
    // on the LEFT (clamped at the file start) so the fresh-FIR cold-start
    // transient lives in the discarded lead-in, and pass the absolute index of
    // the first PULLED sample as sampleid so the NCO phase is correct.
    const size_t history = std::min(start, this->historySize());
    auto raw = src->getSamples(start - history, length + history);
    if (!raw)
        return false;
    auto temp = std::make_unique<std::complex<float>[]>(length + history);
    work(raw.get(), temp.get(), static_cast<int>(length + history), start - history);
    std::memcpy(out, temp.get() + history, length * sizeof(std::complex<float>));
    return true;
}

std::unique_ptr<std::complex<float>[]> TunerTransform::computeRange(size_t start, size_t length)
{
    auto out = std::make_unique<std::complex<float>[]>(length);
    if (!computeInto(start, length, out.get()))
        return nullptr;
    return out;
}

TunerTransform::Block TunerTransform::computeBlock(size_t blockIdx)
{
    const size_t total = count();
    const size_t blkStart = blockIdx * kBlock;
    if (blkStart >= total)
        return nullptr;
    const size_t blkLen = std::min(kBlock, total - blkStart);
    auto vec = std::make_shared<std::vector<std::complex<float>>>(blkLen);
    if (!computeInto(blkStart, blkLen, vec->data()))
        return nullptr;
    return vec;
}

void TunerTransform::touchLocked(size_t blockIdx)
{
    lru_.remove(blockIdx);     // n <= kMaxBlocks, so O(n) is cheap
    lru_.push_front(blockIdx);
}

std::unique_ptr<std::complex<float>[]> TunerTransform::getSamples(size_t start, size_t length)
{
    if (length == 0)
        return std::make_unique<std::complex<float>[]>(0);
    const size_t total = count();
    if (start >= total || length > total - start)
        return nullptr; // out of range — match the upstream contract

    const size_t b0 = start / kBlock;
    const size_t b1 = (start + length - 1) / kBlock;

    // Large requests bypass the cache: they'd evict everyone else's blocks and
    // thrash a bounded LRU. Compute directly in one pass, exactly like the
    // pre-cache path (and like FrequencyDemod's own large batch pull).
    if (b1 - b0 + 1 > kBypassBlocks)
        return computeRange(start, length);

    const uint64_t nowEpoch = cacheEpoch_.load(std::memory_order_acquire);
    auto result = std::make_unique<std::complex<float>[]>(length);

    for (size_t b = b0; b <= b1; ++b) {
        Block blk;
        bool stale = false;
        {
            QMutexLocker lk(&cacheMutex_);
            // Only an OLDER map gets dropped. mapEpoch_ never exceeds the live
            // cacheEpoch_, so mapEpoch_ > nowEpoch means a newer generation
            // already owns the map and *we* are a stale worker draining an old
            // frame — don't clear it (that would ping-pong against the live
            // workers and zero the hit rate); just compute directly.
            if (mapEpoch_ < nowEpoch) {
                blocks_.clear();
                lru_.clear();
                mapEpoch_ = nowEpoch;
            }
            if (mapEpoch_ == nowEpoch) {
                auto it = blocks_.find(b);
                if (it != blocks_.end()) {
                    blk = it->second;
                    touchLocked(b);
                }
            } else {
                stale = true;
            }
        }
        if (!blk) {
            // Miss (or stale): compute OUTSIDE the lock (work() is reentrant).
            blk = computeBlock(b);
            if (!blk)
                return nullptr;
            if (!stale) {
                QMutexLocker lk(&cacheMutex_);
                // Insert only while we're genuinely current — no epoch bump
                // since entry AND the map is still our generation. Otherwise a
                // repaint under the new generation will supersede this frame.
                if (cacheEpoch_.load(std::memory_order_acquire) == nowEpoch &&
                    mapEpoch_ == nowEpoch) {
                    auto it = blocks_.find(b);
                    if (it == blocks_.end()) {
                        blocks_.emplace(b, blk);
                        lru_.push_front(b);
                        while (lru_.size() > kMaxBlocks) {
                            blocks_.erase(lru_.back());
                            lru_.pop_back();
                        }
                    } else {
                        blk = it->second; // someone else computed it first; share theirs
                        touchLocked(b);
                    }
                }
            }
        }

        // Copy this block's overlap with [start, start+length) into the result.
        const size_t blkStart = b * kBlock;
        const size_t blkEnd = blkStart + blk->size();
        const size_t copyStart = std::max(start, blkStart);
        const size_t copyEnd = std::min(start + length, blkEnd);
        if (copyEnd > copyStart) {
            std::memcpy(result.get() + (copyStart - start),
                        blk->data() + (copyStart - blkStart),
                        (copyEnd - copyStart) * sizeof(std::complex<float>));
        }
    }
    return result;
}
