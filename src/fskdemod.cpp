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
// Relative squelch: regions whose local deviation falls below this fraction of
// the window's peak deviation are treated as gaps between bursts and faded, so
// the AGC doesn't amplify their noise to full scale. Assumption-free (no
// absolute-Hz threshold) and only bites when there's clear dynamic range.
constexpr float kSquelchFrac = 0.08f;

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
    // Warm BOTH cascaded moving averages. The level MA (kLevelWindow) reads
    // absCentered values that each depend on a fully-ramped centre MA
    // (kCenterWindow), so the first kept sample is only fully warmed after
    // kCenterWindow + kLevelWindow lead-in samples. Discarding that much makes
    // the output independent of where the render window's left edge falls
    // (otherwise the leftmost ~kLevelWindow samples shift with the view).
    return kCenterWindow + kLevelWindow;
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

    // Peak deviation across the KEPT (visible) region, used as the squelch
    // reference. Excluding the leading historySize() lead-in keeps the fade
    // dependent only on what's on screen — otherwise a strong burst just left
    // of the visible edge would raise the floor and shift the fade as you pan.
    const int lead = std::min<int>(count, static_cast<int>(historySize()));
    float peakLevel = 0.0f;
    for (int i = lead; i < count; ++i) {
        if (level[i] > peakLevel)
            peakLevel = level[i];
    }
    if (peakLevel == 0.0f) {
        // Window shorter than the lead-in (or an all-zero kept region): fall
        // back to the full span so there's still a reference.
        for (int i = 0; i < count; ++i)
            if (level[i] > peakLevel)
                peakLevel = level[i];
    }
    const float squelchFloor = peakLevel * kSquelchFrac;

    for (int i = 0; i < count; ++i) {
        const float scale = std::max(level[i] * 2.0f, 1e-5f);
        float v = centered[i] / scale;
        if (!std::isfinite(v))
            v = 0.0f;
        v = std::max(-1.0f, std::min(1.0f, v));
        // Relative squelch: the AGC normalises every region to full scale, so a
        // quiet gap between bursts would otherwise read as full-scale noise.
        // Fade regions whose deviation is far below the window peak (smooth t²
        // ramp so a weak-but-real burst isn't hard-cut).
        if (squelchFloor > 0.0f && level[i] < squelchFloor) {
            const float t = level[i] / squelchFloor;
            v *= t * t;
        }
        out[i] = v;
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
