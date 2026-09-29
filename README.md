# inspectrum
inspectrum is a tool for analysing captured signals, primarily from software-defined radio receivers.

![inspectrum screenshot](/screenshot.jpg)

## Features
 * Large (100GB+) file support
 * Spectrogram with zoom/pan
 * Cursor status: absolute frequency (when a capture center is set), frequency offset, and spectrogram-bin power
 * Plots of amplitude, frequency, phase and IQ samples
 * Cursors for measuring period, symbol rate and extracting symbols
 * Export of selected time period, filtered samples and demodulated data
 * External AI integration via MCP: shared signal analysis and undoable annotation proposals

Open **Tools → AI review** to review proposals from your external AI session. See
[AI integration](doc/ai-integration.md) for setup, privacy, external MCP clients
and the testing workflow.

The cursor's power readout uses the spectrogram's relative, windowed FFT-bin
power before color scaling (including reassignment when enabled). It is not
calibrated dBm or total channel power; FFT/window settings affect the reading.

Spectrum side views have a **Detect peaks** checkbox and a table below the trace.
Numbered markers match table rows: absolute frequency, center-frequency offset,
uncalibrated FFT-bin level, level relative to the reference peak, and frequency
ratio. The strongest peak is the initial reference; click another row to use it.
Choose **Offset ratios** for baseband harmonics or **RF ratios** for absolute
frequencies. Ratios are undefined for a zero-frequency reference, and integer
ratios alone do not establish a harmonic relationship.

Detection uses the current column, not persistence overlays, and reports up to
16 narrow local maxima separated by at least three FFT bins. Threshold controls
height above the median and local prominence within four bins on each side;
the range control excludes bins too far below the strongest. Frequencies are
FFT-bin centers without interpolation. Broad peaks, unresolved tones and band
edges may not be detected; window sidelobes can be mistaken for weak signals.

Hover over or drag the FFT, zoom, power-range and reassignment-floor sliders to
see their effective values. FFT tooltips also show bin spacing and window duration
when a sample rate is set; zoom shows the actual samples per column.

## Install
### Linux
Install inspectrum with your package manager, it should be present in most distros.

### macOS
 * [Homebrew](https://formulae.brew.sh/formula/inspectrum)
 * [MacPorts](https://ports.macports.org/port/inspectrum/)

## Windows
 * [radioconda](https://github.com/ryanvolz/radioconda)
 * [conda](https://anaconda.org/conda-forge/inspectrum)

## Build from source
### Prerequisites

 * cmake >= 3.16
 * fftw 3.x
 * [liquid-dsp](https://github.com/jgaeddert/liquid-dsp) >= v1.3.0
 * pkg-config
 * qt5

Tests additionally require Qt5 Test and Python 3 with NumPy. To build only the
application, configure with `-DBUILD_TESTING=OFF`. See the
[improvement workflow](doc/improvement-workflow.md) for build and test commands.

### Build instructions

Build instructions can be found here: https://github.com/miek/inspectrum/wiki/Build

### Run

    ./inspectrum [filename]

## Input
inspectrum supports the following file types:
 * `*.sigmf-meta, *.sigmf-data` - SigMF recordings
 * `*.cf32`, `*.fc32`, `*.cfile` - Complex 32-bit floating point samples (GNU Radio, osmocom_fft)
 * `*.cf64`, `*.fc64` - Complex 64-bit floating point samples
 * `*.cs32`, `*.sc32`, `*.c32` - Complex 32-bit signed integer samples (SDRAngel)
 * `*.cs16`, `*.sc16`, `*.c16` - Complex 16-bit signed integer samples (BladeRF)
 * `*.cs8`, `*.sc8`, `*.c8` - Complex 8-bit signed integer samples (HackRF)
 * `*.cu8`, `*.uc8` - Complex 8-bit unsigned integer samples (RTL-SDR)
 * `*.f32` - Real 32-bit floating point samples
 * `*.f64` - Real 64-bit floating point samples (MATLAB)
 * `*.s16` - Real 16-bit signed integer samples
 * `*.s8` - Real 8-bit signed integer samples
 * `*.u8` - Real 8-bit unsigned integer samples

If an unknown file extension is loaded, inspectrum will default to `*.cf32`.

Note: 64-bit samples will be truncated to 32-bit before processing, as inspectrum only supports 32-bit internally.
