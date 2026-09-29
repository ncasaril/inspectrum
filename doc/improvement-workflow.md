# Reliability and responsiveness workflow

All work stays local; no push is part of this workflow.

1. Completed: register regression tests in CTest and CI, covering DSP coordinates, cache
   keys, plugin annotation mapping and extraction cancellation.
2. Completed: stage sample exports and check failures before replacing existing outputs.
3. Completed: share continuous, filtered decimation between raw, SigMF and plugin exports.
4. Completed: add undo/redo and batch plugin annotation imports.
5. Completed: prepare missing spectrum data asynchronously and reject stale results.

Each stage adds focused regression coverage. Validate with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Tests require Qt5 Test and Python 3 with NumPy. Application-only builds may set
`-DBUILD_TESTING=OFF`.

The C++ suite checks cancellation, export rollback, decimation phase and alias
rejection, plugin extraction, undo/save/reload, spectrum parity in both modes,
stale-result rejection, and destruction with queued spectrum work. CTest also
runs the existing end-to-end FSK suite. CI installs NumPy and runs both suites
with failure output enabled.

The CI matrix replaces Ubuntu 20.04 with 24.04 because GitHub
[retired the 20.04 runner](https://github.com/actions/runner-images/issues/11101).
CMake 3.16 or newer uses the supported `FindPython3` module for test discovery.

Export replacement rolls back ordinary I/O failures and preserves existing files
on cancellation. A SigMF pair requires two filesystem renames, so it cannot be
published atomically across a process crash or power loss; retained `.backup-*`
files provide recovery if rollback itself fails. Exporting over the open capture
or its metadata is rejected. Export FIR decimation supports factors 1–65536.

Spectrum requests are coalesced into one worker per spectrogram using the global
thread-pool limit. Each worker receives copied frames and render settings;
copying at most 65536 complex samples and preparing FFT plans still happens on
the GUI thread, outside painting. Spectrum workers do not access live mappings
or widgets. Results from an old render generation are discarded. This change
does not make the main spectrogram's existing synchronous prewarm asynchronous.
