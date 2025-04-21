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

class TracePlot : public Plot
{
    Q_OBJECT

public:
    TracePlot(std::shared_ptr<AbstractSampleSource> source);

    void paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange);
    std::shared_ptr<AbstractSampleSource> source() { return sampleSource; };

signals:
    void imageReady(QString key, QImage image);

public slots:
    void handleImage(QString key, QImage image);

private slots:
    // Debounce timer expired: schedule all pending tile-draw tasks
    void schedulePendingTiles();

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
    // Width of each tile in pixels
    // default tile width in pixels (fallback)
    const int defaultTileWidth = 1000;

    // Request the pixmap for a given tile (width in pixels drives sample count)
    QPixmap getTile(size_t tileID, size_t sampleCount, int tileWidthPx);
    void drawTile(QString key, const QRect &rect, range_t<size_t> sampleRange);
    void plotTrace(QPainter &painter, const QRect &rect, float *samples, size_t count, int step);
};
