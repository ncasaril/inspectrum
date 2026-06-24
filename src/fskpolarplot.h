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

#include <complex>
#include <memory>

class FskPolarPlot : public Plot
{
public:
    FskPolarPlot(std::shared_ptr<SampleSource<std::complex<float>>> source);
    void paintBack(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void setReferenceCutoff(double hz);
    void setSelection(bool enabled, range_t<size_t> sampleRange);

private:
    std::shared_ptr<SampleSource<std::complex<float>>> iqSource;
    double referenceCutoffHz = 0.0;
    bool selectionEnabled = false;
    range_t<size_t> selectedRange{0, 0};
    size_t delayFor(const QRect &rect, range_t<size_t> sampleRange) const;
};
