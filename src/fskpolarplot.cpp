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

#include <QFontMetrics>
#include <algorithm>
#include <cmath>

namespace {
constexpr size_t kMaxSamples = 250000;
constexpr size_t kMaxPoints = 60000;
constexpr double kMinMag = 1e-8;
}

FskPolarPlot::FskPolarPlot(std::shared_ptr<SampleSource<std::complex<float>>> source)
    : Plot(source), iqSource(std::move(source))
{
}

void FskPolarPlot::setReferenceCutoff(double hz)
{
    referenceCutoffHz = hz;
    emit repaint();
}

void FskPolarPlot::setSelection(bool enabled, range_t<size_t> sampleRange)
{
    selectionEnabled = enabled;
    selectedRange = sampleRange;
    emit repaint();
}

size_t FskPolarPlot::delayFor(const QRect &rect, range_t<size_t> sampleRange) const
{
    const double rate = iqSource ? iqSource->rate() : 0.0;
    if (referenceCutoffHz > 0.0 && rate > 0.0) {
        const double d = rate / (4.0 * referenceCutoffHz);
        return std::max<size_t>(1, std::min<size_t>(8192, static_cast<size_t>(d + 0.5)));
    }

    const size_t span = sampleRange.maximum > sampleRange.minimum
        ? sampleRange.maximum - sampleRange.minimum
        : 1;
    const int w = std::max(1, rect.width());
    return std::max<size_t>(1, std::min<size_t>(8192, span / static_cast<size_t>(w * 4)));
}

void FskPolarPlot::paintBack(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    Q_UNUSED(sampleRange);

    painter.save();
    painter.fillRect(rect, Qt::black);

    const int side = std::max(10, std::min(rect.width(), rect.height()) - 16);
    const QPoint c(rect.left() + rect.width() / 2, rect.top() + rect.height() / 2);
    const int r = side / 2;

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(80, 80, 80), 1));
    painter.drawEllipse(c, r, r);
    painter.drawLine(c.x() - r, c.y(), c.x() + r, c.y());
    painter.drawLine(c.x(), c.y() - r, c.x(), c.y() + r);

    painter.setPen(QColor(180, 180, 180));
    const QString title = QStringLiteral("FSK differential phase");
    painter.drawText(rect.left() + 6, rect.top() + painter.fontMetrics().ascent() + 4, title);
    painter.restore();
}

void FskPolarPlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    if (selectionEnabled && selectedRange.maximum > selectedRange.minimum)
        sampleRange = selectedRange;

    if (!iqSource || sampleRange.maximum <= sampleRange.minimum)
        return;

    const size_t delay = delayFor(rect, sampleRange);
    size_t len = sampleRange.maximum - sampleRange.minimum;
    if (len <= delay + 1)
        return;

    size_t start = sampleRange.minimum;
    if (len > kMaxSamples) {
        start += (len - kMaxSamples) / 2;
        len = kMaxSamples;
    }

    auto samples = iqSource->getSamples(start, len);
    if (!samples)
        return;

    const int side = std::max(10, std::min(rect.width(), rect.height()) - 16);
    const QPoint c(rect.left() + rect.width() / 2, rect.top() + rect.height() / 2);
    const double radius = side * 0.47;
    const size_t usable = len > delay ? len - delay : 0;
    const size_t step = std::max<size_t>(1, usable / kMaxPoints);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(40, 230, 80, 90));

    for (size_t i = delay; i < len; i += step) {
        const auto d = samples[i] * std::conj(samples[i - delay]);
        const double mag = std::abs(d);
        if (!std::isfinite(mag) || mag < kMinMag)
            continue;
        const double x = d.real() / mag;
        const double y = d.imag() / mag;
        const int px = c.x() + static_cast<int>(x * radius);
        const int py = c.y() - static_cast<int>(y * radius);
        painter.drawPoint(px, py);
    }

    painter.setPen(QColor(210, 210, 210));
    const QString info = selectionEnabled
        ? QStringLiteral("selection, delay %1 samples").arg(delay)
        : QStringLiteral("visible range, delay %1 samples").arg(delay);
    painter.drawText(rect.left() + 6, rect.bottom() - 6, info);
    painter.restore();
}
