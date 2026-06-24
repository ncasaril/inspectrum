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
#include <complex>
#include <memory>

// Differential-phase constellation: plots d = s[i]·conj(s[i-delay]) on the
// unit circle, where `delay` is one symbol period (Fs / symbol rate). This is
// the natural domain for differentially-encoded PSK (π/4-DQPSK clusters at
// ±45°/±135°) and a useful cross-check for FSK. The scatter is accumulated as
// a per-pixel density heatmap on a worker thread so symbol-centre phases
// (revisited often) glow brighter than the inter-symbol transition smear.
class FskPolarPlot : public Plot
{
public:
    FskPolarPlot(std::shared_ptr<SampleSource<std::complex<float>>> source);
    void paintBack(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    // Symbol rate in baud. Drives the differential delay (= round(Fs/baud)).
    // 0 (unset) draws nothing but a hint — a wrong delay yields a meaningless
    // smear, so we decline to guess.
    void setSymbolRate(double baud);
    void setSelection(bool enabled, range_t<size_t> sampleRange);

    // Everything that determines the rendered image. The worker re-runs only
    // when this changes, so hover/scroll that don't move the window are free.
    // Public so the file-scope worker render helper can take it by value.
    struct RenderKey {
        size_t start = 0;
        size_t len = 0;
        size_t delay = 0;
        int w = 0;
        int h = 0;
        bool operator==(const RenderKey &o) const {
            return start == o.start && len == o.len && delay == o.delay &&
                   w == o.w && h == o.h;
        }
        bool operator!=(const RenderKey &o) const { return !(*this == o); }
    };

private:
    std::shared_ptr<SampleSource<std::complex<float>>> iqSource;
    double symbolRateHz = 0.0;
    bool selectionEnabled = false;
    range_t<size_t> selectedRange{0, 0};

    // Off-GUI-thread render pipeline (mirrors TracePlot's float path).
    QFutureWatcher<QImage> *watcher_ = nullptr;
    QImage image_;
    RenderKey imageKey_;
    RenderKey runningKey_;
    RenderKey pendingKey_;
    bool hasImage_ = false;
    bool running_ = false;
    bool havePending_ = false;

    size_t delayForRate() const;
    void startRender(const RenderKey &k);
    void onRenderReady();
};
