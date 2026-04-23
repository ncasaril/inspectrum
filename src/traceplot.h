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

#pragma once
#include <memory>
#include "abstractsamplesource.h"
#include "plot.h"
#include "util.h"
#include <QTimer>
#include <QHash>
#include <QPair>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QWheelEvent>

class TracePlot : public Plot
{
    Q_OBJECT

public:
    TracePlot(std::shared_ptr<AbstractSampleSource> source);

    void paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange);
    void paintFront(QPainter &painter, QRect &rect, range_t<size_t> sampleRange);
    std::shared_ptr<AbstractSampleSource> source() { return sampleSource; };
    // Handle vertical zoom via mouse wheel
    bool wheelEvent(QWheelEvent *event) override;

signals:
    void imageReady(QString key, QImage image);

public slots:
    void handleImage(QString key, QImage image);

private slots:
    // Debounce timer expired: schedule all pending tile-draw tasks
    void schedulePendingTiles();
    // Background min/max for float plots is ready
    void onMinMaxReady();

private:
    // In-process tile keys
    QSet<QString> tasks;
    // Tiles waiting to be scheduled: key -> (tileID, sampleCount, tileWidthPx)
    struct PendingInfo { size_t tileID; size_t sampleCount; int tileWidth; };
    QHash<QString, PendingInfo> pendingInfo;
    // Keys desired in the current paint frame (for early-exit)
    QSet<QString> currentFrameKeys;
    // Debounce timer for batching tile requests
    QTimer *debounceTimer;
    // Scale factor for vertical zoom
    double yScale = 1.0;
    // Background worker for global min/max
    QFutureWatcher<QPair<double,double>> *minMaxWatcher;
    // Range for which min/max is computed
    range_t<size_t> minMaxRange;
    bool firstMinMax;
    // Last-known global min/max
    double globalMin = 0.0;
    double globalMax = 1.0;
    // Bumps whenever globalMin/Max change; included in tile cache keys so old
    // tiles (rendered with a stale scale) become unreachable and get replaced.
    int minMaxEpoch = 0;
    // Width of each tile in pixels
    // default tile width in pixels (fallback)
    const int defaultTileWidth = 1000;

    // Kick off a background global min/max compute if the view has changed.
    void scheduleMinMaxIfNeeded(range_t<size_t> sampleRange);
    // Request the pixmap for a given tile (width in pixels drives sample count)
    QPixmap getTile(size_t tileID, size_t sampleCount, int tileWidthPx);
    void drawTile(QString key, const QRect &rect, range_t<size_t> sampleRange);
    void plotTrace(QPainter &painter, const QRect &rect, float *samples,
                   size_t count, int step, double mid, double invRange);
};
