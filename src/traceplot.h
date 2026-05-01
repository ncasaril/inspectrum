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
    // When upstream data changes (tuner moved, LPF retuned, etc.) invalidate
    // our cached min/max so the next paint reschedules the background scan,
    // and bump the tile-cache epoch so stale complex-path tiles aren't reused.
    void invalidateEvent() override;
    std::shared_ptr<AbstractSampleSource> source() { return sampleSource; };
    // Handle vertical zoom via mouse wheel
    bool wheelEvent(QWheelEvent *event) override;
    // Set/clear the hover-cursor overlay (vertical line + value label drawn
    // by paintFront). PlotView sets this on the plot under the mouse, and
    // clears all others, so only the active plot shows the marker.
    void setHoverCursor(bool active, size_t sampleIdx,
                        double value, QString valueText);
    // Peak markers used by the auto-period analyser. Each entry is an
    // absolute sample index where a peak was detected; paintFront draws a
    // small triangle at each peak that's currently in view and a horizontal
    // line connecting consecutive peaks so the period is visible at a
    // glance. Pass an empty vector to clear.
    void setPeriodMarkers(std::vector<size_t> peakSamples);

signals:
    void imageReady(QString key, QImage image);

public slots:
    void handleImage(QString key, QImage image);

private slots:
    // Debounce timer expired: schedule all pending tile-draw tasks
    void schedulePendingTiles();
    // Background min/max for float plots is ready
    void onMinMaxReady();
    // Async float-trace renderer finished — swap in the new image and, if the
    // view has moved on while it ran, kick off another render for the latest key.
    void onFloatImageReady();

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
    // Bumped only by invalidateEvent (real upstream data changes — tuner
    // moved, FM cutoff changed, etc.). Used as the cache-invalidation signal
    // for both the complex-tile pixmap cache and the float-trace image
    // cache. Splitting this from any min/max-driven bump means a tiny
    // scale wobble (3% range shift after a tuner move) doesn't churn the
    // entire cached render — the user sees the rendered trace, plus axis
    // labels that paintFront draws live from globalMin/Max each frame.
    int dataEpoch = 0;
    // Counter that's bumped on min/max changes; kept for the trace cache
    // logic that wants to know "did min/max move at all". Currently only
    // used to drive the LatencyLog message; could be repurposed if the
    // axis layer ever wants to debounce its own redraw on it.
    int minMaxEpoch = 0;
    // Width of each tile in pixels
    // default tile width in pixels (fallback)
    const int defaultTileWidth = 1000;
    // Hover-cursor state — drawn by paintFront on top of the trace.
    bool         hoverActive_ = false;
    size_t       hoverSample_ = 0;
    double       hoverValue_  = 0.0;
    QString      hoverText_;
    // Auto-period peak markers (absolute sample indices). Drawn by
    // paintFront whenever the visible range overlaps with the marker.
    std::vector<size_t> periodMarkers_;

    // Async float-trace render state. Single-image pipeline: paintMid blits
    // the last completed image (possibly stale) and requests a new render
    // whenever the key shifts. Workers run on the global thread pool; we
    // keep at most one in flight per plot, and as soon as it finishes we
    // launch the next one if the latest key has moved on.
    struct FloatKey {
        size_t   start = 0;
        size_t   len   = 0;
        int      w     = 0;
        int      h     = 0;
        double   yScale = 1.0;
        int      epoch = 0;   // mirrors TracePlot::dataEpoch
        bool operator==(const FloatKey &o) const {
            return start==o.start && len==o.len && w==o.w && h==o.h
                && yScale==o.yScale && epoch==o.epoch;
        }
        bool operator!=(const FloatKey &o) const { return !(*this == o); }
    };
    QImage                       floatImage_;
    FloatKey                     floatImageKey_{};
    bool                         floatHasImage_ = false;
    FloatKey                     floatPendingKey_{};
    bool                         floatPendingValid_ = false;
    FloatKey                     floatRunningKey_{};
    bool                         floatRunning_ = false;
    QFutureWatcher<QImage>      *floatWatcher_ = nullptr;

    // Kick off a background global min/max compute if the view has changed.
    void scheduleMinMaxIfNeeded(range_t<size_t> sampleRange);
    // Update globalMin/Max and bump the epoch on change; skips non-finite inputs.
    void applyMinMax(QPair<double,double> result);
    // Launch a background render for the given key. Caller must guarantee
    // floatRunning_ is false.
    void startFloatRender(const FloatKey &k, double mid, double invRange);
    // Request the pixmap for a given tile (width in pixels drives sample count)
    QPixmap getTile(size_t tileID, size_t sampleCount, int tileWidthPx);
    void drawTile(QString key, const QRect &rect, range_t<size_t> sampleRange);
    void plotTrace(QPainter &painter, const QRect &rect, float *samples,
                   size_t count, int step, double mid, double invRange);
};
