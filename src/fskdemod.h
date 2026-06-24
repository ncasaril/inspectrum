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

#include "frequencydemod.h"
#include "samplebuffer.h"

class FskDemod : public SampleBuffer<float, float>
{
public:
    FskDemod(std::shared_ptr<SampleSource<std::complex<float>>> src);
    size_t historySize() override;
    void work(void *input, void *output, int count, size_t sampleid) override;

    void setCheapDemod(bool enabled);
    void setPostLpfCutoff(double hz);
    void setPostLpfMethod(FrequencyDemod::LpfMethod method);
    void setPostDecimation(int n);
    void setPredemodDecimation(int m);
};
