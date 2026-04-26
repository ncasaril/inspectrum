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

#include "frequencydemod.h"
#include <liquid/liquid.h>
#include <QDebug>
#include <QMutexLocker>
#include <cmath>
#include <complex>
#include <algorithm>
#include <limits>
#include <vector>

// Toggle in one place — prints from the demod hot-path are noisy, so leave
// these off by default and flip to 1 when chasing a regression.
#define INSPECTRUM_FM_DEBUG 0

FrequencyDemod::FrequencyDemod(std::shared_ptr<SampleSource<std::complex<float>>> src) : SampleBuffer(src)
{
    // Use a fixed wide modulation factor (kf ≈ 0.49). This makes freqdem's
    // output scale invariant to the tuner bandwidth — narrowing or widening
    // the tuner now changes the *content* the demod sees but not the units of
    // its output, so the plot's y-axis stays calibrated and doesn't jump
    // wildly when the user drags the tuner cursors. The trade-off is reduced
    // dynamic range for very narrow signals (output values are smaller than
    // they'd be with a tightly-matched kf), but the plot auto-scales anyway.
    fdemBuiltAtBandwidth_ = 1.0f;
    fdem_ = freqdem_create(0.49f);
}

FrequencyDemod::~FrequencyDemod()
{
    if (postLpf_) iirfilt_rrrf_destroy(postLpf_);
    freqdem_destroy(fdem_);
}

size_t FrequencyDemod::historySize()
{
    QMutexLocker ml(&mutex);
    size_t base = SampleBuffer::historySize();
    if (postLpfLen_ > base) base = postLpfLen_;
    return base;
}

void FrequencyDemod::setPostLpfCutoff(double hz)
{
    if (hz < 0.0) hz = 0.0;
    {
        QMutexLocker ml(&mutex);
        if (hz == postLpfCutoffHz_) return;
        postLpfCutoffHz_ = hz;
        rebuildPostLpf(); // assumes mutex held
    }
    invalidate();
}

void FrequencyDemod::setPostDecimation(int n)
{
    if (n < 1) n = 1;
    {
        QMutexLocker ml(&mutex);
        if (n == postDecim_) return;
        postDecim_ = n;
    }
    invalidate();
}

void FrequencyDemod::rebuildPostLpf()
{
    if (postLpf_) {
        iirfilt_rrrf_destroy(postLpf_);
        postLpf_ = nullptr;
        postLpfLen_ = 0;
        lpfSettleSamples_ = 0;
    }
    postLpfBuiltAtRate_ = rate();
    if (postLpfCutoffHz_ <= 0.0 || postLpfBuiltAtRate_ <= 0.0) return;

    // Normalized cutoff (Fs = 1). Clamp away from 0 and 0.5 to keep the
    // Butterworth design well-conditioned.
    double cutoff = postLpfCutoffHz_ / postLpfBuiltAtRate_;
    if (cutoff >= 0.499) cutoff = 0.499;
    if (cutoff <= 1e-6)  cutoff = 1e-6;

    // Elliptic IIR (cascaded SOS): closest IIR shape to a Kaiser FIR's brick
    // wall — sharp transition with bounded passband / stopband ripple. Order
    // 8 gives a transition width comparable to a long Kaiser FIR at a tiny
    // fraction of the per-sample cost. SOS formatting keeps coefficients
    // numerically well-conditioned at low normalized cutoffs.
    const unsigned int order = 8;
    const float Ap = 0.1f;   // passband ripple (dB)
    const float As = 60.0f;  // stopband attenuation (dB)
    postLpf_ = iirfilt_rrrf_create_prototype(
        LIQUID_IIRDES_ELLIP, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS,
        order, static_cast<float>(cutoff), 0.0f, Ap, As);

    // Step-response settling time for a Butterworth lowpass scales like
    // 1/cutoff_norm. Empirically ~4 / cutoff_norm samples gets us well past
    // any visible overshoot ringing. Cap so a 1 Hz cutoff doesn't try to
    // allocate gigabyte lead-ins; cap value chosen to keep history under
    // ~1 MB of complex<float> upstream allocations.
    constexpr size_t kSettleCap = 200000;
    double settle = 4.0 / cutoff;
    if (settle > kSettleCap) settle = kSettleCap;
    lpfSettleSamples_ = static_cast<size_t>(settle);
    // postLpfLen_ feeds into historySize() so SampleBuffer fetches enough
    // upstream lead-in to consume the IIR transient before the returned
    // output begins.
    postLpfLen_ = lpfSettleSamples_;
}

void FrequencyDemod::applyPostLpf(float *out, int count)
{
    if (!postLpf_) return;
    // Reset state each call: getSamples ranges can be non-contiguous. The
    // SampleBuffer lead-in re-warms the IIR within ~order samples, so the
    // 64-sample historySize() leeway is plenty.
    iirfilt_rrrf_reset(postLpf_);
    for (int i = 0; i < count; ++i) {
        iirfilt_rrrf_execute(postLpf_, out[i], &out[i]);
    }
}

void FrequencyDemod::applyPostDecimation(float *out, int count, size_t sampleid)
{
    if (postDecim_ <= 1) return;
    const int N = postDecim_;
    // Windows are aligned to absolute sample index (sampleid + i) so that
    // successive getSamples() calls produce consistent values across call
    // boundaries.
    int i = 0;
    while (i < count) {
        const size_t abs_i = sampleid + static_cast<size_t>(i);
        const size_t win_abs_start = (abs_i / N) * N;
        int win_local_start = static_cast<int>(win_abs_start - sampleid);
        int win_local_end = win_local_start + N;
        int clip_start = std::max(0, win_local_start);
        int clip_end = std::min(count, win_local_end);
        if (clip_end <= clip_start) break;
        double sum = 0.0;
        for (int j = clip_start; j < clip_end; ++j) sum += out[j];
        float avg = static_cast<float>(sum / (clip_end - clip_start));
        for (int j = clip_start; j < clip_end; ++j) out[j] = avg;
        i = clip_end;
    }
}

void FrequencyDemod::work(void *input, void *output, int count, size_t sampleid)
{
    auto in  = static_cast<std::complex<float>*>(input);
    auto out = static_cast<float*>(output);

    // Lazy build of the post-demod LPF: the upstream sample rate is often not
    // set at construction time (file opens after the chain is built), so the
    // filter is rebuilt here whenever Fs changes and a cutoff is configured.
    if (postLpfCutoffHz_ > 0.0 && (!postLpf_ || postLpfBuiltAtRate_ != rate())) {
        rebuildPostLpf();
    }

    // (freqdem's kf is held fixed at construction — see comment in the ctor.)

    if (!cheapMode_) {
        // The demod is stateful and the same object is reused across all
        // getSamples() calls. Successive calls can be for non-contiguous
        // ranges (different tiles), so prior state is garbage. Reset here
        // and rely on SampleBuffer's lead-in samples to re-warm the filter.
        freqdem_reset(fdem_);
        // full FIR-based demod: run filter on every sample
        for (int i = 0; i < count; i++) {
            float dem;
            // interpret std::complex<float> bits as liquid_float_complex
            freqdem_demodulate(fdem_, *reinterpret_cast<liquid_float_complex*>(&in[i]), &dem);
            out[i] = dem;
        }
    } else {
        // cheap instantaneous-frequency demod: phase diff per sample
        if (count > 0) {
            std::complex<float> prev = in[0];
            out[0] = 0.0f;
            for (int i = 1; i < count; i++) {
                float dem = std::arg(in[i] * std::conj(prev));
                prev = in[i];
                out[i] = dem;
            }
        }
    }

    // Defensive scrub before the IIR: any non-finite freqdem sample (cold-
    // start, or numerical edge case) would poison the IIR's recursive state
    // and turn the entire rest of the chunk into NaN. Replace with zero so
    // the IIR sees a well-behaved input and just transients from there.
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(out[i])) out[i] = 0.0f;
    }

    applyPostLpf(out, count);
    applyPostDecimation(out, count, sampleid);

    // Mark filter-warmup samples as NaN. The size of the warmup depends on
    // what's running:
    //   - tuner FIR transient (~256 samples)
    //   - freqdem cold-start (~few samples)
    //   - post-LPF IIR settle (~4/cutoff_norm samples — can be many thousands)
    // The existing isfinite() guards in the scan and painter path then skip
    // these so globalMin/Max isn't biased by IIR overshoot ringing.
    const size_t coldStart = std::max<size_t>(512, lpfSettleSamples_);
    if (sampleid < coldStart) {
        const size_t bad = std::min<size_t>(coldStart - sampleid,
                                            static_cast<size_t>(count));
        for (size_t i = 0; i < bad; ++i) {
            out[i] = std::numeric_limits<float>::quiet_NaN();
        }
    }

#if INSPECTRUM_FM_DEBUG
    {
        size_t nans = 0;
        double mn = std::numeric_limits<double>::infinity();
        double mx = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < count; ++i) {
            if (!std::isfinite(out[i])) { ++nans; continue; }
            if (out[i] < mn) mn = out[i];
            if (out[i] > mx) mx = out[i];
        }
        qDebug().nospace() << "[FM] work sampleid=" << sampleid
                           << " count=" << count
                           << " nans=" << nans
                           << " finite_min=" << mn
                           << " finite_max=" << mx
                           << " cheap=" << cheapMode_;
    }
#endif
}
