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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

// Toggle in one place — prints from the demod hot-path are noisy, so leave
// these off by default and flip to 1 when chasing a regression.
#define INSPECTRUM_FM_DEBUG 0

namespace {
// File-scoped, lazily-opened debug log. Activated by setting the env var
// INSPECTRUM_FM_LOG to a filename (e.g. /tmp/inspectrum_fm.log). Writes are
// serialised on a single mutex so multi-tile traces stay readable. The whole
// thing is a no-op when the env var is unset, so leaving the support compiled
// in costs nothing on a normal run.
class FmLog {
public:
    static FmLog &instance() {
        static FmLog s;
        return s;
    }
    bool enabled() const { return file_ != nullptr; }
    void writef(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!file_) return;
        std::lock_guard<std::mutex> lk(m_);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(file_, fmt, ap);
        va_end(ap);
        fflush(file_);
    }
private:
    FmLog() {
        const char *p = std::getenv("INSPECTRUM_FM_LOG");
        if (p && *p) {
            file_ = std::fopen(p, "w");
            if (file_) {
                std::fprintf(file_, "# inspectrum FM demod log — fields: "
                                    "tid sampleid count method cutoffHz coldStart "
                                    "preNan finiteMin finiteMax postNan finiteMinAfter "
                                    "finiteMaxAfter\n");
                std::fflush(file_);
            }
        }
    }
    ~FmLog() { if (file_) std::fclose(file_); }
    std::mutex m_;
    std::FILE *file_ = nullptr;
};

const char *methodName(int m) {
    switch (m) {
    case 0: return "kaiserFir";
    case 1: return "butterIir";
    case 2: return "ellipIir";
    default: return "unknown";
    }
}
} // namespace

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
    destroyPostLpf();
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

void FrequencyDemod::setPostLpfMethod(LpfMethod m)
{
    {
        QMutexLocker ml(&mutex);
        if (m == postLpfMethod_) return;
        postLpfMethod_ = m;
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

void FrequencyDemod::destroyPostLpf()
{
    if (postFir_) { firfilt_rrrf_destroy(postFir_); postFir_ = nullptr; }
    if (postIir_) { iirfilt_rrrf_destroy(postIir_); postIir_ = nullptr; }
    postLpfLen_ = 0;
    lpfSettleSamples_ = 0;
}

void FrequencyDemod::rebuildPostLpf()
{
    destroyPostLpf();
    postLpfBuiltAtRate_ = rate();
    if (postLpfCutoffHz_ <= 0.0 || postLpfBuiltAtRate_ <= 0.0) return;

    // Normalized cutoff (Fs = 1). Clamp away from 0 and 0.5 so every
    // backend's design path stays well-conditioned.
    double cutoff = postLpfCutoffHz_ / postLpfBuiltAtRate_;
    if (cutoff >= 0.499) cutoff = 0.499;
    if (cutoff <= 1e-6)  cutoff = 1e-6;

    switch (postLpfMethod_) {
    case LpfMethod::KaiserFir: {
        // Reference linear-phase FIR. liquid's estimator wants tens of
        // thousands of taps at narrow cutoffs (~7300 at fc/fs=5e-4 for 60 dB
        // stopband); cap the length to keep per-tile cost bounded. At the
        // cap the transition width widens (worse stop-band rejection at the
        // very narrowest cutoffs) but visually it's still correct — the
        // linear-phase response preserves wave shape, which is the whole
        // reason this backend exists. 4096 taps gives ~9 kHz transition at
        // 10 MHz Fs, more than adequate for FM-audio-rate cutoffs.
        constexpr unsigned int kMaxTaps = 4096;
        const float atten = 60.0f;
        unsigned int len = estimate_req_filter_len(std::max(cutoff, 1e-4), atten);
        if (len < 3) len = 3;
        if (len > kMaxTaps) len = kMaxTaps;
        std::vector<float> taps(len);
        liquid_firdes_kaiser(len, static_cast<float>(cutoff), atten, 0.0f, taps.data());
        // liquid_firdes_kaiser returns un-normalised taps — sum diverges from
        // 1.0 by a few % at typical lengths, so the Kaiser path used to
        // produce a different magnitude than the IIRs. Renormalise to unity
        // DC gain so the three filter methods are directly comparable.
        double dc = 0.0;
        for (auto t : taps) dc += t;
        if (dc != 0.0) for (auto &t : taps) t = static_cast<float>(t / dc);
        postFir_ = firfilt_rrrf_create(taps.data(), len);
        // FIR is linear-phase; the impulse fully fits in `len` samples, so
        // the lead-in / cold-start window is just the tap count.
        postLpfLen_ = len;
        lpfSettleSamples_ = len;
        break;
    }
    case LpfMethod::ButterworthIir: {
        // Cheap maximally-flat IIR. ~36 dB/octave at order 6, no ripple.
        // Built via the SOS prototype path: cascaded biquads stay
        // numerically well-conditioned at low normalised cutoffs (a
        // direct-form order-6 at fc/fs = 5e-4 is broken in float32 — the
        // poles cluster at z=1 and the coefficients lose precision).
        const unsigned int order = 6;
        postIir_ = iirfilt_rrrf_create_prototype(
            LIQUID_IIRDES_BUTTER, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS,
            order, static_cast<float>(cutoff), 0.0f, 0.1f, 60.0f);
        constexpr size_t kSettleCap = 500000;
        double settle = 2.0 * order / cutoff;
        if (settle > kSettleCap) settle = kSettleCap;
        lpfSettleSamples_ = static_cast<size_t>(settle);
        postLpfLen_ = lpfSettleSamples_;
        break;
    }
    case LpfMethod::EllipticIir: {
        // Sharpest IIR transition for the order, at the cost of equiripple
        // passband / stopband. Cascaded SOS keeps coefficients numerically
        // well-conditioned at low normalized cutoffs.
        const unsigned int order = 8;
        const float Ap = 0.1f;   // passband ripple (dB)
        const float As = 60.0f;  // stopband attenuation (dB)
        postIir_ = iirfilt_rrrf_create_prototype(
            LIQUID_IIRDES_ELLIP, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS,
            order, static_cast<float>(cutoff), 0.0f, Ap, As);
        // Equiripple stopband leaves a slowly-decaying tail past the nominal
        // 1.6·N/cutoff_norm; double it so the cold-start NaN window covers
        // the actual ringing rather than chasing residual peaks.
        constexpr size_t kSettleCap = 500000;
        double settle = 3.2 * order / cutoff;
        if (settle > kSettleCap) settle = kSettleCap;
        lpfSettleSamples_ = static_cast<size_t>(settle);
        postLpfLen_ = lpfSettleSamples_;
        break;
    }
    }

    FmLog::instance().writef(
        "rebuildPostLpf: method=%s cutoffHz=%.3f Fs=%.0f cutoffNorm=%.6e "
        "postLpfLen=%zu lpfSettle=%zu\n",
        methodName(static_cast<int>(postLpfMethod_)),
        postLpfCutoffHz_, postLpfBuiltAtRate_, cutoff,
        postLpfLen_, lpfSettleSamples_);
}

void FrequencyDemod::applyPostLpf(float *out, int count)
{
    // Reset state each call: getSamples ranges can be non-contiguous. The
    // SampleBuffer lead-in (sized via postLpfLen_) re-warms the filter
    // before the first returned sample.
    if (postFir_) {
        firfilt_rrrf_reset(postFir_);
        for (int i = 0; i < count; ++i) {
            firfilt_rrrf_push(postFir_, out[i]);
            firfilt_rrrf_execute(postFir_, &out[i]);
        }
        return;
    }
    if (!postIir_) return;

    // IIR backends run filtfilt (forward then reversed) so the visible plot
    // gets a zero-phase response — the wave shape is preserved like a
    // linear-phase FIR even though we're using cheap-per-sample biquads.
    //
    // Forward pass uses the leading SampleBuffer history to warm up. Reverse
    // pass starts at the right-hand edge of the buffer and would corrupt the
    // last lpfSettleSamples_ samples of every tile if run directly. Pad the
    // right with edge-reflected samples (matches scipy.signal.filtfilt's
    // default) so the reverse pass cold-start happens in the pad region and
    // the real data is fully settled by the time it's reached.
    const int pad = std::min<int>(static_cast<int>(lpfSettleSamples_),
                                  std::max(count, 1));
    std::vector<float> buf(static_cast<size_t>(count) + pad);
    std::memcpy(buf.data(), out, count * sizeof(float));
    if (pad > 0 && count > 0) {
        // Odd reflection around the last sample: 2*last - x[count-2-i].
        // Avoids introducing a step at the boundary that would itself ring.
        const float last = out[count - 1];
        for (int i = 0; i < pad; ++i) {
            const int src = (count - 2 - i >= 0) ? (count - 2 - i) : 0;
            buf[count + i] = 2.0f * last - out[src];
        }
    }

    iirfilt_rrrf_reset(postIir_);
    for (size_t i = 0; i < buf.size(); ++i) {
        iirfilt_rrrf_execute(postIir_, buf[i], &buf[i]);
    }
    iirfilt_rrrf_reset(postIir_);
    for (size_t i = 0; i < buf.size(); ++i) {
        const size_t j = buf.size() - 1 - i;
        iirfilt_rrrf_execute(postIir_, buf[j], &buf[j]);
    }
    std::memcpy(out, buf.data(), count * sizeof(float));
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
    const bool haveLpf = (postFir_ || postIir_);
    if (postLpfCutoffHz_ > 0.0 && (!haveLpf || postLpfBuiltAtRate_ != rate())) {
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

    // Stats just before the LPF — useful when comparing what each filter is
    // fed against what it produces. Skip the per-sample scan unless the log
    // is actually open (env var is set), so the hot path stays cheap.
    double preMn = 0.0, preMx = 0.0;
    if (FmLog::instance().enabled()) {
        preMn = std::numeric_limits<double>::infinity();
        preMx = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < count; ++i) {
            if (out[i] < preMn) preMn = out[i];
            if (out[i] > preMx) preMx = out[i];
        }
    }

    applyPostLpf(out, count);
    applyPostDecimation(out, count, sampleid);

    // Stats after the LPF (still pre-NaN-mark) so we can see what the filter
    // produced before the cold-start mask hides it.
    double postMn = 0.0, postMx = 0.0;
    size_t postNonFinite = 0;
    if (FmLog::instance().enabled()) {
        postMn = std::numeric_limits<double>::infinity();
        postMx = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < count; ++i) {
            if (!std::isfinite(out[i])) { ++postNonFinite; continue; }
            if (out[i] < postMn) postMn = out[i];
            if (out[i] > postMx) postMx = out[i];
        }
    }

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

    if (FmLog::instance().enabled()) {
        size_t nans = 0;
        double mn = std::numeric_limits<double>::infinity();
        double mx = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < count; ++i) {
            if (!std::isfinite(out[i])) { ++nans; continue; }
            if (out[i] < mn) mn = out[i];
            if (out[i] > mx) mx = out[i];
        }
        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff;
        FmLog::instance().writef(
            "work tid=0x%04zx sampleid=%zu count=%d method=%s cutoffHz=%.3f "
            "coldStart=%zu preMin=%.6g preMax=%.6g postMin=%.6g postMax=%.6g "
            "postNonFinite=%zu finalNaN=%zu finalMin=%.6g finalMax=%.6g "
            "cheap=%d\n",
            tid, sampleid, count,
            methodName(static_cast<int>(postLpfMethod_)), postLpfCutoffHz_,
            coldStart, preMn, preMx, postMn, postMx,
            postNonFinite, nans, mn, mx, cheapMode_ ? 1 : 0);
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
