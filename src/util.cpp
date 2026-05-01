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

#include "util.h"
#include <cmath>

std::string formatSIValue(float value)
{
    std::map<int, std::string> prefixes = {
        {  9,   "G" },
        {  6,   "M" },
        {  3,   "k" },
        {  0,   ""  },
        { -3,   "m" },
        { -6,   "µ" },
        { -9,   "n" },
    };

    // Scale by magnitude only; preserve the sign so the result reads
    // e.g. "-445k" rather than "-4.45e+14n" (negative values are always
    // < 1.0 and would otherwise trigger the down-scale loop nine times).
    if (value == 0.0f) {
        std::stringstream ss;
        ss << value;
        return ss.str();
    }
    const float sign = std::signbit(value) ? -1.0f : 1.0f;
    float mag = std::fabs(value);
    int power = 0;
    while (mag < 1.0f && power > -9) {
        mag *= 1e3;
        power -= 3;
    }
    while (mag >= 1e3 && power < 9) {
        mag *= 1e-3;
        power += 3;
    }
    std::stringstream ss;
    ss << (sign * mag) << prefixes[power];
    return ss.str();
}

template<> const char* getFileNameFilter<std::complex<float>>() { return "complex<float> file (*.fc32)"; };
template<> const char* getFileNameFilter<float>() { return "float file (*.f32)"; };
