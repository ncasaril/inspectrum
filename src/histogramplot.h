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

#pragma once

#include "plot.h"
#include "samplesource.h"

#include <QFutureWatcher>
#include <QImage>
#include <memory>

// Value histogram of a float derived source. The number of peaks reveals the
// modulation order clock-free: 2-FSK shows two lobes, 4-FSK four, GFSK smears
// them, a PSK/noise blob is unimodal. Built on a worker thread (the float
// source can fan out to the batched FM-demod), keyed so idle repaints are free.
class HistogramPlot : public Plot
{
public:
    HistogramPlot(std::shared_ptr<SampleSource<float>> source);
    void paintBack(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void setSelection(bool enabled, range_t<size_t> sampleRange);

    // Public so the file-scope worker render helper can take it by value.
    struct RenderKey {
        size_t start = 0;
        size_t len = 0;
        int w = 0;
        int h = 0;
        bool operator==(const RenderKey &o) const {
            return start == o.start && len == o.len && w == o.w && h == o.h;
        }
        bool operator!=(const RenderKey &o) const { return !(*this == o); }
    };

private:
    std::shared_ptr<SampleSource<float>> floatSource;
    bool selectionEnabled = false;
    range_t<size_t> selectedRange{0, 0};

    QFutureWatcher<QImage> *watcher_ = nullptr;
    QImage image_;
    RenderKey imageKey_;
    RenderKey runningKey_;
    RenderKey pendingKey_;
    bool hasImage_ = false;
    bool running_ = false;
    bool havePending_ = false;

    void startRender(const RenderKey &k);
    void onRenderReady();
};
