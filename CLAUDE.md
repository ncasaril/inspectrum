# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

inspectrum is a Qt5 desktop application for analysing captured signals from software-defined radio receivers. Large (100GB+) raw IQ files are memory-mapped and rendered into a spectrogram plus a stack of derived plots (amplitude, frequency, phase, IQ, threshold). This fork (`ncasaril/*`) adds performance knobs on top of upstream `miek/inspectrum`: a fast-path FM demod, configurable derived-plot height, and multi-threaded tile rendering.

## Build and run

```bash
# Out-of-tree build (build/ already exists in this checkout)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/src/inspectrum [file]

# Run tests (CI runs ctest, but there are currently no registered tests)
ctest --test-dir build
```

Dependencies: cmake ≥ 3.1, Qt5 (Widgets + Concurrent), FFTW 3.x, liquid-dsp ≥ 1.3.0, pkg-config. On Ubuntu: `libfftw3-dev libliquid-dev qtbase5-dev`. CI (`.github/workflows/build.yml`) builds against macOS and Ubuntu 20.04/22.04.

Default compile flags when unset: `-O2 -ggdb -march=native`, C++14, `AUTOMOC` on. All source lives under `src/`; adding a `.cpp` requires appending it to `inspectrum_sources` in `src/CMakeLists.txt`.

## Architecture

### Sample-source pipeline (the spine of the app)

Every data flow in inspectrum is a chain of `SampleSource<T>` objects. Downstream nodes pull samples on demand via `getSamples(start, length)` — there is no push/streaming path.

- `AbstractSampleSource` (`abstractsamplesource.h`) — type-erased base; carries the subscriber set and the runtime `sampleType()` (a `std::type_index`) used for plot compatibility.
- `SampleSource<T>` (`samplesource.h`) — templated node producing `T` (typically `std::complex<float>` or `float`). Also owns the per-node `annotationList` and `frequency`.
- `SampleBuffer<Tin, Tout>` (`samplebuffer.h`) — bridge that pulls `Tin` from an upstream source, calls a subclass-defined `work()` to transform to `Tout`, and exposes the result as a `SampleSource<Tout>`. Almost every derived plot sits on top of a `SampleBuffer`.
- Leaves/transforms: `InputSource` (mmap'd file → `complex<float>`), `TunerTransform` (mix + FIR bandpass via liquid-dsp), `FrequencyDemod` / `AmplitudeDemod` / `PhaseDemod` (complex → float), `Threshold` (float → float).

Invalidation uses the `Subscriber` interface (`subscriber.h`): when an upstream source changes (new file, tuner moved, demod parameter toggled) it calls `invalidate()`, which fans out `invalidateEvent()` to every subscriber. Plots react by dropping cached tiles and re-requesting samples. Keep this contract intact when adding new transforms — forgetting to forward `invalidateEvent` produces stale tiles that only refresh on scroll.

### Plot layer

- `Plot` (`plot.h`) is a `QObject` that wraps one `AbstractSampleSource` and exposes `paintBack` / `paintMid` / `paintFront` hooks. Its `output()` may differ from its input (e.g. `SpectrogramPlot` returns the `TunerTransform` output so downstream derived plots see the tuned/filtered signal).
- `SpectrogramPlot` is the top plot; it owns the `FFT`, a colormap, a pixmap tile cache keyed by `(fftSize, zoomLevel, nfftSkip, sample)`, and the `Tuner` + `TunerTransform`. Tile size is fixed at 65536 samples (must be a multiple of max FFT size).
- `TracePlot` is the base for all derived scalar plots. It uses a debounced tile scheduler, `QtConcurrent` background workers (`QFutureWatcher`), and a global-min/max background pass for float sources. **Recent change**: `paintMid` now splits each visible region into N tiles (N = thread count) so rendering scales with cores.
- `Plots` (`plots.h`) is a static registry mapping a sample `type_index` to the list of plots that can consume it. This is how the right-click "add derived plot" menu is populated — to add a new plot type, register it here and it will automatically be offered wherever the sample type matches.
- `PlotView` (`plotview.h`) is the `QGraphicsView` that stacks plots vertically, owns cursors, the input source, and funnels mouse/wheel events. Scroll position and zoom are translated to a sample range that every plot paints against.

### UI

`MainWindow` hosts the `PlotView` and the `SpectrogramControls` dock widget. Controls emit signals that land on `PlotView` / `SpectrogramPlot` slots — there is no central controller. New UI knobs typically need: a widget in `SpectrogramControls`, a `signal` on the dock, a `slot` on `PlotView` (or the relevant plot), and wiring in `MainWindow`.

### File formats

`InputSource` mmaps the file and uses a `SampleAdapter` subclass to convert on the fly to `complex<float>`. Extension → adapter mapping lives in `inputsource.cpp`; SigMF `.sigmf-meta` / `.sigmf-data` pairs are parsed via `readMetaData()` and populate `annotationList`. Unknown extensions default to `cf32`. Everything internal is 32-bit — 64-bit inputs are truncated on read.

## Conventions

- **Match upstream style.** This is a fork of `miek/inspectrum`; keep header guards as `#pragma once`, GPL-3 license blocks at the top of new files, and two-space indent to match existing code.
- **Derived-plot additions** go through the `Plots` registry, not ad-hoc wiring in `PlotView`.
- **Heavy per-tile work belongs off the GUI thread.** Use `QtConcurrent::run` + `QFutureWatcher` like `TracePlot` does, and respect the thread-count setting plumbed through `PlotView::setMaxThreads`.
- **Cache keys must include every parameter that affects output** (see `TileCacheKey` for the spectrogram pattern). Forgetting one produces subtle stale-tile bugs that only surface when the user toggles that parameter.
