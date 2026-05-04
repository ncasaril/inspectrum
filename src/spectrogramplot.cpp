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

#include "spectrogramplot.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFutureSynchronizer>
#include <QMutexLocker>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmapCache>
#include <QRect>
#include <QThreadPool>
#include <QtConcurrent>
#include <liquid/liquid.h>
#include <algorithm>
#include <functional>
#include <cstdlib>
#include <limits>
#include "util.h"
#include "latencylog.h"


SpectrogramPlot::SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>> src) : Plot(src), inputSource(src), fftSize(512), tuner(fftSize, this)
{
    setFFTSize(fftSize);
    zoomLevel = 1;
    nfftSkip = 1;
    powerMax = 0.0f;
    powerMin = -50.0f;
    sampleRate = 0;
    frequencyScaleEnabled = false;
    sigmfAnnotationsEnabled = true;
    sigmfAnnotationLabels = true;
    sigmfAnnotationColors = true;

    // Default QCache cost limit is 100. A single wide-zoom view of a long
    // capture can need 200+ tiles, which means every paint evicts tiles it
    // just rendered → synchronous FFT recompute on the GUI thread → 200 ms
    // paints. Each tile pixmap and fft entry is 256 kB (tileSize × 4 B), so
    // 512 entries gives a ~256 MB combined budget that covers very wide
    // views with comfortable headroom for pan history.
    pixmapCache.setMaxCost(512);
    fftCache.setMaxCost(512);

    for (int i = 0; i < 256; i++) {
        float p = (float)i / 256;
        colormap[i] = QColor::fromHsvF(p * 0.83f, 1.0, 1.0 - p).rgba();
    }

    tunerTransform = std::make_shared<TunerTransform>(src);
    connect(&tuner, &Tuner::tunerMoved, this, &SpectrogramPlot::tunerMoved);
}

void SpectrogramPlot::invalidateEvent()
{
    // HACK: this makes sure we update the height for real signals (as InputSource is passed here before the file is opened)
    setFFTSize(fftSize);

    pixmapCache.clear();
    fftCache.clear();
    emit repaint();
}

void SpectrogramPlot::paintFront(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    if (tunerEnabled())
        tuner.paintFront(painter, rect, sampleRange);

    if (frequencyScaleEnabled)
        paintFrequencyScale(painter, rect);

    if (sigmfAnnotationsEnabled)
        paintAnnotations(painter, rect, sampleRange);
}

void SpectrogramPlot::paintFrequencyScale(QPainter &painter, QRect &rect)
{
    if (sampleRate == 0) {
        return;
    }

    if (sampleRate / 2 > UINT64_MAX) {
        return;
    }

    // At which pixel is F_+sampleRate/2
    int y = rect.y();

    int plotHeight = rect.height();
    if (inputSource->realSignal())
        plotHeight *= 2;

    double bwPerPixel = (double)sampleRate / plotHeight;
    int tickHeight = 50;

    uint64_t bwPerTick = 10 * pow(10, floor(log(bwPerPixel * tickHeight) / log(10)));

    if (bwPerTick < 1) {
        return;
    }

    painter.save();

    QPen pen(Qt::white, 1, Qt::SolidLine);
    painter.setPen(pen);
    QFontMetrics fm(painter.font());


    uint64_t tick = 0;

    while (tick <= sampleRate / 2) {

        int tickpy = plotHeight / 2 - tick / bwPerPixel + y;
        int tickny = plotHeight / 2 + tick / bwPerPixel + y;

        if (!inputSource->realSignal())
            painter.drawLine(0, tickny, 30, tickny);
        painter.drawLine(0, tickpy, 30, tickpy);

        if (tick != 0) {
            char buf[128];

            if (bwPerTick % 1000000000 == 0) {
                snprintf(buf, sizeof(buf), "-%lu GHz", tick / 1000000000);
            } else if (bwPerTick % 1000000 == 0) {
                snprintf(buf, sizeof(buf), "-%lu MHz", tick / 1000000);
            } else if(bwPerTick % 1000 == 0) {
                snprintf(buf, sizeof(buf), "-%lu kHz", tick / 1000);
            } else {
                snprintf(buf, sizeof(buf), "-%lu Hz", tick);
            }

            if (!inputSource->realSignal())
                painter.drawText(5, tickny - 5, buf);

            buf[0] = ' ';
            painter.drawText(5, tickpy + 15, buf);
        }

        tick += bwPerTick;
    }

    // Draw small ticks
    bwPerTick /= 10;

    if (bwPerTick >= 1 ) {
        tick = 0;
        while (tick <= sampleRate / 2) {

            int tickpy = plotHeight / 2 - tick / bwPerPixel + y;
            int tickny = plotHeight / 2 + tick / bwPerPixel + y;

            if (!inputSource->realSignal())
                painter.drawLine(0, tickny, 3, tickny);
            painter.drawLine(0, tickpy, 3, tickpy);

            tick += bwPerTick;
        }
    }
    painter.restore();
}

void SpectrogramPlot::paintAnnotations(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    // Pixel (from the top) at which 0 Hz sits
    int zero = rect.y() + rect.height() / 2;

    painter.save();
    QPen pen(Qt::white, 1, Qt::SolidLine);
    painter.setPen(pen);
    QFontMetrics fm(painter.font());

    visibleAnnotationLocations.clear();

    for (int i = 0; i < inputSource->annotationList.size(); i++) {
        Annotation a = inputSource->annotationList.at(i);

        size_t labelLength = fm.boundingRect(a.label).width() * getStride();

        // Check if:
        //  (1) End of annotation (might be maximum, or end of label text) is still visible in time
        //  (2) Part of the annotation is already visible in time
        //
        // Currently there is no check if the annotation is visible in frequency. This is a
        // possible performance improvement
        //
        size_t start = a.sampleRange.minimum;
        size_t end = std::max(a.sampleRange.minimum + labelLength, a.sampleRange.maximum);

        if(start <= sampleRange.maximum && end >= sampleRange.minimum) {

            double frequency = a.frequencyRange.maximum - inputSource->getFrequency();
            int x = (a.sampleRange.minimum - sampleRange.minimum) / getStride();
            int y = zero - frequency / sampleRate * rect.height();
            int height = (a.frequencyRange.maximum - a.frequencyRange.minimum) / sampleRate * rect.height();
            int width = (a.sampleRange.maximum - a.sampleRange.minimum) / getStride();

            if (sigmfAnnotationColors) {
                painter.setPen(a.boxColor);
            }
            if (sigmfAnnotationLabels) {
                // Draw the label 2 pixels above the box
                painter.drawText(x, y - 2, a.label);
            }
            painter.drawRect(x, y, width, height);

            visibleAnnotationLocations.emplace_back(a, x, y, width, height);
        }
    }

    painter.restore();
}

QString *SpectrogramPlot::mouseAnnotationComment(const QMouseEvent *event) {
    auto pos = event->pos();
    int mouse_x = pos.x();
    int mouse_y = pos.y();

    for (auto& a : visibleAnnotationLocations) {
        if (!a.annotation.comment.isEmpty() && a.isInside(mouse_x, mouse_y)) {
            return &a.annotation.comment;
        }
    }
    return nullptr;
}

void SpectrogramPlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    if (!inputSource || inputSource->count() == 0)
        return;

    size_t sampleOffset = sampleRange.minimum % (getStride() * linesPerTile());
    size_t tileID = sampleRange.minimum - sampleOffset;
    int xoffset = sampleOffset / getStride();

    // Reassigned mode: collect all visible tile IDs, find the cache misses,
    // and compute them in parallel before the main paint loop. The paint
    // loop itself is unchanged — getPixmapTile() will hit the cache.
    if (mode == SpectrogramMode::Reassigned) {
        std::vector<size_t> visible;
        size_t walk = tileID;
        visible.push_back(walk);
        walk += getStride() * linesPerTile();
        for (int x = linesPerTile() - xoffset; x < rect.right(); x += linesPerTile()) {
            visible.push_back(walk);
            walk += getStride() * linesPerTile();
        }
        std::vector<size_t> missing;
        for (size_t t : visible) {
            TileCacheKey key(fftSize, zoomLevel, nfftSkip, t, mode,
                             reassignmentFloorDb, windowType, splatMethod);
            // Skip tiles whose pixmap is already cached — they don't need a
            // float-tile recompute either. fftCache misses still trigger
            // pre-warm so the pixmap step is just colormap conversion.
            if (pixmapCache.contains(key)) continue;
            if (fftCache.contains(key)) continue;
            missing.push_back(t);
        }
        prewarmReassignedTiles(missing);
    }

    // Paint first (possibly partial) tile
    painter.drawPixmap(QRect(rect.left(), rect.y(), linesPerTile() - xoffset, height()), *getPixmapTile(tileID), QRect(xoffset, 0, linesPerTile() - xoffset, height()));
    tileID += getStride() * linesPerTile();

    // Paint remaining tiles
    for (int x = linesPerTile() - xoffset; x < rect.right(); x += linesPerTile()) {
        // TODO: don't draw past rect.right()
        // TODO: handle partial final tile
        painter.drawPixmap(QRect(x, rect.y(), linesPerTile(), height()), *getPixmapTile(tileID), QRect(0, 0, linesPerTile(), height()));
        tileID += getStride() * linesPerTile();
    }
}

QPixmap* SpectrogramPlot::getPixmapTile(size_t tile)
{
    TileCacheKey key(fftSize, zoomLevel, nfftSkip, tile, mode,
                     reassignmentFloorDb, windowType, splatMethod);
    QPixmap *obj = pixmapCache.object(key);
    if (obj != 0)
        return obj;

    LatencyLog::markf("specgm getPixmapTile MISS tile=%zu (FFT compute+colormap)", tile);
    float *fftTile = getFFTTile(tile);
    obj = new QPixmap(linesPerTile(), fftSize);
    QImage image(linesPerTile(), fftSize, QImage::Format_RGB32);
    float powerRange = -1.0f / std::abs(int(powerMin - powerMax));
    for (int y = 0; y < fftSize; y++) {
        auto scanLine = (QRgb*)image.scanLine(fftSize - y - 1);
        for (int x = 0; x < linesPerTile(); x++) {
            float *fftLine = &fftTile[x * fftSize];
            float normPower = (fftLine[y] - powerMax) * powerRange;
            normPower = clamp(normPower, 0.0f, 1.0f);

            scanLine[x] = colormap[(uint8_t)(normPower * (256 - 1))];
        }
    }
    obj->convertFromImage(image);
    pixmapCache.insert(key, obj);
    LatencyLog::markf("specgm getPixmapTile DONE tile=%zu", tile);
    return obj;
}

float* SpectrogramPlot::getFFTTile(size_t tile)
{
    TileCacheKey key(fftSize, zoomLevel, nfftSkip, tile, mode,
                     reassignmentFloorDb, windowType, splatMethod);
    std::array<float, tileSize>* obj = fftCache.object(key);
    if (obj != nullptr)
        return obj->data();

    std::array<float, tileSize>* destStorage = new std::array<float, tileSize>;
    if (mode == SpectrogramMode::Reassigned) {
        // Reassignment can move energy across frame boundaries within a
        // tile, so we have to compute the whole tile in one pass instead of
        // line-by-line. We borrow a work set from the pool — the same pool
        // workers use for parallel pre-warm in paintMid.
        auto set = acquireWorkSet();
        computeReassignedTile(destStorage->data(), tile, *set);
        releaseWorkSet(std::move(set));
    } else {
        float *ptr = destStorage->data();
        size_t sample = tile;
        while ((ptr - destStorage->data()) < tileSize) {
            getLine(ptr, sample);
            sample += getStride();
            ptr += fftSize;
        }
    }
    fftCache.insert(key, destStorage);
    return destStorage->data();
}

void SpectrogramPlot::rebuildWindows()
{
    // Three windows used by the reassignment path:
    //   window[]              h(n)
    //   windowTimeWeighted[]  t·h(n)  with centred t = n - (N-1)/2 so the
    //                         formula gives a sample offset from frame centre
    //   windowDerivative[]    h'(n) (closed form per window family)
    // Standard mode only reads window[]; the other two are kept allocated so
    // mode toggles don't have to (re)allocate.
    const int N = fftSize;
    const float tCentre = (N - 1) * 0.5f;
    if (windowType == WindowType::Gaussian) {
        // σ = 0.15·N gives a window that decays to ≈e^-22 at the endpoints
        // and time-frequency localisation close to optimal for typical N.
        // Gaussian is its own Fourier eigenfunction, so the reassignment
        // formula is "exact" in the continuous limit and gives sharper
        // ridges than Hann at no runtime cost.
        const float sigma = 0.15f * N;
        const float invSigma2 = 1.0f / (sigma * sigma);
        for (int i = 0; i < N; i++) {
            float t = i - tCentre;
            float h = std::exp(-0.5f * t * t * invSigma2);
            window[i] = h;
            windowTimeWeighted[i] = t * h;
            // h'(n) = -t/σ² · h(n)
            windowDerivative[i] = -t * invSigma2 * h;
        }
    } else {
        // Hann h(n) = 0.5·(1 - cos(2π n/(N-1)))
        // h'(n) = π/(N-1) · sin(2π n/(N-1))
        const float hannDerivCoeff = static_cast<float>(M_PI) / (N - 1);
        for (int i = 0; i < N; i++) {
            float phase = Tau * i / (N - 1);
            float h = 0.5f * (1.0f - cos(phase));
            window[i] = h;
            windowTimeWeighted[i] = (i - tCentre) * h;
            windowDerivative[i] = hannDerivCoeff * sin(phase);
        }
    }
}

std::unique_ptr<FftWorkSet> SpectrogramPlot::acquireWorkSet()
{
    QMutexLocker lock(&contextPoolMutex_);
    if (!contextPool_.empty()) {
        auto set = std::move(contextPool_.back());
        contextPool_.pop_back();
        return set;
    }
    // Pool empty: build one under the same lock so concurrent acquireWorkSet
    // calls (across worker threads) don't race FFTW planning. FFTW's wisdom
    // cache makes second-and-onward MEASURE plans for the same size fast.
    auto set = std::make_unique<FftWorkSet>();
    set->fftH.reset(new FFT(fftSize));
    set->fftTH.reset(new FFT(fftSize));
    set->fftDH.reset(new FFT(fftSize));
    set->bufH.resize(fftSize);
    set->bufTH.resize(fftSize);
    set->bufDH.resize(fftSize);
    set->outH.resize(fftSize);
    set->outTH.resize(fftSize);
    set->outDH.resize(fftSize);
    set->size = fftSize;
    return set;
}

void SpectrogramPlot::releaseWorkSet(std::unique_ptr<FftWorkSet> set)
{
    if (!set) return;
    QMutexLocker lock(&contextPoolMutex_);
    if (set->size != fftSize) {
        // Stale (FFT size changed while this set was checked out) — drop it.
        return;
    }
    contextPool_.push_back(std::move(set));
}

void SpectrogramPlot::ensureWorkSetPool(int target)
{
    QMutexLocker lock(&contextPoolMutex_);
    while (static_cast<int>(contextPool_.size()) < target) {
        auto set = std::make_unique<FftWorkSet>();
        set->fftH.reset(new FFT(fftSize));
        set->fftTH.reset(new FFT(fftSize));
        set->fftDH.reset(new FFT(fftSize));
        set->bufH.resize(fftSize);
        set->bufTH.resize(fftSize);
        set->bufDH.resize(fftSize);
        set->outH.resize(fftSize);
        set->outTH.resize(fftSize);
        set->outDH.resize(fftSize);
        set->size = fftSize;
        contextPool_.push_back(std::move(set));
    }
}

void SpectrogramPlot::invalidateWorkSetPool()
{
    QMutexLocker lock(&contextPoolMutex_);
    contextPool_.clear();
}

void SpectrogramPlot::prewarmReassignedTiles(const std::vector<size_t> &tiles)
{
    if (tiles.empty() || mode != SpectrogramMode::Reassigned) return;

    int maxThreads = QThreadPool::globalInstance()->maxThreadCount();
    if (maxThreads < 1) maxThreads = 1;
    int parallelism = std::min(static_cast<int>(tiles.size()), maxThreads);
    if (parallelism <= 1) {
        // No win from dispatch — fall through to the synchronous path that
        // getPixmapTile already takes.
        return;
    }

    // Pre-build the pool on the GUI thread so workers never need to plan.
    ensureWorkSetPool(parallelism);

    struct TileResult {
        size_t tile = 0;
        std::array<float, tileSize>* data = nullptr;
    };

    QFutureSynchronizer<TileResult> sync;
    for (size_t tileID : tiles) {
        sync.addFuture(QtConcurrent::run([this, tileID]() -> TileResult {
            TileResult r;
            r.tile = tileID;
            auto storage = std::unique_ptr<std::array<float, tileSize>>(
                new std::array<float, tileSize>);
            auto set = acquireWorkSet();
            computeReassignedTile(storage->data(), tileID, *set);
            releaseWorkSet(std::move(set));
            r.data = storage.release();
            return r;
        }));
    }
    sync.waitForFinished();

    // Insert all results into fftCache on the GUI thread (QCache isn't
    // thread-safe). The cache takes ownership of the raw pointer.
    for (const auto &fut : sync.futures()) {
        auto r = fut.result();
        if (!r.data) continue;
        TileCacheKey key(fftSize, zoomLevel, nfftSkip, r.tile, mode,
                         reassignmentFloorDb, windowType, splatMethod);
        fftCache.insert(key, r.data);
    }
}

void SpectrogramPlot::computeReassignedTile(float *dest, size_t tile, FftWorkSet &set)
{
    // Fulop-Fitz reassignment, JASA 2006:
    //   X_h  : STFT with analysis window h(n)
    //   X_th : STFT with time-weighted window t·h(n)
    //   X_dh : STFT with derivative window h'(n)
    //   t̂ = t - Re{X_th · conj(X_h) / |X_h|²} = t - Re{X_th / X_h}
    //   ω̂ = ω + Im{X_dh · conj(X_h) / |X_h|²} = ω + Im{X_dh / X_h}
    // |X_h|² is then accumulated at (t̂, ω̂) instead of (t, ω). Bins below
    // the noise floor get rendered at their original location so the noise
    // background stays contextual but isn't smeared into speckle.
    //
    // FFT plans + scratch buffers come from `set` so multiple tiles can be
    // computed in parallel (FFTW execute is thread-safe; planning isn't,
    // and is done up front by ensureWorkSetPool).
    const int N = fftSize;
    const int cols = linesPerTile();
    const int stride = getStride();
    const float invN = 1.0f / N;
    const float floorPower = std::pow(10.0f, reassignmentFloorDb / 10.0f);
    const float halfShift = static_cast<float>(N >> 1);

    // accum is indexed as accum[col * N + bin] to match the tile layout
    // that getPixmapTile() reads. assign() resizes + zero-inits in one step.
    set.accum.assign(static_cast<size_t>(cols) * N, 0.0f);
    auto &bufH  = set.bufH;
    auto &bufTH = set.bufTH;
    auto &bufDH = set.bufDH;
    auto &outH  = set.outH;
    auto &outTH = set.outTH;
    auto &outDH = set.outDH;
    auto &accum = set.accum;

    auto splat = [&](int col, int bin, float power) {
        if (col < 0 || col >= cols) return;
        // Frequency bins wrap around (periodic) so bin offsets that fall
        // off either end land on the opposite side. Time bins don't wrap
        // — energy that lands outside the tile is dropped (acceptable
        // edge artefact, bounded by ~window/2 samples).
        bin = ((bin % N) + N) % N;
        accum[static_cast<size_t>(col) * N + bin] += power;
    };

    for (int c = 0; c < cols; c++) {
        size_t sample = tile + static_cast<size_t>(c) * stride;
        const auto first_sample = std::max(static_cast<ssize_t>(sample) - N / 2,
                                           static_cast<ssize_t>(0));
        auto buffer = inputSource->getSamples(first_sample, N);
        if (buffer == nullptr) continue;

        for (int i = 0; i < N; i++) {
            auto s = buffer[i];
            bufH[i]  = s * window[i];
            bufTH[i] = s * windowTimeWeighted[i];
            bufDH[i] = s * windowDerivative[i];
        }

        set.fftH->process(outH.data(), bufH.data());
        set.fftTH->process(outTH.data(), bufTH.data());
        set.fftDH->process(outDH.data(), bufDH.data());

        for (int k = 0; k < N; k++) {
            auto Xh = outH[k];
            float magH2 = Xh.real() * Xh.real() + Xh.imag() * Xh.imag();
            float power = magH2 * invN * invN;
            // Match the Standard-mode FFT shift: bin k in FFTW order maps
            // to display bin k XOR (N/2) so DC sits at the centre row.
            int displayBin = k ^ (N >> 1);

            if (power < floorPower || magH2 == 0.0f) {
                // Below noise floor — keep at original (t, ω). Zero magH2
                // is the degenerate divide-by-zero guard.
                splat(c, displayBin, power);
                continue;
            }

            // Reciprocal of X_h: ratio_t = X_th / X_h, ratio_d = X_dh / X_h.
            float invMag2 = 1.0f / magH2;
            std::complex<float> conjXh(Xh.real(), -Xh.imag());
            std::complex<float> ratioT = outTH[k] * conjXh * invMag2;
            std::complex<float> ratioD = outDH[k] * conjXh * invMag2;
            float tOffsetSamples = -ratioT.real();
            float wOffsetRad = ratioD.imag();

            // Convert to (column, bin) offsets. stride samples per column
            // and 2π/N radians per bin.
            float dCol = tOffsetSamples / static_cast<float>(stride);
            float dBin = wOffsetRad * N / static_cast<float>(Tau);

            // FFT-shift offset is added in continuous form so fractional
            // splats wrap correctly across the centre.
            float colHat = static_cast<float>(c) + dCol;
            float binHat = static_cast<float>(k) + dBin + halfShift;

            if (splatMethod == SplatMethod::Nearest) {
                // ~4× cheaper than bilinear; visually fine for tonal/chirp
                // signals, slightly more aliased on weak ridges.
                int colN = static_cast<int>(std::lround(colHat));
                int binN = static_cast<int>(std::lround(binHat));
                splat(colN, binN, power);
            } else {
                // Bilinear splat across the 4 nearest pixels.
                int c0 = static_cast<int>(std::floor(colHat));
                float fc = colHat - c0;
                int b0 = static_cast<int>(std::floor(binHat));
                float fb = binHat - b0;
                splat(c0,     b0,     power * (1.0f - fc) * (1.0f - fb));
                splat(c0 + 1, b0,     power *         fc  * (1.0f - fb));
                splat(c0,     b0 + 1, power * (1.0f - fc) *         fb );
                splat(c0 + 1, b0 + 1, power *         fc  *         fb );
            }
        }
    }

    // Linear power → dB, matching getLine()'s scaling so the colour bar
    // reading is consistent across modes.
    const float logMultiplier = 10.0f / log2f(10.0f);
    const float negInf = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < static_cast<size_t>(cols) * N; i++) {
        float p = accum[i];
        dest[i] = (p > 0.0f) ? log2f(p) * logMultiplier : negInf;
    }
}

void SpectrogramPlot::getLine(float *dest, size_t sample)
{
    if (inputSource && fft) {
        // Make sample be the midpoint of the FFT, unless this takes us
        // past the beginning of the inputSource (if we remove the
        // std::max(·, 0), then an ugly red bar appears at the beginning
        // of the spectrogram with large zooms and FFT sizes).
        const auto first_sample = std::max(static_cast<ssize_t>(sample) - fftSize / 2,
                        static_cast<ssize_t>(0));
        auto buffer = inputSource->getSamples(first_sample, fftSize);
        if (buffer == nullptr) {
            auto neg_infinity = -1 * std::numeric_limits<float>::infinity();
            for (int i = 0; i < fftSize; i++, dest++)
                *dest = neg_infinity;
            return;
        }

        for (int i = 0; i < fftSize; i++) {
            buffer[i] *= window[i];
        }

        fft->process(buffer.get(), buffer.get());
        const float invFFTSize = 1.0f / fftSize;
        const float logMultiplier = 10.0f / log2f(10.0f);
        for (int i = 0; i < fftSize; i++) {
            // Start from the middle of the FFTW array and wrap
            // to rearrange the data
            int k = i ^ (fftSize >> 1);
            auto s = buffer[k] * invFFTSize;
            float power = s.real() * s.real() + s.imag() * s.imag();
            float logPower = log2f(power) * logMultiplier;
            *dest = logPower;
            dest++;
        }
    }
}

int SpectrogramPlot::getStride()
{
    return fftSize * nfftSkip / zoomLevel;
}

float SpectrogramPlot::getTunerPhaseInc()
{
    auto freq = 0.5f - tuner.centre() / (float)fftSize;
    return freq * Tau;
}

std::vector<float> SpectrogramPlot::getTunerTaps()
{
    float cutoff = tuner.deviation() / (float)fftSize;
    float gain = pow(10.0f, powerMax / -10.0f);
    auto atten = 60.0f;
    auto len = estimate_req_filter_len(std::min(cutoff, 0.05f), atten);
    auto taps = std::vector<float>(len);
    liquid_firdes_kaiser(len, cutoff, atten, 0.0f, taps.data());
    std::transform(taps.begin(), taps.end(), taps.begin(),
                   std::bind(std::multiplies<float>(), std::placeholders::_1, gain));
    return taps;
}

int SpectrogramPlot::linesPerTile()
{
    return tileSize / fftSize;
}

bool SpectrogramPlot::mouseEvent(QEvent::Type type, QMouseEvent event)
{
    if (tunerEnabled())
        return tuner.mouseEvent(type, event);

    return false;
}

std::shared_ptr<AbstractSampleSource> SpectrogramPlot::output()
{
    return tunerTransform;
}

void SpectrogramPlot::setFFTSize(int size)
{
    float sizeScale = float(size) / float(fftSize);
    fftSize = size;
    fft.reset(new FFT(fftSize));
    // FFTW plans in the worker pool are size-specific — drop them so they
    // get rebuilt at the new size on the next reassigned paint.
    invalidateWorkSetPool();

    window.reset(new float[fftSize]);
    windowTimeWeighted.reset(new float[fftSize]);
    windowDerivative.reset(new float[fftSize]);
    rebuildWindows();

    if (inputSource->realSignal()) {
        setHeight(fftSize/2);
    } else {
        setHeight(fftSize);
    }
    auto dev = tuner.deviation();
    auto centre = tuner.centre();
    tuner.setHeight(height());
    tuner.setDeviation( dev * sizeScale );
    tuner.setCentre( centre * sizeScale );
}

void SpectrogramPlot::setPowerMax(int power)
{
    powerMax = power;
    pixmapCache.clear();
    tunerMoved();
}

void SpectrogramPlot::setPowerMin(int power)
{
    powerMin = power;
    pixmapCache.clear();
}

void SpectrogramPlot::setZoomLevel(int zoom)
{
    zoomLevel = zoom;
}

void SpectrogramPlot::setSkip(int skip)
{
    nfftSkip = skip;
}

void SpectrogramPlot::setSampleRate(double rate)
{
    sampleRate = rate;
}

void SpectrogramPlot::setSpectrogramMode(int newMode)
{
    SpectrogramMode m = (newMode == static_cast<int>(SpectrogramMode::Reassigned))
                            ? SpectrogramMode::Reassigned
                            : SpectrogramMode::Standard;
    if (m == mode) return;
    mode = m;
    // Cache keys include the mode, so old tiles will sit unused; clear
    // them to free the budget for the new render path.
    pixmapCache.clear();
    fftCache.clear();
    emit repaint();
}

void SpectrogramPlot::setReassignmentFloor(int floorDb)
{
    if (floorDb == reassignmentFloorDb) return;
    reassignmentFloorDb = floorDb;
    if (mode != SpectrogramMode::Reassigned) return;
    pixmapCache.clear();
    fftCache.clear();
    emit repaint();
}

void SpectrogramPlot::setWindowType(int wt)
{
    WindowType w = (wt == static_cast<int>(WindowType::Gaussian))
                       ? WindowType::Gaussian
                       : WindowType::Hann;
    if (w == windowType) return;
    windowType = w;
    rebuildWindows();
    if (mode != SpectrogramMode::Reassigned) return;
    pixmapCache.clear();
    fftCache.clear();
    emit repaint();
}

void SpectrogramPlot::setSplatMethod(int sm)
{
    SplatMethod s = (sm == static_cast<int>(SplatMethod::Nearest))
                        ? SplatMethod::Nearest
                        : SplatMethod::Bilinear;
    if (s == splatMethod) return;
    splatMethod = s;
    if (mode != SpectrogramMode::Reassigned) return;
    pixmapCache.clear();
    fftCache.clear();
    emit repaint();
}

void SpectrogramPlot::enableScales(bool enabled)
{
   frequencyScaleEnabled = enabled;
}

void SpectrogramPlot::enableAnnotations(bool enabled)
{
   sigmfAnnotationsEnabled = enabled;
}

bool SpectrogramPlot::isAnnotationsEnabled(void)
{
    return sigmfAnnotationsEnabled;
}

void SpectrogramPlot::enableAnnoLabels(bool enabled)
{
    sigmfAnnotationLabels = enabled;
}

void SpectrogramPlot::enableAnnoColors(bool enabled)
{
    sigmfAnnotationColors = enabled;
}

bool SpectrogramPlot::tunerEnabled()
{
    return (tunerTransform->subscriberCount() > 0);
}

void SpectrogramPlot::setTunerCentreY(int y)
{
    // Clamp into the plot so the cursors stay visible. The tuner uses
    // [0..height()] in plot pixels; freq mapping is in getTunerPhaseInc().
    if (y < 0) y = 0;
    if (y > height()) y = height();
    tuner.setCentre(y);
    tunerMoved();
}

void SpectrogramPlot::tunerMoved()
{
    LatencyLog::mark("tunerMoved start");
    const float newFreq = getTunerPhaseInc();
    tunerTransform->setFrequency(newFreq);

    // Tap design is the dominant per-event cost during a tuner drag
    // (liquid_firdes_kaiser scales with filter length, which gets large at
    // narrow cutoffs). The taps only depend on deviation/fftSize/powerMax —
    // pure centre-frequency moves don't touch any of those, so the same
    // taps are valid and we can skip the redesign + setTaps mutex round.
    const int   dev   = tuner.deviation();
    const int   fft_  = fftSize;
    const float pmax_ = powerMax;
    bool tapsRebuilt = false;
    if (dev != lastTapsDeviation_ || fft_ != lastTapsFftSize_ ||
        pmax_ != lastTapsPowerMax_) {
        tunerTransform->setTaps(getTunerTaps());
        lastTapsDeviation_ = dev;
        lastTapsFftSize_   = fft_;
        lastTapsPowerMax_  = pmax_;
        tapsRebuilt = true;
    }

    tunerTransform->setRelativeBandwith(tuner.deviation() * 2.0 / height());

    // Skip the invalidate fan-out when nothing the downstream chain cares
    // about actually moved. The Tuner emits `tunerMoved` on every mouse
    // event during drag and on release; many of those events leave
    // (frequency, deviation) unchanged (mouse release in the same pixel,
    // duplicate moves while still being processed). Without this guard
    // every spurious emit triggers a full demod re-render and burns a
    // worker cycle.
    const bool notifyNeeded = tapsRebuilt ||
                              newFreq != lastNotifiedFrequency_ ||
                              dev     != lastNotifiedDeviation_;
    if (notifyNeeded) {
        lastNotifiedFrequency_ = newFreq;
        lastNotifiedDeviation_ = dev;
        tunerTransform->notifyChanged();
    }

    LatencyLog::markf("tunerMoved end (taps_rebuilt=%d notify=%d)",
                      tapsRebuilt ? 1 : 0, notifyNeeded ? 1 : 0);
    emit repaint();
}

uint qHash(const TileCacheKey &key, uint seed)
{
    return key.fftSize ^ key.zoomLevel ^ key.sample ^ seed
           ^ (static_cast<uint>(key.mode) << 24)
           ^ (static_cast<uint>(key.windowType) << 25)
           ^ (static_cast<uint>(key.splatMethod) << 26)
           ^ static_cast<uint>(key.reassignmentFloorDb);
}
