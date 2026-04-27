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
#include <liquid/liquid.h>

#include <QMutex>
#include <vector>

#include "samplebuffer.h"

class FrequencyDemod : public SampleBuffer<std::complex<float>, float>
{
public:
    // Available post-demod LPF implementations. KaiserFir is the original
    // (linear-phase, accurate, but very slow at narrow cutoffs);
    // ButterworthIir and EllipticIir are cheap-per-sample IIR alternatives.
    // Kept selectable so the user can A/B against the reference.
    enum class LpfMethod {
        KaiserFir = 0,
        ButterworthIir = 1,
        EllipticIir = 2,
    };

    FrequencyDemod(std::shared_ptr<SampleSource<std::complex<float>>> src);
    virtual ~FrequencyDemod();
    void work(void *input, void *output, int count, size_t sampleid) override;
    size_t historySize() override;
    // Override SampleBuffer's per-tile getSamples so the IIR backends can
    // run filtfilt over a *batched* range and serve sliced results from a
    // cache. Filtfilt is non-composable across tile boundaries (each tile's
    // independent forward+reverse pass picks up its own boundary conditions
    // → visible step discontinuities at every seam) so per-tile filtfilt
    // never produces a clean plot. Kaiser FIR is composable and falls
    // through to the standard per-tile path unchanged.
    std::unique_ptr<float[]> getSamples(size_t start, size_t length) override;
    void invalidateEvent() override;

    // Toggle fast-path demodulation (instantaneous phase diff).
    void setCheapDemod(bool enabled);
    // Post-demod LPF cutoff in Hz. 0 disables the filter.
    void setPostLpfCutoff(double hz);
    // Select which LPF implementation to use.
    void setPostLpfMethod(LpfMethod m);
    // Block-averaging decimation on the post-demod stream. N=1 disables.
    void setPostDecimation(int n);

private:
    // Liquid-DSP frequency demodulator object
    freqdem      fdem_;
    // Bandwidth used to create fdem_; rebuilt lazily in work() when the
    // upstream tuner's relativeBandwidth() changes (e.g. user dragged the
    // tuner cursors), so freqdem's modulation factor tracks the signal.
    float        fdemBuiltAtBandwidth_ = -1.0f;
    // Fast instantaneous-frequency demod (phase difference) instead of full FIR
    bool         cheapMode_ = false;
    // Post-demod LPF (built lazily when the upstream sample rate is known).
    // Two backends are supported — exactly one of postFir_/postIir_ is non-null
    // at a time depending on postLpfMethod_.
    double       postLpfCutoffHz_ = 0.0;
    LpfMethod    postLpfMethod_ = LpfMethod::KaiserFir;
    firfilt_rrrf postFir_ = nullptr;
    iirfilt_rrrf postIir_ = nullptr;
    size_t       postLpfLen_ = 0;
    // Number of samples the LPF needs to settle from a cold-start. For FIR
    // this is the tap count; for IIR it scales with 1/cutoff_norm. Used to
    // size both SampleBuffer's history and the cold-start NaN-mark window.
    size_t       lpfSettleSamples_ = 0;
    // Cached sample rate used to build the LPF; rebuilds when it changes.
    double       postLpfBuiltAtRate_ = 0.0;
    // Post-demod block-average decimation. 1 = disabled.
    int          postDecim_ = 1;

    void destroyPostLpf();
    void rebuildPostLpf();
    void applyPostLpf(float *out, int count);
    void applyPostDecimation(float *out, int count, size_t sampleid);

    // Batched-filter cache used by the IIR backends so that all tiles in a
    // frame slice from the same single forward+reverse pass — the only way
    // to make filtfilt produce a continuous result across tile boundaries.
    // Guarded by batchMutex_; filter rebuild and freqdem state still go
    // through the SampleBuffer base mutex.
    struct BatchCache {
        size_t       startSample = 0;
        size_t       length = 0;
        std::vector<float> data;
        bool         valid = false;
        // Snapshot the filter parameters used to populate this cache. Any
        // change here invalidates the entry on the next access.
        LpfMethod    method = LpfMethod::KaiserFir;
        double       cutoffHz = 0.0;
        double       rate = 0.0;
        bool         cheap = false;
    };
    QMutex      batchMutex_;
    BatchCache  batchCache_;
    void invalidateBatchCache();
    bool fillBatchCache(size_t needStart, size_t needEnd);
};
