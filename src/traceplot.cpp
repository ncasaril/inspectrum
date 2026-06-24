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

#include <QDebug>
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
#include "latencylog.h"

#define INSPECTRUM_TRACE_DEBUG 0

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

void TracePlot::invalidateEvent()
{
    // Force a fresh min/max scan next paint and unreach cached tiles. We keep
    // the previous globalMin/Max around until the new scan completes so the
    // first post-invalidate frame is at least drawable rather than blank.
    // Bumping dataEpoch unreaches both the complex-tile cache (key includes
    // dataEpoch) and the async float image cache (FloatKey carries it), so
    // we naturally fall through to a re-render on next paint.
    //
    // We deliberately bump *only* on real upstream changes here, not on
    // min/max wobbles — see applyMinMax for the rationale. Successive
    // tuner positions can produce min/max scans that differ by 1-3% from
    // pure FIR/cache boundary noise, and re-rendering for those would
    // cascade for seconds after every release.
    firstMinMax = true;
    ++dataEpoch;
    // Drop the stale float-trace image so paintMid blanks the plot until
    // the in-flight worker delivers a fresh one. Showing stale-but-pretty
    // data during a drag made the user think the worker had stalled — a
    // brief blank frame is a clearer "we are recomputing" signal.
    floatHasImage_ = false;
    emit repaint();
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

void TracePlot::setHoverCursor(bool active, size_t sampleIdx,
                               double value, QString valueText)
{
    hoverActive_ = active;
    hoverSample_ = sampleIdx;
    hoverValue_  = value;
    hoverText_   = std::move(valueText);
    emit repaint();
}

void TracePlot::setPeriodMarkers(std::vector<size_t> peakSamples)
{
    periodMarkers_ = std::move(peakSamples);
    emit repaint();
}

void TracePlot::paintFront(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    // Draw a left-margin y-axis (min / mid / max) and a dashed zero line when
    // zero is within the currently-displayed range. Values come from the shared
    // globalMin/globalMax computed in scheduleMinMaxIfNeeded(); the float path
    // additionally compresses/expands by yScale, so reflect that here.
    double minv = globalMin;
    double maxv = globalMax;
    if (maxv <= minv) maxv = minv + 1.0;
    double mid = 0.5 * (minv + maxv);
    double axisMin = minv;
    double axisMax = maxv;
    if (dynamic_cast<SampleSource<float>*>(sampleSource.get()) && yScale > 0.0) {
        double halfRange = 0.5 * (maxv - minv) / yScale;
        axisMin = mid - halfRange;
        axisMax = mid + halfRange;
    }
    double visibleSpan = axisMax - axisMin;
    if (visibleSpan <= 0.0) visibleSpan = 1.0;

    painter.save();

    // Dashed zero line when 0 is visible.
    if (axisMin < 0.0 && axisMax > 0.0) {
        double normZero = -2.0 * mid / visibleSpan;
        int zeroY = rect.y() + static_cast<int>((1.0 - normZero) * (rect.height() * 0.5));
        QPen zeroPen(QColor(200, 200, 200, 110), 1, Qt::DashLine);
        painter.setPen(zeroPen);
        painter.drawLine(rect.left(), zeroY, rect.right(), zeroY);
    }

    // Left-margin scale with a translucent backdrop so it reads over the trace.
    QFont f = painter.font();
    f.setPointSizeF(8.0);
    painter.setFont(f);
    QFontMetrics fm(f);

    auto fmt = [](double v) -> QString {
        if (std::abs(v) < 1e-12) return QStringLiteral("0");
        return QString::number(v, 'g', 3);
    };
    QString sMax = fmt(axisMax);
    QString sMid = fmt(mid);
    QString sMin = fmt(axisMin);

    const int textMargin = 4;
    int maxW = std::max({fm.width(sMax), fm.width(sMid), fm.width(sMin)});
    int bgW = maxW + textMargin * 2;
    painter.fillRect(rect.left(), rect.top(), bgW, rect.height(), QColor(0, 0, 0, 140));

    painter.setPen(QColor(220, 220, 220));
    int x = rect.left() + textMargin;
    painter.drawText(x, rect.top() + fm.ascent() + 1, sMax);
    painter.drawText(x, rect.top() + rect.height() / 2 + fm.ascent() / 2 - 1, sMid);
    painter.drawText(x, rect.bottom() - fm.descent() - 1, sMin);

    // Sample-to-pixel mapping for the marker overlays.
    auto sampleToX = [&](size_t s) -> int {
        if (sampleRange.maximum <= sampleRange.minimum) return rect.left();
        double t = (static_cast<double>(s) - sampleRange.minimum) /
                   (sampleRange.maximum - sampleRange.minimum);
        return rect.left() + static_cast<int>(t * rect.width());
    };
    // Mirror paintMid's float-trace mapping exactly: with yScale=1 the
    // data fills the middle half of the plot, so v=maxv lands at h/4 (not
    // at the top — the top of the rect is "empty headroom"). Using the
    // same formula keeps the hover dot on top of the rendered green line
    // instead of at a different scale.
    const int plotH = height();
    const double invRange = (maxv > minv) ? (yScale / (maxv - minv)) : 1.0;
    auto valueToY = [&](double v) -> int {
        double norm = (v - mid) * invRange;
        if (norm >  1.0) norm =  1.0;
        if (norm < -1.0) norm = -1.0;
        return rect.y() + static_cast<int>((1.0 - norm) * (plotH * 0.5));
    };

    // Period markers: small upward triangle at each peak that's in view, plus
    // a horizontal line connecting consecutive in-view peaks at the trace's
    // top so the user can see the period span at a glance. Skip cleanly when
    // there are < 2 in-view markers.
    if (periodMarkers_.size() >= 2) {
        QPen markerPen(QColor(255, 200, 0, 220), 1);
        painter.setPen(markerPen);
        const int triH = 6;
        const int yLine = rect.top() + 3;
        int prevX = -1;
        for (size_t s : periodMarkers_) {
            if (s < sampleRange.minimum || s >= sampleRange.maximum) continue;
            int mx = sampleToX(s);
            // Triangle pointing down from yLine
            QPolygon tri;
            tri << QPoint(mx, yLine + triH)
                << QPoint(mx - triH/2, yLine)
                << QPoint(mx + triH/2, yLine);
            painter.setBrush(QColor(255, 200, 0, 220));
            painter.drawPolygon(tri);
            painter.setBrush(Qt::NoBrush);
            // Vertical guide down to the trace area
            painter.setPen(QPen(QColor(255, 200, 0, 60), 1, Qt::DashLine));
            painter.drawLine(mx, yLine + triH, mx, rect.bottom());
            painter.setPen(markerPen);
            // Connecting line to the previous in-view peak at the marker row
            if (prevX >= 0) {
                painter.drawLine(prevX, yLine + triH, mx, yLine + triH);
            }
            prevX = mx;
        }
    }

    // Hover-cursor overlay: vertical line at the cursor's sample, small
    // filled dot at the data value, and the value text in a translucent
    // pill (placed above or below the dot to stay on-screen).
    if (hoverActive_ &&
        hoverSample_ >= sampleRange.minimum &&
        hoverSample_ <  sampleRange.maximum)
    {
        const int cx = sampleToX(hoverSample_);
        // Vertical guide
        painter.setPen(QPen(QColor(120, 220, 255, 180), 1, Qt::DashLine));
        painter.drawLine(cx, rect.top(), cx, rect.bottom());
        // Dot at the sample value (only if the value is finite and in range)
        if (std::isfinite(hoverValue_)) {
            int cy = valueToY(hoverValue_);
            cy = std::max(rect.top(), std::min(rect.bottom(), cy));
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(120, 220, 255, 230));
            painter.drawEllipse(QPoint(cx, cy), 3, 3);
            painter.setBrush(Qt::NoBrush);
        }
        // Text label
        if (!hoverText_.isEmpty()) {
            QFontMetrics tm(painter.font());
            int tw = tm.width(hoverText_);
            int th = tm.height();
            int pad = 3;
            int tx = cx + 6;
            // Flip to the left if running off the right edge
            if (tx + tw + 2 * pad > rect.right()) tx = cx - 6 - tw - 2 * pad;
            int ty = rect.top() + 2;
            painter.fillRect(tx, ty, tw + 2 * pad, th + 2 * pad,
                             QColor(0, 0, 0, 180));
            painter.setPen(QColor(120, 220, 255));
            painter.drawText(tx + pad, ty + pad + tm.ascent(), hoverText_);
        }
    }

    painter.restore();
}

static QPair<double,double> scanFloatRange(SampleSource<float> *src, range_t<size_t> range)
{
    size_t count = range.maximum - range.minimum;
    QPair<double,double> result{
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    auto data = src->getSamples(range.minimum, count);
    if (data) {
        for (size_t i = 0; i < count; ++i) {
            double v = data[i];
            // Skip NaN/Inf — freqdem's fresh-state output near t=0 can emit
            // non-finite samples which would poison min/max comparisons
            // (NaN < x is always false).
            if (!std::isfinite(v)) continue;
            if (v < result.first)  result.first  = v;
            if (v > result.second) result.second = v;
        }
    }
    return result;
}

static QPair<double,double> scanComplexRange(SampleSource<std::complex<float>> *src, range_t<size_t> range)
{
    size_t count = range.maximum - range.minimum;
    QPair<double,double> result{
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    auto data = src->getSamples(range.minimum, count);
    if (data) {
        for (size_t i = 0; i < count; ++i) {
            double re = data[i].real();
            double im = data[i].imag();
            if (std::isfinite(re)) {
                if (re < result.first)  result.first  = re;
                if (re > result.second) result.second = re;
            }
            if (std::isfinite(im)) {
                if (im < result.first)  result.first  = im;
                if (im > result.second) result.second = im;
            }
        }
    }
    return result;
}

void TracePlot::applyMinMax(QPair<double,double> result)
{
#if INSPECTRUM_TRACE_DEBUG
    qDebug().nospace() << "[TP " << this << "] applyMinMax got ("
                       << result.first << ", " << result.second << ")"
                       << " current global=(" << globalMin << ", " << globalMax << ")";
#endif
    if (!std::isfinite(result.first) || !std::isfinite(result.second)) {
#if INSPECTRUM_TRACE_DEBUG
        qDebug() << "[TP" << this << "] applyMinMax REJECTED — non-finite (no valid samples in scan range)";
#endif
        return;
    }
    const double newMin = result.first;
    double newMax = result.second;
    if (newMax <= newMin) newMax = newMin + 1.0;

    // Tolerance-gate the bump: each render kicks off a scan, and the scan's
    // result wobbles by tiny float amounts every cycle (different cache
    // fill boundaries, FIR transient differences). Without a tolerance the
    // epoch keeps bumping → keeps invalidating the float-trace key → keeps
    // re-rendering. Net effect: the trace never "settles" after a tuner
    // drag and the user sees several seconds of churn.
    //
    // 1% of the current range is well below visual significance (a 200 px
    // tall plot would shift by <2 px), so reject sub-tolerance updates and
    // leave globalMin/Max unchanged so the axis labels match the cached
    // image's mapping exactly.
    const double oldRange = std::max(globalMax - globalMin, 1.0);
    const double tol = std::max(1e-9, oldRange * 0.01);
    const bool significant = std::abs(newMin - globalMin) > tol ||
                             std::abs(newMax - globalMax) > tol;
    if (!significant) return;

    const double prevMin = globalMin, prevMax = globalMax;
    globalMin = newMin;
    globalMax = newMax;
    ++minMaxEpoch;
    LatencyLog::markf("traceplot[%p] minMax bump epoch=%d (%.4g..%.4g -> %.4g..%.4g)",
                      (void*)this, minMaxEpoch, prevMin, prevMax, globalMin, globalMax);
}

void TracePlot::scheduleMinMaxIfNeeded(range_t<size_t> sampleRange)
{
    bool rangeChanged = firstMinMax ||
        sampleRange.minimum != minMaxRange.minimum ||
        (sampleRange.maximum - sampleRange.minimum) !=
            (minMaxRange.maximum - minMaxRange.minimum);
    if (!rangeChanged || minMaxWatcher->isRunning())
        return;
    minMaxRange = sampleRange;
    firstMinMax = false;

    // Always async, including the first scan — a sync scan here means the
    // GUI thread blocks on getSamples (which can fan out to the FFT-LPF in
    // FrequencyDemod, hundreds of ms for large views). We accept that the
    // very first frame after a parameter change uses the previous globalMin/
    // Max (or the default 0..1 if this plot has never rendered) — the float
    // image renderer will re-run automatically once the scan finishes via the
    // minMaxEpoch bump in applyMinMax.
    auto rangeCopy = sampleRange;
    LatencyLog::markf("traceplot[%p] minMax scan dispatch range=[%zu..%zu)",
                      (void*)this, rangeCopy.minimum, rangeCopy.maximum);
    if (auto srcF = dynamic_cast<SampleSource<float>*>(sampleSource.get())) {
        auto srcPtr = srcF;
        minMaxWatcher->setFuture(QtConcurrent::run([srcPtr, rangeCopy]() {
            return scanFloatRange(srcPtr, rangeCopy);
        }));
    } else if (auto srcC = dynamic_cast<SampleSource<std::complex<float>>*>(sampleSource.get())) {
        auto srcPtr = srcC;
        minMaxWatcher->setFuture(QtConcurrent::run([srcPtr, rangeCopy]() {
            return scanComplexRange(srcPtr, rangeCopy);
        }));
    }
}

void TracePlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    // Shared: kick off background global min/max whenever the range changes.
    // Used for consistent vertical scaling across both paths and across tiles.
    scheduleMinMaxIfNeeded(sampleRange);

    // Single-channel (float) derived plots: render to an offscreen QImage on
    // a worker thread and blit the most recent completed frame here. The GUI
    // thread never builds the path or pulls samples; pan/zoom/parameter
    // changes just bump the desired key, and as soon as the in-flight render
    // finishes we request the latest one. The user sees one slightly stale
    // frame while the new one renders, and a fresh frame at "as fast as the
    // worker can complete" rate during a sustained drag.
    if (auto srcF = dynamic_cast<SampleSource<float>*>(sampleSource.get())) {
        const int w = rect.width();
        const int h = height();
        if (w < 1 || h < 1) return;
        const size_t start = sampleRange.minimum;
        const size_t len = sampleRange.maximum - sampleRange.minimum;

        FloatKey k{start, len, w, h, yScale, dataEpoch};
        floatPendingKey_ = k;
        floatPendingValid_ = true;

        // Only blit when the cached image actually matches the current key.
        // Showing a stale image during a tuner/zoom drag was misleading —
        // the user couldn't tell whether the worker was running or stuck,
        // and an out-of-date trace masquerades as live data. A blank plot
        // while computation is in flight + an instant fresh frame on
        // completion is a more honest signal.
        if (floatHasImage_ && floatImageKey_ == k) {
            painter.drawImage(rect, floatImage_);
        }

        const bool needRender = len > 0 &&
            (!floatHasImage_ || floatImageKey_ != k);
        if (needRender && !floatRunning_) {
            double minv = globalMin;
            double maxv = globalMax;
            if (maxv <= minv) maxv = minv + 1.0;
            double mid = 0.5 * (minv + maxv);
            double invRange = yScale / (maxv - minv);
            LatencyLog::markf("traceplot[%p] paintMid float: dispatch render len=%zu w=%d",
                              (void*)this, len, w);
            startFloatRender(k, mid, invRange);
        } else if (needRender) {
            LatencyLog::markf("traceplot[%p] paintMid float: render busy, key shifted",
                              (void*)this);
        }
        return;
    }
    // Fallback: raw complex or multi-channel trace uses threaded pixmap pipeline.
    // Tiles share the same globalMin/globalMax, so amplitude is consistent across edges.
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
    ts << "traceplot_" << this << "_" << tileID << "_" << sampleCount
       << "_" << dataEpoch;
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
    // NOTE: runs on a QtConcurrent worker thread. Do not touch currentFrameKeys
    // or tasks from here — they live on the GUI thread and QSet isn't
    // thread-safe. Any early-exit / bookkeeping based on those sets happens in
    // handleImage() on the GUI thread (at the cost of a potentially wasted
    // render for tiles that have scrolled out of view).
    QImage image(rect.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    auto firstSample = sampleRange.minimum;
    auto length = sampleRange.length();

    // Snapshot the shared global range so every tile renders at the same scale.
    double minv = globalMin;
    double maxv = globalMax;
    if (maxv <= minv) maxv = minv + 1.0;
    double mid = 0.5 * (minv + maxv);
    double invRange = 1.0 / (maxv - minv);

    // Is it a 2-channel (complex) trace?
    if (auto src = dynamic_cast<SampleSource<std::complex<float>>*>(sampleSource.get())) {
        auto samples = src->getSamples(firstSample, length);
        if (samples) {
            painter.setPen(Qt::red);
            plotTrace(painter, rect, reinterpret_cast<float*>(samples.get()), length, 2, mid, invRange);
            painter.setPen(Qt::blue);
            plotTrace(painter, rect, reinterpret_cast<float*>(samples.get())+1, length, 2, mid, invRange);
        }

    // Otherwise is it single channel?
    } else if (auto src = dynamic_cast<SampleSource<float>*>(sampleSource.get())) {
        auto samples = src->getSamples(firstSample, length);
        if (samples) {
            painter.setPen(Qt::green);
            plotTrace(painter, rect, samples.get(), length, 1, mid, invRange);
        }
    } else {
        throw std::runtime_error("TracePlot::drawTile: Unsupported source type");
    }

    // Always emit — handleImage needs to clear the in-flight bookkeeping even
    // when getSamples returned null (otherwise the tile stays "in flight"
    // indefinitely and never re-schedules).
    emit imageReady(key, image);
}

void TracePlot::handleImage(QString key, QImage image)
{
    // Worker finished; clear the in-flight bookkeeping on the GUI thread.
    tasks.remove(key);
    // If this tile is no longer desired (viewport moved past it while the
    // worker was running), drop it without caching.
    if (!currentFrameKeys.contains(key))
        return;
    auto pixmap = QPixmap::fromImage(image);
    QPixmapCache::insert(key, pixmap);
    emit repaint();
}

void TracePlot::plotTrace(QPainter &painter, const QRect &rect, float *samples,
                          size_t count, int step, double mid, double invRange)
{
    // Build a path by down-sampling to at most one point per pixel column.
    // Scaling (mid, invRange) is supplied by the caller so all tiles share one range.
    QPainterPath path;
    const int w = rect.width();
    const int h = rect.height();
    range_t<float> xRange{0.f, float(w - 2)};
    range_t<float> yRange{0.f, float(h - 2)};

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
    LatencyLog::markf("traceplot[%p] minMax scan ready", (void*)this);
    applyMinMax(minMaxWatcher->result());
    emit repaint();
}

// Render a float trace into an offscreen image. Runs on a QtConcurrent
// worker — the only main-thread state it touches is the SampleSource pointer,
// which the chain already supports concurrent reads on (the complex tile
// path has been doing exactly that since this fork landed).
static QImage renderFloatTrace(SampleSource<float> *src,
                               size_t start, size_t len, int w, int h,
                               double mid, double invRange)
{
    LatencyLog::markf("renderFloatTrace start src=%p len=%zu w=%d", (void*)src, len, w);
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    if (len == 0 || w < 1 || h < 1) return image;
    auto samples = src->getSamples(start, len);
    LatencyLog::markf("renderFloatTrace samples_ready src=%p", (void*)src);
    if (!samples) return image;

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::green);

    QPainterPath path;
    bool first = true;
    const double xStep = double(w) / double(len);
    const size_t decim = (len > size_t(w)) ? (len + w - 1) / w : 1;
    auto addPoint = [&](size_t i, double xCoord) {
        double s = samples[i];
        // Break the path at a non-finite sample (squelch / cold-start gap) so
        // the next finite point starts a fresh subpath instead of bridging the
        // gap with a straight line.
        if (!std::isfinite(s)) { first = true; return; }
        double norm = (s - mid) * invRange;
        if (norm >  1.0) norm =  1.0;
        if (norm < -1.0) norm = -1.0;
        double y = (1.0 - norm) * (h * 0.5);
        if (first) { path.moveTo(xCoord, y); first = false; }
        else       { path.lineTo(xCoord, y); }
    };
    for (size_t i = 0; i < len; i += decim) {
        addPoint(i, i * xStep);
    }
    if (len > 0 && (len - 1) % decim != 0) {
        addPoint(len - 1, w - 1);
    }
    painter.drawPath(path);
    return image;
}

void TracePlot::startFloatRender(const FloatKey &k, double mid, double invRange)
{
    if (!floatWatcher_) {
        floatWatcher_ = new QFutureWatcher<QImage>(this);
        connect(floatWatcher_, &QFutureWatcher<QImage>::finished,
                this, &TracePlot::onFloatImageReady);
    }
    auto srcF = dynamic_cast<SampleSource<float>*>(sampleSource.get());
    if (!srcF) return;
    floatRunning_ = true;
    floatRunningKey_ = k;
    auto kCopy = k;
    auto srcPtr = srcF;
    floatWatcher_->setFuture(QtConcurrent::run([srcPtr, kCopy, mid, invRange]() {
        return renderFloatTrace(srcPtr, kCopy.start, kCopy.len,
                                kCopy.w, kCopy.h, mid, invRange);
    }));
}

void TracePlot::onFloatImageReady()
{
    floatImage_ = floatWatcher_->result();
    floatImageKey_ = floatRunningKey_;
    floatHasImage_ = true;
    floatRunning_ = false;
    LatencyLog::markf("traceplot[%p] onFloatImageReady (back on GUI)", (void*)this);
    // If the desired view has moved on while we were rendering (pan, zoom,
    // tuner shift, FM cutoff change, etc.), kick off another render right
    // away so the worker stays busy and the user gets continuous updates.
    if (floatPendingValid_ && floatPendingKey_ != floatImageKey_) {
        double minv = globalMin;
        double maxv = globalMax;
        if (maxv <= minv) maxv = minv + 1.0;
        double mid = 0.5 * (minv + maxv);
        double invRange = yScale / (maxv - minv);
        startFloatRender(floatPendingKey_, mid, invRange);
    }
    emit repaint();
}
