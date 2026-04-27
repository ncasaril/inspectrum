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

void FrequencyDemod::invalidateBatchCache()
{
    QMutexLocker ml(&batchMutex_);
    batchCache_.valid = false;
    batchCache_.length = 0;
    batchCache_.startSample = 0;
    // Drop the buffer so we don't hang onto megabytes after a parameter
    // change — the next request will repopulate.
    std::vector<float>().swap(batchCache_.data);
}

void FrequencyDemod::invalidateEvent()
{
    invalidateBatchCache();
    SampleBuffer::invalidateEvent();
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
    invalidateBatchCache();
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
    invalidateBatchCache();
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
    invalidateBatchCache();
    invalidate();
}

void FrequencyDemod::setCheapDemod(bool enabled)
{
    {
        QMutexLocker ml(&mutex);
        if (enabled == cheapMode_) return;
        cheapMode_ = enabled;
    }
    invalidateBatchCache();
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
        // Filtfilt squares the magnitude response so stopband attenuation
        // doubles in dB, which reduces overshoot and lets us shrink the
        // settle budget vs single-pass. 1.0·N/cutoff_norm is empirically
        // enough for Butterworth (no equiripple tail).
        constexpr size_t kSettleCap = 500000;
        double settle = 1.0 * order / cutoff;
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
    // last lpfSettleSamples_ samples of every tile if run directly. The trick
    // is to pad on the right so the reverse-pass cold-start happens in the
    // pad region. Pad order matters here:
    //
    //   1) Forward pass over [data] → forward_out (smoothed; spikes already
    //      attenuated by the LPF).
    //   2) Pad with the LAST forward-output value held constant. Reverse pass
    //      starts in this constant region; with a unity-DC-gain filter and
    //      zero initial state it settles to that constant within
    //      lpfSettleSamples_ samples, matches the data boundary exactly,
    //      and continues into the data with no transient.
    //
    // (Earlier: odd-reflection of the raw freqdem output. That gave wild pad
    //  values whenever a spike landed near the right edge — e.g. a sample at
    //  1.02 next to data at 0.018 reflected to -0.99 — which made the reverse
    //  pass start chasing a non-physical level and bias the visible region.)
    const int pad = std::min<int>(static_cast<int>(lpfSettleSamples_),
                                  std::max(count, 1));
    std::vector<float> buf(static_cast<size_t>(count) + pad);
    std::memcpy(buf.data(), out, count * sizeof(float));

    // Forward pass over the data portion only.
    iirfilt_rrrf_reset(postIir_);
    for (int i = 0; i < count; ++i) {
        iirfilt_rrrf_execute(postIir_, buf[i], &buf[i]);
    }
    // Constant pad with the last forward-output sample so the reverse-pass
    // cold-start has a smooth, in-range value to settle to.
    if (pad > 0 && count > 0) {
        const float last = buf[count - 1];
        for (int i = 0; i < pad; ++i) buf[count + i] = last;
    }
    // Reverse pass over [data | constant pad]. Cold-start happens in the pad
    // region; by the time the pass reaches the data it's settled.
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

bool FrequencyDemod::fillBatchCache(size_t needStart, size_t needEnd)
{
    // Caller must hold batchMutex_. Builds (or rebuilds) the cache so that
    // it covers [needStart, needEnd). Pulls a wider range than strictly
    // requested so successive tile requests panning around the same view
    // hit the cache cheaply, and so the filtfilt has plenty of margin on
    // both sides for its forward and reverse passes to settle outside the
    // visible region.
    const size_t total = count();
    if (needEnd > total) needEnd = total;
    if (needStart >= needEnd) return false;

    // Snapshot filter parameters (the main mutex protects these from setter
    // changes; a setter that wins the race will also have called
    // invalidateBatchCache() so a stale cache won't survive).
    LpfMethod method;
    double    cutoffHz;
    double    fs;
    size_t    settle;
    bool      cheap;
    {
        QMutexLocker ml(&mutex);
        method   = postLpfMethod_;
        cutoffHz = postLpfCutoffHz_;
        fs       = rate();
        settle   = lpfSettleSamples_ ? lpfSettleSamples_ : 4096;
        cheap    = cheapMode_;
    }

    // Aim for a generous batch: at least 4× the requested range, with
    // settle-many samples of margin on each side, and never less than 1 M
    // samples (so casual panning stays in cache). Capped so a 100 GB file
    // doesn't try to allocate ridiculous amounts of memory.
    constexpr size_t kMinBatch = 1'000'000;
    constexpr size_t kMaxBatch = 8'000'000;
    const size_t reqLen = needEnd - needStart;
    size_t margin = std::max<size_t>(settle, 4096);
    size_t want   = std::max(kMinBatch, reqLen + 2 * margin);
    if (want > kMaxBatch) want = kMaxBatch;
    size_t centre = needStart + reqLen / 2;
    size_t half   = want / 2;
    size_t start  = (centre > half) ? (centre - half) : 0;
    size_t end    = std::min(start + want, total);
    // If we hit the end of the file, slide the window left so we still have
    // the requested range covered.
    if (end - start < want && start > 0) {
        size_t shift = std::min(start, want - (end - start));
        start -= shift;
    }
    if (start > needStart) start = needStart;
    if (end   < needEnd)   end   = needEnd;
    const size_t batchLen = end - start;
    if (batchLen == 0) return false;

    // Pull raw IQ from the upstream tuner. This one big getSamples call
    // does the upstream lead-in once (Kaiser FIR tuner cold-start), so the
    // freqdem and post-LPF below see a continuous, fully-warmed input.
    auto rawIq = src->getSamples(start, batchLen);
    if (!rawIq) return false;

    // Demod the whole batch with a single freqdem pass so its internal
    // state tracks the signal continuously across what would otherwise be
    // tile boundaries.
    std::vector<float> demod(batchLen);
    {
        QMutexLocker ml(&mutex);
        if (cheap) {
            if (batchLen > 0) {
                std::complex<float> prev = rawIq[0];
                demod[0] = 0.0f;
                for (size_t i = 1; i < batchLen; ++i) {
                    demod[i] = std::arg(rawIq[i] * std::conj(prev));
                    prev = rawIq[i];
                }
            }
        } else {
            freqdem_reset(fdem_);
            for (size_t i = 0; i < batchLen; ++i) {
                float dem;
                freqdem_demodulate(
                    fdem_,
                    *reinterpret_cast<liquid_float_complex*>(&rawIq[i]),
                    &dem);
                demod[i] = dem;
            }
        }
        // Scrub non-finite freqdem output before the LPF — recursive IIR
        // state would propagate any NaN forever.
        for (size_t i = 0; i < batchLen; ++i) {
            if (!std::isfinite(demod[i])) demod[i] = 0.0f;
        }

        // LPF (filtfilt for IIR; one-pass for FIR — although this code
        // path is only entered for IIR methods).
        if (method == LpfMethod::KaiserFir) {
            // For Kaiser the per-tile path is correct (FIR is composable);
            // we only end up here if the caller bypassed that intentionally.
            if (postFir_) {
                firfilt_rrrf_reset(postFir_);
                for (size_t i = 0; i < batchLen; ++i) {
                    firfilt_rrrf_push(postFir_, demod[i]);
                    firfilt_rrrf_execute(postFir_, &demod[i]);
                }
            }
        } else if (postIir_) {
            // Filtfilt with bilateral constant padding. The forward pass
            // cold-starts at the *start* of the buffer (state = 0); we pad
            // the left with `first_sample` so the forward filter settles
            // to that constant in the pad, then enters the data already
            // matched at the boundary. The reverse pass cold-starts at the
            // *end* of the buffer; we pad the right with the *forward
            // pass*'s last output value (after running forward) so the
            // reverse filter settles to that constant in its pad, then
            // enters the data with the right initial conditions.
            //
            // Net result: the entire data region is fully settled for both
            // passes — no NaN cold-start window needed at the start of the
            // file, no boundary transients anywhere.
            const size_t leftPad  = std::min<size_t>(settle, batchLen);
            const size_t rightPad = std::min<size_t>(settle, batchLen);
            std::vector<float> ext(leftPad + batchLen + rightPad);
            const float first = batchLen > 0 ? demod[0] : 0.0f;
            for (size_t i = 0; i < leftPad; ++i) ext[i] = first;
            std::memcpy(ext.data() + leftPad, demod.data(),
                        batchLen * sizeof(float));
            // Right pad gets the data's last value before forward pass.
            // The forward pass will smooth its output across the
            // pad/data boundary; the *output* in the right pad ends up
            // at whatever the filter settled to over the data, which is
            // exactly what we want as the reverse pass's seed.
            const float last = batchLen > 0 ? demod[batchLen - 1] : 0.0f;
            for (size_t i = 0; i < rightPad; ++i) {
                ext[leftPad + batchLen + i] = last;
            }

            iirfilt_rrrf_reset(postIir_);
            for (size_t i = 0; i < ext.size(); ++i) {
                iirfilt_rrrf_execute(postIir_, ext[i], &ext[i]);
            }
            iirfilt_rrrf_reset(postIir_);
            for (size_t i = 0; i < ext.size(); ++i) {
                const size_t j = ext.size() - 1 - i;
                iirfilt_rrrf_execute(postIir_, ext[j], &ext[j]);
            }
            // Copy back just the data portion — both pads are discarded.
            std::memcpy(demod.data(), ext.data() + leftPad,
                        batchLen * sizeof(float));
        }
        // No cold-start NaN window in batched mode: the bilateral pad
        // above means sample 0 of the file is just as settled as any
        // other sample, so there's nothing to hide.
    }

    batchCache_.startSample = start;
    batchCache_.length      = batchLen;
    batchCache_.data        = std::move(demod);
    batchCache_.valid       = true;
    batchCache_.method      = method;
    batchCache_.cutoffHz    = cutoffHz;
    batchCache_.rate        = fs;
    batchCache_.cheap       = cheap;

    FmLog::instance().writef(
        "fillBatchCache: method=%s cutoffHz=%.3f start=%zu len=%zu (covering "
        "request [%zu, %zu))\n",
        methodName(static_cast<int>(method)), cutoffHz, start, batchLen,
        needStart, needEnd);
    return true;
}

std::unique_ptr<float[]> FrequencyDemod::getSamples(size_t start, size_t length)
{
    // Snapshot the current method under the main mutex so we can decide
    // which path to take without holding it through the slow filter run.
    LpfMethod method;
    double    cutoffHz;
    {
        QMutexLocker ml(&mutex);
        method   = postLpfMethod_;
        cutoffHz = postLpfCutoffHz_;
    }

    // Kaiser FIR (or "no LPF" / cheap mode without LPF) is composable
    // across tile boundaries — fall through to the standard SampleBuffer
    // pattern, which has the parallel-friendly per-tile work() path.
    if (method == LpfMethod::KaiserFir || cutoffHz <= 0.0) {
        return SampleBuffer::getSamples(start, length);
    }

    // IIR backends: serve from the batch cache so all tiles in the same
    // view slice from a single continuous filtfilt pass.
    QMutexLocker ml(&batchMutex_);
    const bool covers = batchCache_.valid &&
                        batchCache_.method   == method &&
                        batchCache_.cutoffHz == cutoffHz &&
                        start >= batchCache_.startSample &&
                        (start + length) <=
                            (batchCache_.startSample + batchCache_.length);
    if (!covers) {
        if (!fillBatchCache(start, start + length)) return nullptr;
    }

    auto result = std::make_unique<float[]>(length);
    const size_t offset = start - batchCache_.startSample;
    std::memcpy(result.get(), batchCache_.data.data() + offset,
                length * sizeof(float));
    return result;
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
        // The kept/visible region is the last `count - postLpfLen_` samples;
        // SampleBuffer discards the leading postLpfLen_ samples (the lead-in
        // that warms up the filter). Compute stats just over that slice so
        // we can tell whether transients live in the discarded lead-in
        // (invisible) or leak into the visible plot.
        const size_t lead = std::min(postLpfLen_, static_cast<size_t>(count));
        size_t keptNan = 0;
        double keptMn = std::numeric_limits<double>::infinity();
        double keptMx = -std::numeric_limits<double>::infinity();
        for (size_t i = lead; i < static_cast<size_t>(count); ++i) {
            if (!std::isfinite(out[i])) { ++keptNan; continue; }
            if (out[i] < keptMn) keptMn = out[i];
            if (out[i] > keptMx) keptMx = out[i];
        }
        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff;
        FmLog::instance().writef(
            "work tid=0x%04zx sampleid=%zu count=%d method=%s cutoffHz=%.3f "
            "coldStart=%zu preMin=%.6g preMax=%.6g postMin=%.6g postMax=%.6g "
            "postNonFinite=%zu finalNaN=%zu finalMin=%.6g finalMax=%.6g "
            "keptLead=%zu keptNaN=%zu keptMin=%.6g keptMax=%.6g cheap=%d\n",
            tid, sampleid, count,
            methodName(static_cast<int>(postLpfMethod_)), postLpfCutoffHz_,
            coldStart, preMn, preMx, postMn, postMx,
            postNonFinite, nans, mn, mx,
            lead, keptNan, keptMn, keptMx, cheapMode_ ? 1 : 0);
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
