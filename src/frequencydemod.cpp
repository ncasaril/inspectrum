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
#include <complex>

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

void FrequencyDemod::work(void *input, void *output, int count, size_t /*sampleid*/)
{
    auto in  = static_cast<std::complex<float>*>(input);
    auto out = static_cast<float*>(output);
    if (!cheapMode_) {
        // full FIR-based demod: run filter on every sample
        for (int i = 0; i < count; i++) {
            float dem;
            // interpret std::complex<float> bits as liquid_float_complex
            freqdem_demodulate(fdem_, *reinterpret_cast<liquid_float_complex*>(&in[i]), &dem);
            out[i] = dem;
        }
    } else {
        // cheap instantaneous-frequency demod: phase diff per sample
        if (count > 0) {
            std::complex<float> prev = in[0];
            out[0] = 0.0f;
            for (int i = 1; i < count; i++) {
                float dem = std::arg(in[i] * std::conj(prev));
                prev = in[i];
                out[i] = dem;
            }
        }
    }
}
