# Bounded memory diagnostics

Run from a desktop terminal as your ordinary user (sudo is requested only for
creating the system service):

```bash
bash tools/debug-memory.sh /absolute/path/to/capture.sigmf-meta
```

An optional second argument selects an inspectrum binary. Prefer a
`RelWithDebInfo` build for source lines and useful stacks; optimized builds may
omit some frames. The launcher uses a unique unit name for each run, prints
the unit and log paths, and runs everything as your user in `system.slice`,
outside GNOME's user-session memory-pressure group.

- Hard service limit: 4 GiB RAM, zero swap, no restart, whole-group OOM kill.
- GDB samples app RSS and service memory every 50 ms. At 3 GiB in either,
  it stops inspectrum, prints memory details and all thread stacks (24 frames
  each), then kills the debuggee. Ordinary crash signals also capture stacks.
- `INSPECTRUM_MEMORY_LOG=1` enables request logs for buffers estimated at 1 MiB
  or more: operation, dynamic source type, Linux thread ID, start sample, sample
  count and estimated bytes. Type names use the compiler's RTTI spelling.
  Estimates overlap between pipeline layers and are **not** live heap totals.
- GDB ignores personal startup files (`--nx`), including custom breakpoints.
- Allocation requests, sampled memory and debugger output go to a private
  `/tmp/inspectrum-memory.XXXXXX/trace.log`, retained after exit. Copy it somewhere
  permanent before reboot/temporary-file cleanup. Logs contain capture paths
  and stacks, but no intentional sample dumps or local-variable dumps.

Follow the printed log with `tail -f /tmp/inspectrum-memory.XXXXXX/trace.log`.
Stop a run with `sudo systemctl stop UNIT`, using its printed unit name.
Inspect the service outcome and peak using:

```bash
systemctl show UNIT -p Result -p MemoryPeak -p MemoryMax -p MemorySwapMax
journalctl -u UNIT
```

This is diagnostic containment, not a memory-growth fix. A fast allocation spike
can still reach the hard cap between polls; SIGKILL cannot produce a backtrace,
but already-flushed request logs survive. External X-server/GPU allocations and
unrelated machine-wide pressure are not contained by this app's memory limit.
No full core dump is generated, avoiding multi-gigabyte dump files.

Test the guard without a capture using `python3 tests/test_memory_guard.py`.
The test needs ptrace permission and uses only tiny debuggees.

## Bounded trace regression

`bounded_trace_regressions` checks chunk-sized reads, exact per-pixel extrema,
FM boundary agreement (unfiltered, FIR, Butterworth and pre-decimation), stale
request cancellation, safe closure and a 28.7-million-sample IQ/AM/FM view.
An optional offscreen test adds FFT rendering and reads a real capture without
modifying it:

```bash
prlimit --as=2147483648 --core=0 /usr/bin/time -v env \
  MALLOC_ARENA_MAX=2 QT_QPA_PLATFORM=offscreen \
  INSPECTRUM_STRESS_CAPTURE=/absolute/path/to/capture.sigmf-meta \
  build/src/trace_tests captureStress
```

The address-space cap also counts the mapped capture and thread reservations;
this command is intended for modest-sized files, not huge mapped captures.
The allocator setting prevents unused per-thread arena reservations consuming
the virtual-address budget. The service-based diagnostic runner remains the
preferred interactive test. Trace source requests are bounded; upstream FM
filters may fetch larger, bounded history/batch windows to preserve filtering.
