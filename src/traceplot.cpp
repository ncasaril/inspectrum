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

#include <QPixmapCache>
#include <QTextStream>
#include <QtConcurrent>
#include <QThreadPool>
#include <QPainterPath>
#include <cmath>
#include <limits>
#include <algorithm>
#include "samplesource.h"
#include "traceplot.h"

TracePlot::TracePlot(std::shared_ptr<AbstractSampleSource> source) : Plot(source) {
    connect(this, &TracePlot::imageReady, this, &TracePlot::handleImage);
    // debounce timer: batch up rapid tile requests
    debounceTimer = new QTimer(this);
    debounceTimer->setSingleShot(true);
    debounceTimer->setInterval(50); // ms delay
    connect(debounceTimer, &QTimer::timeout,
            this, &TracePlot::schedulePendingTiles);
    // vertical zoom scale
    yScale = 1.0;
    // initialize min/max background watcher
    minMaxWatcher = new QFutureWatcher<QPair<double,double>>(this);
    connect(minMaxWatcher, &QFutureWatcher<QPair<double,double>>::finished,
            this, &TracePlot::onMinMaxReady);
    firstMinMax = true;
}

bool TracePlot::wheelEvent(QWheelEvent *event)
{
    // Vertical zoom only for single-channel (float) derived plots
    if (dynamic_cast<SampleSource<float>*>(sampleSource.get())) {
        int delta = event->angleDelta().y();
        // Scale factor: ~1.1x per wheel step
        double factor = std::pow(1.1, delta / 120.0);
        yScale *= factor;
        // Clamp scale
        yScale = std::max(0.1, std::min(yScale, 10.0));
        emit repaint();
        return true;
    }
    return false;
}

void TracePlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    // For single-channel (float) derived plots, do centralized draw with global min/max
    if (auto srcF = dynamic_cast<SampleSource<float>*>(sampleSource.get())) {
        size_t start = sampleRange.minimum;
        size_t len = sampleRange.maximum - sampleRange.minimum;
        if (len == 0) return;
        auto samples = srcF->getSamples(start, len);
        if (!samples) return;
        // Schedule background global min/max compute if needed
        if (firstMinMax ||
            sampleRange.minimum != minMaxRange.minimum ||
            (sampleRange.maximum - sampleRange.minimum) !=
                (minMaxRange.maximum - minMaxRange.minimum)) {
            if (!minMaxWatcher->isRunning()) {
                minMaxRange = sampleRange;
                firstMinMax = false;
                auto srcPtr = srcF;
                auto rangeCopy = sampleRange;
                auto future = QtConcurrent::run([srcPtr, rangeCopy]() {
                    size_t start = rangeCopy.minimum;
                    size_t count = rangeCopy.maximum - rangeCopy.minimum;
                    QPair<double,double> result;
                    result.first = std::numeric_limits<double>::infinity();
                    result.second = -std::numeric_limits<double>::infinity();
                    auto data = srcPtr->getSamples(start, count);
                    if (data) {
                        for (size_t i = 0; i < count; ++i) {
                            double v = data[i];
                            result.first = std::min(result.first, v);
                            result.second = std::max(result.second, v);
                        }
                    }
                    return result;
                });
                minMaxWatcher->setFuture(future);
            }
        }
        // Use last-known global min/max
        double minv = globalMin;
        double maxv = globalMax;
        if (maxv <= minv) { maxv = minv + 1.0; }
        double mid = 0.5 * (minv + maxv);
        double invRange = yScale / (maxv - minv);
        // Down-sample to at most one point per pixel
        int w = rect.width();
        int h = height();
        double xStep = double(w) / double(len);
        size_t decim = (len > size_t(w)) ? (len + w - 1) / w : 1;
        QPainterPath path;
        bool first = true;
        for (size_t i = 0; i < len; i += decim) {
            double s = samples[i];
            double norm = (s - mid) * invRange;
            norm = clamp(norm, -1.0, 1.0);
            double x = rect.x() + i * xStep;
            double y = rect.y() + (1.0 - norm) * (h * 0.5);
            if (first) { path.moveTo(x, y); first = false; }
            else       { path.lineTo(x, y); }
        }
        // Ensure last sample
        if (len > 0 && (len - 1) % decim != 0) {
            size_t i = len - 1;
            double s = samples[i];
            double norm = (s - mid) * invRange;
            norm = clamp(norm, -1.0, 1.0);
            double x = rect.x() + w - 1;
            double y = rect.y() + (1.0 - norm) * (h * 0.5);
            path.lineTo(x, y);
        }
        painter.setPen(Qt::green);
        painter.drawPath(path);
        return;
    }
    // Fallback: raw complex or multi-channel trace uses existing threaded pixmap pipeline
    currentFrameKeys.clear();
    size_t totalLen = sampleRange.maximum - sampleRange.minimum;
    if (totalLen == 0) return;
    int samplesPerColumn = std::max(1UL, totalLen / rect.width());
    int threads = QThreadPool::globalInstance()->maxThreadCount();
    if (threads < 1) threads = 1;
    int tilePx = rect.width() / threads;
    if (tilePx < 1) tilePx = 1;
    int samplesPerTile = tilePx * samplesPerColumn;
    size_t tileID = sampleRange.minimum / samplesPerTile;
    size_t tileOffset = sampleRange.minimum % samplesPerTile;
    int xOffset = tileOffset / samplesPerColumn;
    // Paint first (possibly partial) tile
    painter.drawPixmap(
        QRect(rect.x(), rect.y(), tilePx - xOffset, height()),
        getTile(tileID++, samplesPerTile, tilePx),
        QRect(xOffset, 0, tilePx - xOffset, height())
    );
    // Paint remaining tiles
    for (int x = tilePx - xOffset; x < rect.right(); x += tilePx) {
        painter.drawPixmap(
            QRect(x, rect.y(), tilePx, height()),
            getTile(tileID++, samplesPerTile, tilePx)
        );
    }
}

QPixmap TracePlot::getTile(size_t tileID, size_t sampleCount, int tileWidthPx)
{
    QPixmap pixmap(tileWidthPx, height());
    // build tile key and mark as desired for this frame
    QString key;
    QTextStream ts(&key);
    ts << "traceplot_" << this << "_" << tileID << "_" << sampleCount;
    currentFrameKeys.insert(key);
    // if we already have a cached pixmap, return it immediately
    if (QPixmapCache::find(key, &pixmap))
        return pixmap;

    // schedule a new tile-draw if not already running or pending
    if (!tasks.contains(key) && !pendingInfo.contains(key)) {
        pendingInfo.insert(key, {tileID, sampleCount, tileWidthPx});
        debounceTimer->start();
    }
    pixmap.fill(Qt::transparent);
    return pixmap;
}

void TracePlot::drawTile(QString key, const QRect &rect, range_t<size_t> sampleRange)
{
    // if this tile is no longer part of the current view, abort early
    if (!currentFrameKeys.contains(key)) {
        tasks.remove(key);
        return;
    }
    // render into an offscreen image
    QImage image(rect.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    auto firstSample = sampleRange.minimum;
    auto length = sampleRange.length();

    // Is it a 2-channel (complex) trace?
    if (auto src = dynamic_cast<SampleSource<std::complex<float>>*>(sampleSource.get())) {
        auto samples = src->getSamples(firstSample, length);
        if (samples == nullptr)
            return;

        painter.setPen(Qt::red);
        plotTrace(painter, rect, reinterpret_cast<float*>(samples.get()), length, 2);
        painter.setPen(Qt::blue);
        plotTrace(painter, rect, reinterpret_cast<float*>(samples.get())+1, length, 2);

    // Otherwise is it single channel?
    } else if (auto src = dynamic_cast<SampleSource<float>*>(sampleSource.get())) {
        auto samples = src->getSamples(firstSample, length);
        if (samples == nullptr)
            return;

        painter.setPen(Qt::green);
        plotTrace(painter, rect, samples.get(), length, 1);
    } else {
        throw std::runtime_error("TracePlot::paintMid: Unsupported source type");
    }

    emit imageReady(key, image);
}

void TracePlot::handleImage(QString key, QImage image)
{
    auto pixmap = QPixmap::fromImage(image);
    QPixmapCache::insert(key, pixmap);
    tasks.remove(key);
    emit repaint();
}

void TracePlot::plotTrace(QPainter &painter, const QRect &rect, float *samples, size_t count, int step = 1)
{
    // Build a path by down-sampling to at most one point per pixel column.
    QPainterPath path;
    const int w = rect.width();
    const int h = rect.height();
    range_t<float> xRange{0.f, float(w - 2)};
    range_t<float> yRange{0.f, float(h - 2)};

    // Compute normalization range
    double minv = 1.0e6, maxv = -1.0e6;
    for (size_t i = 0; i < count; ++i) {
        float s = samples[i * step];
        if (s < minv) minv = s;
        if (s > maxv) maxv = s;
    }
    double range = maxv - minv;
    if (range <= 0.0)
        range = 1.0;
    double mid = (minv + maxv) * 0.5;
    double invRange = 1.0 / range;

    // Compute x-step per sample
    const double xStep = double(w) / double(count);
    // Down-sample: at most one point per pixel
    size_t decim = 1;
    if (size_t(w) < count)
        decim = (count + w - 1) / w;  // ceil(count/w)

    bool first = true;
    // Plot samples at intervals of decim
    for (size_t i = 0; i < count; i += decim) {
        float s = samples[i * step];
        double norm = (s - mid) * invRange;
        double x = i * xStep;
        double y = (1.0 - norm) * (h * 0.5);

        x = xRange.clip(x) + rect.x();
        y = yRange.clip(y) + rect.y();

        if (first) {
            path.moveTo(x, y);
            first = false;
        } else {
            path.lineTo(x, y);
        }
    }
    // Ensure last sample is included
    if (count > 0 && (count - 1) % decim != 0) {
        float s = samples[(count - 1) * step];
        double norm = (s - mid) * invRange;
        double x = double(w - 1);
        double y = (1.0 - norm) * (h * 0.5);

        x = xRange.clip(x) + rect.x();
        y = yRange.clip(y) + rect.y();
        path.lineTo(x, y);
    }
    painter.drawPath(path);
}
 
// Slot: called when debounce timer fires; schedule all pending tile draws
void TracePlot::schedulePendingTiles()
{
    // take current pending list and clear it
    QHash<QString, PendingInfo> info = std::move(pendingInfo);
    pendingInfo.clear();
    for (auto it = info.constBegin(); it != info.constEnd(); ++it) {
        const QString &key = it.key();
        size_t tileID = it.value().tileID;
        size_t sampleCount = it.value().sampleCount;
        int tilePx = it.value().tileWidth;
        range_t<size_t> sampleRange{ tileID * sampleCount,
                                    (tileID + 1) * sampleCount };
        // launch background draw (rect size uses tilePx)
        QtConcurrent::run(this, &TracePlot::drawTile,
                         key, QRect(0, 0, tilePx, height()), sampleRange);
        tasks.insert(key);
    }
}

// Slot: global min/max computed in background
void TracePlot::onMinMaxReady()
{
    auto p = minMaxWatcher->result();
    globalMin = p.first;
    globalMax = p.second;
    // ensure non-zero range
    if (globalMax <= globalMin) globalMax = globalMin + 1.0;
    emit repaint();
}
