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
#include <complex>
#include <algorithm>
#include <vector>

FrequencyDemod::FrequencyDemod(std::shared_ptr<SampleSource<std::complex<float>>> src) : SampleBuffer(src)
{
    // create the demodulator once
    fdem_ = freqdem_create(relativeBandwidth() / 2.0);
}

FrequencyDemod::~FrequencyDemod()
{
    if (postLpf_) firfilt_rrrf_destroy(postLpf_);
    freqdem_destroy(fdem_);
}

size_t FrequencyDemod::historySize()
{
    // The post-demod LPF is run across (history + length) samples and reset at
    // the start of each work() call, so the lead-in must cover the LPF taps or
    // the first output samples are filter transient.
    size_t base = SampleBuffer::historySize();
    if (postLpfLen_ > base) base = postLpfLen_;
    return base;
}

void FrequencyDemod::setPostLpfCutoff(double hz)
{
    if (hz < 0.0) hz = 0.0;
    if (hz == postLpfCutoffHz_) return;
    postLpfCutoffHz_ = hz;
    rebuildPostLpf();
    invalidate();
}

void FrequencyDemod::setPostDecimation(int n)
{
    if (n < 1) n = 1;
    if (n == postDecim_) return;
    postDecim_ = n;
    invalidate();
}

void FrequencyDemod::rebuildPostLpf()
{
    if (postLpf_) {
        firfilt_rrrf_destroy(postLpf_);
        postLpf_ = nullptr;
        postLpfLen_ = 0;
    }
    postLpfBuiltAtRate_ = rate();
    if (postLpfCutoffHz_ <= 0.0 || postLpfBuiltAtRate_ <= 0.0) return;

    // liquid_firdes_kaiser takes normalized cutoff (Fs=1).
    double cutoff = postLpfCutoffHz_ / postLpfBuiltAtRate_;
    if (cutoff >= 0.5) cutoff = 0.49;   // can't cut above Nyquist
    const float atten = 60.0f;
    // estimate_req_filter_len uses the transition bandwidth; use the cutoff
    // itself as a conservative proxy (also clamp for very narrow cutoffs so
    // filter length doesn't explode).
    unsigned int len = estimate_req_filter_len(std::max(cutoff, 1e-4), atten);
    if (len < 3) len = 3;
    std::vector<float> taps(len);
    liquid_firdes_kaiser(len, (float)cutoff, atten, 0.0f, taps.data());
    postLpf_ = firfilt_rrrf_create(taps.data(), len);
    postLpfLen_ = len;
}

void FrequencyDemod::applyPostLpf(float *out, int count)
{
    if (!postLpf_) return;
    // Reset state each call: getSamples ranges can be non-contiguous. The
    // SampleBuffer lead-in (historySize() covers the filter length) re-warms
    // the FIR before the first returned sample.
    firfilt_rrrf_reset(postLpf_);
    for (int i = 0; i < count; ++i) {
        firfilt_rrrf_push(postLpf_, out[i]);
        firfilt_rrrf_execute(postLpf_, &out[i]);
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
}
