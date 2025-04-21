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
#include <QPainterPath>
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
}

void TracePlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    // start a fresh set of needed tiles for this paint pass
    currentFrameKeys.clear();
    if (sampleRange.length() == 0) return;

    int samplesPerColumn = std::max(1UL, sampleRange.length() / rect.width());
    // dynamic tile size to match thread count
    int threads = QThreadPool::globalInstance()->maxThreadCount();
    if (threads < 1) threads = 1;
    int tilePx = rect.width() / threads;
    if (tilePx < 1) tilePx = 1;
    int samplesPerTile = tilePx * samplesPerColumn;
    size_t tileID = sampleRange.minimum / samplesPerTile;
    size_t tileOffset = sampleRange.minimum % samplesPerTile; // Number of samples to skip from first image tile
    int xOffset = tileOffset / samplesPerColumn; // Number of columns to skip from first image tile

    // Paint first (possibly partial) tile
    // Paint first (possibly partial) tile
    painter.drawPixmap(
        QRect(rect.x(), rect.y(), tilePx - xOffset, height()),
        getTile(tileID++, samplesPerTile, tilePx),
        QRect(xOffset, 0, tilePx - xOffset, height())
    );

    // Paint remaining tiles
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
