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
    // create the demodulator once
    fdem_ = freqdem_create(relativeBandwidth() / 2.0);
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
    }
    postLpfBuiltAtRate_ = rate();
    if (postLpfCutoffHz_ <= 0.0 || postLpfBuiltAtRate_ <= 0.0) return;

    // Normalized cutoff (Fs = 1). Clamp away from 0 and 0.5 to keep the
    // Butterworth design well-conditioned.
    double cutoff = postLpfCutoffHz_ / postLpfBuiltAtRate_;
    if (cutoff >= 0.499) cutoff = 0.499;
    if (cutoff <= 1e-6)  cutoff = 1e-6;

    // 6th-order Butterworth: ~36 dB/octave rolloff and ~6-tap impulse decay.
    // Far cheaper than a Kaiser FIR (which needs thousands of taps for the
    // same cutoff) and the non-linear phase doesn't matter for visualization.
    const unsigned int order = 6;
    postLpf_ = iirfilt_rrrf_create_lowpass(order, static_cast<float>(cutoff));
    // Used for SampleBuffer lead-in sizing. IIR settles much faster than the
    // old FIR; a small constant comfortably covers the impulse response of
    // typical orders (≤ ~12).
    postLpfLen_ = 64;
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

    applyPostLpf(out, count);
    applyPostDecimation(out, count, sampleid);

    // Mark filter-warmup samples as NaN. SampleBuffer can only fetch
    // min(start, historySize()) samples of lead-in, so when sampleid is small
    // (the user is viewing near t=0) the upstream tuner FIR and freqdem are
    // running from cold zero state and the first samples of work() output are
    // filter transient — biased toward zero, not real FM. Explicit NaN means
    // the existing isfinite() guards in the min/max scan and the painter path
    // skip them rather than treating zeros as real data points and pulling
    // globalMin/Max toward zero.
    constexpr size_t COLD_START = 512;
    if (sampleid < COLD_START) {
        const size_t bad = std::min<size_t>(COLD_START - sampleid,
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
