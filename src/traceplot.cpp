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
#include <QtConcurrent>
#include <QThreadPool>
#include <QPainterPath>
#include <cmath>
#include <algorithm>
#include "samplesource.h"
#include "traceplot.h"

TracePlot::TracePlot(std::shared_ptr<AbstractSampleSource> source) : Plot(source)
{
    watcher = new QFutureWatcher<TraceSummary>(this);
    connect(watcher, &QFutureWatcher<TraceSummary>::finished, this, &TracePlot::summaryReady);
}

TracePlot::~TracePlot()
{
    if (cancel) cancel->store(true);
    // Workers own their source and token, never the plot; no GUI-thread wait.
}

void TracePlot::invalidateEvent()
{
    ++dataEpoch;
    if (cancel) cancel->store(true);
    wanted = false;
    hasSummary = false;
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
    // globalMin/globalMax computed with the trace summary; the float path
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


namespace {
QThreadPool *tracePool()
{
    // Shared bounded concurrency, independent of the machine's CPU count.
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(2); return true; }();
    (void)configured;
    return &pool;
}
}

void TracePlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    Key next{sampleRange.minimum, sampleRange.maximum >= sampleRange.minimum
        ? sampleRange.maximum-sampleRange.minimum : 0, std::min(rect.width(), 32768), dataEpoch};
    if (!wanted || next != desired) {
        desired = next;
        wanted = true;
        if (cancel) cancel->store(true);
    }
    if (hasSummary && completed == desired) drawSummary(painter, rect);
    else if (!busy) startSummary();
}

void TracePlot::startSummary()
{
    if (!wanted || busy || desired.width <= 0 || !desired.length) return;
    busy = true;
    running = desired;
    cancel = std::make_shared<std::atomic<bool>>(false);
    const auto token = cancel;
    const auto key = running;
    const auto source = sampleSource; // Keeps the processing chain alive on close.
    watcher->setFuture(QtConcurrent::run(tracePool(), [source, token, key] {
        try {
            if (auto f = dynamic_cast<SampleSource<float>*>(source.get()))
                return summarizeTrace(*f, key.start, key.length, key.width, *token);
            if (auto iq = dynamic_cast<SampleSource<std::complex<float>>*>(source.get()))
                return summarizeTrace(*iq, key.start, key.length, key.width, *token);
        } catch (const std::exception &error) {
            qWarning("Trace summary failed: %s", error.what());
        }
        return TraceSummary{};
    }));
}

void TracePlot::summaryReady()
{
    busy = false;
    if (wanted && running == desired && !cancel->load()) {
        summary = watcher->result();
        completed = running;
        hasSummary = true;
        if (std::isfinite(summary.minimum) && std::isfinite(summary.maximum)) {
            globalMin = summary.minimum;
            globalMax = summary.maximum > globalMin ? summary.maximum : globalMin+1;
        }
    }
    if (wanted && (!hasSummary || completed != desired)) startSummary();
    emit repaint();
}

void TracePlot::drawSummary(QPainter &painter, const QRect &rect)
{
    if (!summary.complete || rect.width() < 1 || rect.height() < 1) return;
    const double mid = .5*(globalMin+globalMax);
    const double scale = (summary.channels == 1 ? yScale : 1.0)/(globalMax-globalMin);
    auto y = [&](float value) {
        return rect.y()+std::max(0.0, std::min(double(rect.height()-1),
            (1.0-(value-mid)*scale)*rect.height()*.5));
    };
    painter.save();
    painter.setClipRect(rect, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int c = 0; c < summary.channels; ++c) {
        painter.setPen(summary.channels == 1 ? Qt::green : c == 0 ? Qt::red : Qt::blue);
        QPainterPath path;
        bool first = true;
        size_t run = 0;
        double lastX = 0, lastY = 0;
        auto endRun = [&] {
            if (run == 1 && rect.width() >= 2)
                path.lineTo(lastX < rect.right() ? lastX+1 : lastX-1, lastY);
            first = true; run = 0;
        };
        if (summary.dense) {
            const auto &points = summary.points[c];
            for (size_t i = 0; i < points.size(); ++i) {
                if (!std::isfinite(points[i])) { endRun(); continue; }
                lastX = rect.x()+std::min(double(rect.width()-1), double(i)*rect.width()/points.size());
                lastY = y(points[i]);
                if (first) path.moveTo(lastX, lastY);
                else path.lineTo(lastX, lastY);
                first = false; ++run;
            }
            endRun();
        } else {
            for (int x = 0; x < summary.width; ++x) {
                const auto &bucket = summary.columns[c][x];
                if (bucket.low > bucket.high) { first = true; continue; }
                const double px = rect.x()+double(x)*rect.width()/summary.width;
                double top = y(bucket.high), bottom = y(bucket.low);
                if (bottom-top < 1) {
                    top = std::min(top, double(rect.bottom())-1);
                    bottom = top+1;
                }
                if (first) path.moveTo(px, top);
                else path.lineTo(px, top);
                path.lineTo(px, bottom);
                first = false;
            }
        }
        painter.drawPath(path);
    }
    painter.restore();
}
