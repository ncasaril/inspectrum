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

#include "frequencydemod.h"
#include <liquid/liquid.h>
#include <liquid/liquid.h>
#include "util.h"

FrequencyDemod::FrequencyDemod(std::shared_ptr<SampleSource<std::complex<float>>> src) : SampleBuffer(src)
{
    // create the demodulator once
    fdem_ = freqdem_create(relativeBandwidth() / 2.0);
}

FrequencyDemod::~FrequencyDemod()
{
    // destroy the demodulator
    freqdem_destroy(fdem_);
}

void FrequencyDemod::work(void *input, void *output, int count, size_t sampleid)
{
    auto in  = static_cast<std::complex<float>*>(input);
    auto out = static_cast<float*>(output);
    // decimation factor: reduce output points to match trace tile width
    size_t decim = (count > 1000) ? size_t(count / 1000) : 1;
    float lastDem = 0.0f;
    // run filter on every sample to maintain state, but only store every decim-th output
    for (int i = 0; i < count; i++) {
        float dem;
        freqdem_demodulate(fdem_, in[i], &dem);
        lastDem = dem;
        if (static_cast<size_t>(i) % decim == 0) {
            out[i] = dem;
        }
    }
    // ensure last sample is output
    if (count > 0 && (size_t(count - 1) % decim) != 0) {
        out[count - 1] = lastDem;
    }
}
