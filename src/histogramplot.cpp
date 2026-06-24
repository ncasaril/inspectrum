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

#include "histogramplot.h"

#include "util.h"

#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {
// Cap the contiguous window pulled per render (the float source can fan out to
// the batched FM demod). A histogram only needs a representative sample.
constexpr size_t kMaxSamples = 1000000;

static QString fmtValue(double v)
{
    return QString::fromStdString(formatSIValue(v));
}

static QImage renderHistogram(std::shared_ptr<SampleSource<float>> src,
                              HistogramPlot::RenderKey k)
{
    QImage img(k.w, k.h, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    if (!src || k.w < 2 || k.h < 2 || k.len == 0)
        return img;

    auto samples = src->getSamples(k.start, k.len);
    if (!samples)
        return img;

    // Auto-range over the finite samples.
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < k.len; ++i) {
        const double v = samples[i];
        if (!std::isfinite(v))
            continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo)
        return img;
    const double pad = (hi - lo) * 0.05;
    lo -= pad;
    hi += pad;

    const int nbins = std::min(256, std::max(32, k.w / 4));
    std::vector<uint32_t> bins(nbins, 0);
    uint32_t maxBin = 0;
    const double span = hi - lo;
    for (size_t i = 0; i < k.len; ++i) {
        const double v = samples[i];
        if (!std::isfinite(v))
            continue;
        int b = static_cast<int>((v - lo) / span * nbins);
        if (b < 0) b = 0;
        if (b >= nbins) b = nbins - 1;
        const uint32_t c = ++bins[b];
        if (c > maxBin)
            maxBin = c;
    }
    if (maxBin == 0)
        return img;

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);

    // Bars: value on X (lo→hi left→right), count as height. sqrt scaling lifts
    // small lobes so a weak fourth FSK level still reads.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(40, 210, 90));
    const int baseY = k.h - 1;
    for (int b = 0; b < nbins; ++b) {
        if (!bins[b])
            continue;
        const double frac = static_cast<double>(bins[b]) / maxBin;
        const int barH = static_cast<int>(std::sqrt(frac) * (k.h - 14));
        const int x0 = static_cast<int>(static_cast<double>(b) / nbins * k.w);
        int x1 = static_cast<int>(static_cast<double>(b + 1) / nbins * k.w);
        if (x1 <= x0)
            x1 = x0 + 1;
        p.drawRect(x0, baseY - barH, x1 - x0 - (x1 - x0 > 1 ? 1 : 0), barH);
    }

    // A zero reference line, if zero is in range (FSK levels straddle it).
    if (lo < 0.0 && hi > 0.0) {
        const int zx = static_cast<int>((0.0 - lo) / span * k.w);
        p.setPen(QPen(QColor(200, 200, 200, 110), 1, Qt::DashLine));
        p.drawLine(zx, 0, zx, k.h);
    }

    // Value-axis endpoints so the peaks have a scale.
    p.setPen(QColor(210, 210, 210));
    p.drawText(2, k.h - 3, fmtValue(lo));
    const QString hiText = fmtValue(hi);
    p.drawText(k.w - 2 - p.fontMetrics().width(hiText), k.h - 3, hiText);
    return img;
}
} // namespace

HistogramPlot::HistogramPlot(std::shared_ptr<SampleSource<float>> source)
    : Plot(source), floatSource(std::move(source))
{
}

void HistogramPlot::setSelection(bool enabled, range_t<size_t> sampleRange)
{
    selectionEnabled = enabled;
    selectedRange = sampleRange;
    emit repaint();
}

void HistogramPlot::paintBack(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    Q_UNUSED(sampleRange);
    painter.save();
    painter.fillRect(rect, Qt::black);
    painter.setPen(QColor(180, 180, 180));
    painter.drawText(rect.left() + 6, rect.top() + painter.fontMetrics().ascent() + 4,
                     QStringLiteral("value histogram"));
    painter.restore();
}

void HistogramPlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    if (selectionEnabled && selectedRange.maximum > selectedRange.minimum)
        sampleRange = selectedRange;

    if (!floatSource || sampleRange.maximum <= sampleRange.minimum)
        return;

    size_t len = sampleRange.maximum - sampleRange.minimum;
    size_t start = sampleRange.minimum;
    if (len > kMaxSamples) {
        start += (len - kMaxSamples) / 2;
        len = kMaxSamples;
    }

    const int w = rect.width();
    const int h = rect.height();
    if (w < 1 || h < 1)
        return;

    const RenderKey key{start, len, w, h};
    pendingKey_ = key;
    havePending_ = true;

    if (hasImage_ && imageKey_ == key)
        painter.drawImage(rect.topLeft(), image_);

    const bool needRender = !hasImage_ || imageKey_ != key;
    if (needRender && !running_)
        startRender(key);
}

void HistogramPlot::startRender(const RenderKey &k)
{
    if (!watcher_) {
        watcher_ = new QFutureWatcher<QImage>(this);
        connect(watcher_, &QFutureWatcher<QImage>::finished, this,
                [this]() { onRenderReady(); });
    }
    running_ = true;
    runningKey_ = k;
    auto src = floatSource;
    watcher_->setFuture(QtConcurrent::run(
        [src, k]() { return renderHistogram(src, k); }));
}

void HistogramPlot::onRenderReady()
{
    image_ = watcher_->result();
    imageKey_ = runningKey_;
    hasImage_ = true;
    running_ = false;
    if (havePending_ && pendingKey_ != imageKey_)
        startRender(pendingKey_);
    emit repaint();
}
