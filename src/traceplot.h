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
#include <atomic>
#include "abstractsamplesource.h"
#include "plot.h"
#include "util.h"
#include "tracesummary.h"
#include <QFutureWatcher>
#include <QWheelEvent>

class TracePlot : public Plot
{
    Q_OBJECT
public:
    TracePlot(std::shared_ptr<AbstractSampleSource> source);
    ~TracePlot() override;
    void paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void paintFront(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void invalidateEvent() override;
    std::shared_ptr<AbstractSampleSource> source() { return sampleSource; }
    bool wheelEvent(QWheelEvent *event) override;
    void setHoverCursor(bool active, size_t sampleIdx, double value, QString valueText);
    void setPeriodMarkers(std::vector<size_t> peakSamples);
private:
    struct Key {
        size_t start = 0, length = 0;
        int width = 0;
        uint64_t epoch = 0;
        bool operator==(const Key &other) const {
            return start == other.start && length == other.length &&
                width == other.width && epoch == other.epoch;
        }
        bool operator!=(const Key &other) const { return !(*this == other); }
    };
    void startSummary();
    void summaryReady();
    void drawSummary(QPainter &painter, const QRect &rect);
    QFutureWatcher<TraceSummary> *watcher;
    std::shared_ptr<std::atomic<bool>> cancel;
    Key desired, running, completed;
    bool wanted = false, busy = false, hasSummary = false;
    uint64_t dataEpoch = 0;
    TraceSummary summary;
    double yScale = 1.0, globalMin = 0.0, globalMax = 1.0;
    bool hoverActive_ = false;
    size_t hoverSample_ = 0;
    double hoverValue_ = 0.0;
    QString hoverText_;
    std::vector<size_t> periodMarkers_;
};
