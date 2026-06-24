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

#include "fskdemod.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int kCenterWindow = 1024;
constexpr int kLevelWindow = 256;

static void movingAverage(const std::vector<float> &in, std::vector<float> &out, int window)
{
    if (in.empty())
        return;

    double sum = 0.0;
    for (size_t i = 0; i < in.size(); ++i) {
        const float v = std::isfinite(in[i]) ? in[i] : 0.0f;
        sum += v;
        if (i >= static_cast<size_t>(window))
            sum -= std::isfinite(in[i - window]) ? in[i - window] : 0.0f;
        const int n = std::min<int>(static_cast<int>(i) + 1, window);
        out[i] = static_cast<float>(sum / n);
    }
}
}

FskDemod::FskDemod(std::shared_ptr<SampleSource<std::complex<float>>> src)
    : SampleBuffer(std::make_shared<FrequencyDemod>(src))
{
}

size_t FskDemod::historySize()
{
    return kCenterWindow;
}

void FskDemod::work(void *input, void *output, int count, size_t sampleid)
{
    (void)sampleid;

    auto in = static_cast<float*>(input);
    auto out = static_cast<float*>(output);

    if (count <= 0)
        return;

    std::vector<float> centre(count, 0.0f);
    std::vector<float> centered(count, 0.0f);
    std::vector<float> absCentered(count, 0.0f);
    std::vector<float> level(count, 0.0f);

    movingAverage(std::vector<float>(in, in + count), centre, kCenterWindow);

    for (int i = 0; i < count; ++i) {
        const float v = std::isfinite(in[i]) ? in[i] : centre[i];
        centered[i] = v - centre[i];
        absCentered[i] = std::fabs(centered[i]);
    }
    movingAverage(absCentered, level, kLevelWindow);

    for (int i = 0; i < count; ++i) {
        const float scale = std::max(level[i] * 2.0f, 1e-5f);
        float v = centered[i] / scale;
        if (!std::isfinite(v))
            v = 0.0f;
        out[i] = std::max(-1.0f, std::min(1.0f, v));
    }
}

void FskDemod::setCheapDemod(bool enabled)
{
    if (auto fm = std::dynamic_pointer_cast<FrequencyDemod>(src))
        fm->setCheapDemod(enabled);
}

void FskDemod::setPostLpfCutoff(double hz)
{
    if (auto fm = std::dynamic_pointer_cast<FrequencyDemod>(src))
        fm->setPostLpfCutoff(hz);
}

void FskDemod::setPostLpfMethod(FrequencyDemod::LpfMethod method)
{
    if (auto fm = std::dynamic_pointer_cast<FrequencyDemod>(src))
        fm->setPostLpfMethod(method);
}

void FskDemod::setPostDecimation(int n)
{
    if (auto fm = std::dynamic_pointer_cast<FrequencyDemod>(src))
        fm->setPostDecimation(n);
}

void FskDemod::setPredemodDecimation(int m)
{
    if (auto fm = std::dynamic_pointer_cast<FrequencyDemod>(src))
        fm->setPredemodDecimation(m);
}
