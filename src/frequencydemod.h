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
#include <liquid/liquid.h>

#include "samplebuffer.h"

class FrequencyDemod : public SampleBuffer<std::complex<float>, float>
{
public:
    FrequencyDemod(std::shared_ptr<SampleSource<std::complex<float>>> src);
    virtual ~FrequencyDemod();
    void work(void *input, void *output, int count, size_t sampleid) override;
private:
    // Liquid-DSP frequency demodulator object
    freqdem      fdem_;
    // if true, use fast instantaneous-frequency demod instead of full FIR
    bool         cheapMode_ = false;
public:
    /**
     * Toggle fast-path demodulation mode (instantaneous phase diff).
     */
    void setCheapDemod(bool enabled) { cheapMode_ = enabled; }
};
