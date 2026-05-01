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

#include "tunertransform.h"
#include <liquid/liquid.h>
#include <QMutexLocker>
#include <cmath>
#include "util.h"

TunerTransform::TunerTransform(std::shared_ptr<SampleSource<std::complex<float>>> src) : SampleBuffer(src), frequency(0), bandwidth(1.), taps{1.0f}
{

}

void TunerTransform::work(void *input, void *output, int count, size_t sampleid)
{
    auto out = static_cast<std::complex<float>*>(output);
    auto temp = std::make_unique<std::complex<float>[]>(count);

    // Snapshot parameters under the short-hold paramMutex_ and drop it
    // before the heavy NCO+FIR loop. The base class's `mutex` is also held
    // here (by SampleBuffer::getSamples), but the GUI-thread setters use
    // paramMutex_ instead, so they aren't blocked by this work() running.
    float freqLocal;
    std::vector<float> tapsLocal;
    {
        QMutexLocker ml(&paramMutex_);
        freqLocal = frequency;
        tapsLocal = taps;
    }

    // Mix down
    nco_crcf mix = nco_crcf_create(LIQUID_NCO);
    nco_crcf_set_phase(mix, fmodf(freqLocal * sampleid, Tau));
    nco_crcf_set_frequency(mix, freqLocal);
    nco_crcf_mix_block_down(mix,
                            static_cast<std::complex<float>*>(input),
                            temp.get(),
                            count);
    nco_crcf_destroy(mix);

    // Filter
    firfilt_crcf filter = firfilt_crcf_create(tapsLocal.data(), tapsLocal.size());
    for (int i = 0; i < count; i++)
    {
        firfilt_crcf_push(filter, temp[i]);
        firfilt_crcf_execute(filter, &out[i]);
    }
    firfilt_crcf_destroy(filter);
}

void TunerTransform::setFrequency(float frequency)
{
    QMutexLocker ml(&paramMutex_);
    this->frequency = frequency;
}

void TunerTransform::setTaps(std::vector<float> taps)
{
    QMutexLocker ml(&paramMutex_);
    this->taps = std::move(taps);
}

float TunerTransform::relativeBandwidth() {
    QMutexLocker ml(&paramMutex_);
    return bandwidth;
}

void TunerTransform::setRelativeBandwith(float bandwidth)
{
    QMutexLocker ml(&paramMutex_);
    this->bandwidth = bandwidth;
}

size_t TunerTransform::historySize()
{
    QMutexLocker ml(&paramMutex_);
    return std::max(static_cast<size_t>(256), taps.size());
}

