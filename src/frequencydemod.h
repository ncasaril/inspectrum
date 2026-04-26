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

#include "samplebuffer.h"

class FrequencyDemod : public SampleBuffer<std::complex<float>, float>
{
public:
    FrequencyDemod(std::shared_ptr<SampleSource<std::complex<float>>> src);
    virtual ~FrequencyDemod();
    void work(void *input, void *output, int count, size_t sampleid) override;
    size_t historySize() override;

    // Toggle fast-path demodulation (instantaneous phase diff).
    void setCheapDemod(bool enabled) { cheapMode_ = enabled; }
    // Post-demod LPF cutoff in Hz. 0 disables the filter.
    void setPostLpfCutoff(double hz);
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
    // IIR Butterworth — fast at any cutoff, short impulse response (a few
    // samples). Order 6 ≈ 36 dB/octave which is plenty for FM noise cleanup
    // and avoids the huge tap count a Kaiser FIR would need at low Fc.
    double       postLpfCutoffHz_ = 0.0;
    iirfilt_rrrf postLpf_ = nullptr;
    size_t       postLpfLen_ = 0;
    // Number of samples the IIR needs to settle from a cold-start. Scales
    // with 1/cutoff_norm (narrow filter → longer settle); used to size both
    // SampleBuffer's history and the cold-start NaN-mark window.
    size_t       lpfSettleSamples_ = 0;
    // Cached sample rate used to build postLpf_; rebuilds when it changes.
    double       postLpfBuiltAtRate_ = 0.0;
    // Post-demod block-average decimation. 1 = disabled.
    int          postDecim_ = 1;

    void rebuildPostLpf();
    void applyPostLpf(float *out, int count);
    void applyPostDecimation(float *out, int count, size_t sampleid);
};
