/*
 *  Copyright (C) 2026
 *
 *  This file is part of inspectrum.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "fskpolarplot.h"

#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
// Cap the contiguous IQ window we pull per render. At 15.36 Msps this is ~33 ms
// (~600 TETRA symbols) — plenty of points for a dense constellation without an
// unbounded getSamples through the tuner FIR.
constexpr size_t kMaxSamples = 500000;
constexpr double kMinMag = 1e-8;

static double radiusFor(int w, int h)
{
    const int side = std::max(10, std::min(w, h) - 16);
    return side * 0.47;
}

// Worker-thread render: pull IQ, accumulate differential-phase hits into a
// per-pixel counter, then map counts → a dark-green→white-hot heatmap. Runs
// off the GUI thread; touches only the (kept-alive) source and local buffers.
static QImage renderConstellation(std::shared_ptr<SampleSource<std::complex<float>>> src,
                                  FskPolarPlot::RenderKey k)
{
    QImage img(k.w, k.h, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    if (!src || k.w < 1 || k.h < 1 || k.len <= k.delay + 1)
        return img;

    auto samples = src->getSamples(k.start, k.len);
    if (!samples)
        return img;

    const double radius = radiusFor(k.w, k.h);
    const double cx = k.w / 2.0;
    const double cy = k.h / 2.0;

    // Build the differential points to plot.
    std::vector<std::complex<float>> dv;
    if (k.symbolTimed && k.sps >= 2.0) {
        // Symbol-timed: resample to one sample per symbol at fractional spacing
        // k.sps (linear interp), at the timing phase that maximises symbol
        // energy, then take the 1-symbol differential. This collapses the
        // inter-symbol trajectory "spokes" so only the decision-point clusters
        // remain (e.g. the 4 points of π/4-DQPSK).
        const size_t M = static_cast<size_t>(k.len / k.sps);
        if (M < 4)
            return img;
        auto interp = [&](double pos) -> std::complex<float> {
            if (pos < 0.0) pos = 0.0;
            const size_t i0 = static_cast<size_t>(pos);
            if (i0 + 1 >= k.len) return samples[k.len - 1];
            const float f = static_cast<float>(pos - i0);
            return samples[i0] * (1.0f - f) + samples[i0 + 1] * f;
        };
        // Timing recovery: pick the fractional phase maximising mean symbol
        // energy (symbol centres are the energy peaks for pulse-shaped signals;
        // for constant-envelope FSK any phase is equivalent, so it's a no-op).
        double bestPhase = 0.0, bestScore = -1.0;
        for (int p = 0; p < 16; ++p) {
            const double ph = p / 16.0;
            double e = 0.0; size_t cnt = 0;
            for (size_t s = 0; s < M; s += 4) {
                const double pos = (s + ph) * k.sps;
                if (pos >= k.len) break;
                e += std::norm(interp(pos)); ++cnt;
            }
            if (cnt && e / cnt > bestScore) { bestScore = e / cnt; bestPhase = ph; }
        }
        std::vector<std::complex<float>> sym;
        sym.reserve(M);
        for (size_t s = 0; s < M; ++s) {
            const double pos = (s + bestPhase) * k.sps;
            if (pos >= k.len) break;
            sym.push_back(interp(pos));
        }
        dv.reserve(sym.size());
        for (size_t s = 1; s < sym.size(); ++s)
            dv.push_back(sym[s] * std::conj(sym[s - 1]));
    } else {
        // Full-rate: every sample's 1-symbol-delayed differential (the
        // trajectory, including inter-symbol transitions). k.len is capped at
        // kMaxSamples so this is a bounded (≤500k) loop.
        dv.reserve(k.len - k.delay);
        for (size_t i = k.delay; i < k.len; ++i)
            dv.push_back(samples[i] * std::conj(samples[i - k.delay]));
    }
    if (dv.empty())
        return img;

    // Window reference = peak differential magnitude. In-burst points cluster
    // near it; noise/gaps sit far below. Points are placed at radius |d|/refMag
    // — NOT normalised to the unit circle — so weak residual points fall toward
    // the centre instead of polluting the cluster ring.
    double refMag = 0.0;
    for (const auto &d : dv) {
        const double m = std::abs(d);
        if (std::isfinite(m) && m > refMag)
            refMag = m;
    }
    if (refMag < kMinMag)
        return img;
    const double gate = std::max(refMag * (k.gatePct / 100.0), kMinMag);
    const double rscale = radius / refMag;

    std::vector<uint32_t> hits(static_cast<size_t>(k.w) * k.h, 0);
    uint32_t maxHit = 0;
    for (const auto &d : dv) {
        const double mag = std::abs(d);
        if (!std::isfinite(mag) || mag < gate)
            continue;
        const int px = static_cast<int>(std::lround(cx + d.real() * rscale));
        const int py = static_cast<int>(std::lround(cy - d.imag() * rscale));
        if (px < 0 || px >= k.w || py < 0 || py >= k.h)
            continue;
        const uint32_t c = ++hits[static_cast<size_t>(py) * k.w + px];
        if (c > maxHit)
            maxHit = c;
    }
    if (maxHit == 0)
        return img;

    // Log scaling so a few hot symbol centres don't wash out the rest.
    const double lmax = std::log1p(static_cast<double>(maxHit));
    for (int y = 0; y < k.h; ++y) {
        QRgb *line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < k.w; ++x) {
            const uint32_t c = hits[static_cast<size_t>(y) * k.w + x];
            if (!c)
                continue;
            const double t = std::log1p(static_cast<double>(c)) / lmax; // 0..1
            const int r = static_cast<int>(255.0 * std::pow(t, 1.6));
            const int g = static_cast<int>(70.0 + 185.0 * t);
            const int b = static_cast<int>(255.0 * std::pow(t, 2.4));
            line[x] = qRgba(r, g, b, 255);
        }
    }
    return img;
}
} // namespace

FskPolarPlot::FskPolarPlot(std::shared_ptr<SampleSource<std::complex<float>>> source)
    : Plot(source), iqSource(std::move(source))
{
}

void FskPolarPlot::setSymbolRate(double baud)
{
    symbolRateHz = baud > 0.0 ? baud : 0.0;
    emit repaint();
}

void FskPolarPlot::setLevelGate(int pct)
{
    levelGatePct = std::max(0, std::min(100, pct));
    emit repaint();
}

void FskPolarPlot::setSymbolTimed(bool on)
{
    symbolTimed_ = on;
    emit repaint();
}

void FskPolarPlot::invalidateEvent()
{
    // Bump the epoch so the next paint's RenderKey differs from the cached
    // image and forces a re-render against the new upstream data.
    ++dataEpoch_;
    emit repaint();
}

void FskPolarPlot::setSelection(bool enabled, range_t<size_t> sampleRange)
{
    selectionEnabled = enabled;
    selectedRange = sampleRange;
    emit repaint();
}

size_t FskPolarPlot::delayForRate() const
{
    const double rate = iqSource ? iqSource->rate() : 0.0;
    if (symbolRateHz > 0.0 && rate > 0.0) {
        const double d = rate / symbolRateHz; // samples per symbol
        const size_t want = static_cast<size_t>(d + 0.5);
        // The differential needs both s[i] and s[i-delay] inside the render
        // window. If one symbol exceeds the window the constellation can't be
        // formed — return 0 so the caller hints, rather than silently clamping
        // to a wrong delay that would still draw a (meaningless) ring while the
        // overlay claimed it was one symbol.
        if (want < 1 || want + 1 >= kMaxSamples)
            return 0;
        return want; // exact round(Fs/baud) — the overlay's "1 symbol" holds
    }
    return 0; // no symbol rate → caller draws a hint instead of a smear
}

void FskPolarPlot::paintBack(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    Q_UNUSED(sampleRange);

    painter.save();
    painter.fillRect(rect, Qt::black);

    const QPoint c(rect.left() + rect.width() / 2, rect.top() + rect.height() / 2);
    const int r = static_cast<int>(radiusFor(rect.width(), rect.height()));

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(80, 80, 80), 1));
    painter.drawEllipse(c, r, r);
    painter.drawLine(c.x() - r, c.y(), c.x() + r, c.y());
    painter.drawLine(c.x(), c.y() - r, c.x(), c.y() + r);

    // Faint 45° orientation ticks — modulation-agnostic, but π/4-DQPSK clusters
    // land on the four diagonals so they double as a read-off guide.
    painter.setPen(QPen(QColor(60, 60, 60), 1));
    for (int k = 0; k < 8; ++k) {
        const double a = k * M_PI / 4.0;
        const double inner = r * 0.88;
        painter.drawLine(QPointF(c.x() + inner * std::cos(a), c.y() - inner * std::sin(a)),
                         QPointF(c.x() + r * std::cos(a), c.y() - r * std::sin(a)));
    }

    painter.setPen(QColor(180, 180, 180));
    painter.drawText(rect.left() + 6, rect.top() + painter.fontMetrics().ascent() + 4,
                     QStringLiteral("FSK/PSK differential phase"));
    painter.restore();
}

void FskPolarPlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    if (selectionEnabled && selectedRange.maximum > selectedRange.minimum)
        sampleRange = selectedRange;

    const size_t delay = delayForRate();

    // Always-visible status line just below the title, on a dark pill so it
    // reads over the constellation — lets the symbol rate / delay be verified
    // at a glance (a too-small delay collapses the scope to one blob).
    auto drawStatus = [&](const QString &text) {
        painter.save();
        const QFontMetrics fmx(painter.font());
        const int pad = 3;
        const int x = rect.left() + 6;
        const int baseline = rect.top() + 6 + fmx.height() + fmx.ascent();
        painter.fillRect(x - pad, baseline - fmx.ascent() - pad,
                         fmx.width(text) + 2 * pad, fmx.height() + 2 * pad,
                         QColor(0, 0, 0, 180));
        painter.setPen(QColor(230, 230, 230));
        painter.drawText(x, baseline, text);
        painter.restore();
    };

    if (!iqSource || sampleRange.maximum <= sampleRange.minimum) {
        drawStatus(QStringLiteral("no data"));
        return;
    }

    if (delay == 0) {
        drawStatus(symbolRateHz > 0.0
            ? QStringLiteral("symbol rate %1 Bd — period too long for window")
                  .arg(symbolRateHz, 0, 'f', 0)
            : QStringLiteral("symbol rate: not set"));
        painter.save();
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(rect, Qt::AlignCenter,
            symbolRateHz > 0.0
                ? QStringLiteral("symbol period too long\n(decimate or raise the rate)")
                : QStringLiteral("set Symbol rate (Bd)\nto view the constellation"));
        painter.restore();
        return;
    }

    // delay = round(Fs/baud); show it (and the baud) so a wrong setting — e.g. a
    // too-small delay that collapses the scope to one blob — is obvious.
    const double sps = iqSource->rate() / symbolRateHz; // exact (fractional)
    const bool symbolTimed = symbolTimed_ && sps >= 2.0;
    const QString scope = selectionEnabled ? QStringLiteral("sel") : QStringLiteral("view");
    drawStatus(QStringLiteral("%1 · %2 Bd · delay %3 samp · gate %4%%5")
                   .arg(scope)
                   .arg(symbolRateHz, 0, 'f', symbolRateHz < 1000.0 ? 1 : 0)
                   .arg(delay)
                   .arg(levelGatePct)
                   .arg(symbolTimed ? QStringLiteral(" · sym-timed") : QString()));

    size_t len = sampleRange.maximum - sampleRange.minimum;
    if (len <= delay + 1)
        return;
    size_t start = sampleRange.minimum;
    if (len > kMaxSamples) {
        start += (len - kMaxSamples) / 2; // centre window of a long selection
        len = kMaxSamples;
    }

    const int w = rect.width();
    const int h = rect.height();
    if (w < 1 || h < 1)
        return;

    const RenderKey key{start, len, delay, w, h, levelGatePct, dataEpoch_, symbolTimed, sps};
    pendingKey_ = key;
    havePending_ = true;

    if (hasImage_ && imageKey_ == key)
        painter.drawImage(rect.topLeft(), image_);

    const bool needRender = !hasImage_ || imageKey_ != key;
    if (needRender && !running_)
        startRender(key);
}

void FskPolarPlot::startRender(const RenderKey &k)
{
    if (!watcher_) {
        watcher_ = new QFutureWatcher<QImage>(this);
        // `this` as context: the lambda won't fire if the plot is destroyed.
        connect(watcher_, &QFutureWatcher<QImage>::finished, this,
                [this]() { onRenderReady(); });
    }
    running_ = true;
    runningKey_ = k;
    // Capture the source shared_ptr by value so the data outlives the plot if
    // it's removed mid-render; the result is simply discarded in that case.
    auto src = iqSource;
    watcher_->setFuture(QtConcurrent::run(
        [src, k]() { return renderConstellation(src, k); }));
}

void FskPolarPlot::onRenderReady()
{
    image_ = watcher_->result();
    imageKey_ = runningKey_;
    hasImage_ = true;
    running_ = false;
    // View moved on while rendering → chase the latest key.
    if (havePending_ && pendingKey_ != imageKey_)
        startRender(pendingKey_);
    emit repaint();
}
