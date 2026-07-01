# Plugin API — implementation plan

Status: **planning** (not yet implemented). Target branch: `main` (fork `ncasaril/inspectrum`).

## Goal

Let inspectrum hand a **filtered/tuned section** of a recording to an **external
process**, which analyses it and returns **annotations** (e.g. TETRA calls, sync
bursts, energy segments). The user reviews the returned annotations in the normal
annotation UI and saves them back to SigMF like any hand-drawn annotation.

The plugin runs over the same signal the derived plots see — the `TunerTransform`
output (mixed to baseband, FIR band-pass applied) — **not** the raw wideband file.

## Decisions (locked)

| Question | Decision |
|----------|----------|
| Region the plugin runs over | **Offer at run time**: cursor-selection / current-view / whole tuned output |
| Sample handoff | **Temp SigMF file** (`cf32_le` `.sigmf-data` + `.sigmf-meta`) in the session tmp dir, deleted after |
| Annotation output schema | **IQEngine-compatible JSON** (SigMF annotation keys), segment-local sample indices |

## Data flow

```
InputSource ─► TunerTransform (mix to baseband + FIR band-pass)   ← "the filtered section"
                     │
            choose window: selection | view | whole
                     │
            extract [segStart, segCount] of TunerTransform output
                     │
            write seg.sigmf-data (cf32_le) + seg.sigmf-meta (tmp)
                     │
   QProcess:  plugin <seg.sigmf-meta>  < context.json  > annotations.json
                     │
            parse IQEngine annotations (segment-local indices)
                     │
            map segment coords ─► original-file coords
                     │
            inputSrc->addAnnotation() × N   → user reviews / edits / saves
```

## Wire contract

### Manifest — `~/.config/inspectrum/plugins/*.json`
```json
{
  "name": "TETRA burst detector",
  "exec": "/usr/local/bin/inspectrum-tetra",
  "args": ["--mode", "calls"],
  "sample_type": "cf32",
  "params": [
    { "key": "threshold_db", "type": "float", "default": -30, "label": "Threshold (dB)" }
  ]
}
```
- Discovered at startup; each manifest becomes an entry under **Tools → Run plugin ▸**
  and a context-menu item.
- `params` auto-generates a small modal dialog (float / int / bool / string / enum).
- `sample_type` gates which plugins are offered (only `cf32` for now; the tuned
  output is always complex).
- Invalid / unreadable manifests are skipped with a logged warning, never fatal.

### Invocation
```
<exec> [args...] <seg.sigmf-meta>     stdin = context.json     stdout = annotations.json
```

`context.json` (IQEngine field names):
```json
{
  "sample_rate": 384000,
  "center_freq": 391012500,
  "custom_params": { "threshold_db": -30 }
}
```

stdout response (IQEngine annotation shape; **sample indices relative to the segment**):
```json
{ "annotations": [
  { "core:sample_start": 12000, "core:sample_count": 4096,
    "core:freq_lower_edge": 391000000, "core:freq_upper_edge": 391025000,
    "core:label": "call", "core:comment": "voice burst" }
]}
```
- `core:sample_start` + `core:sample_count` are **required** on each annotation
  (IQEngine validates them). `core:freq_lower_edge`/`core:freq_upper_edge` are optional
  but SigMF requires **both or neither** — inspectrum fills both from the live tuner
  pass-band when omitted. IQEngine annotations carry `core:label`/`core:comment` (plus
  `core:generator`/`core:uuid`) and have **no `core:description`** — inspectrum maps
  `core:label→label`, `core:comment→comment`, and also accepts a non-canonical
  `core:description` if a plugin emits one (`extra=allow` lets it pass through).
- This is the IQEngine *annotation schema carried over a CLI*, **not** the
  IQEngine HTTP service API. A plugin can share its detection core with a real
  IQEngine plugin — that portability is the reason for the schema choice.
- Anything the plugin writes to **stderr** is captured and surfaced in an error
  dialog. Non-zero exit ⇒ no annotations added, error shown.

## Coordinate mapping (RESOLVED — no longer the risky bit)

Both unknowns the original draft flagged are now settled by reading the live code:

- **`TunerTransform` is strictly 1:1** — `work()` mixes (NCO) then FIR-filters in
  place, the loop emits exactly `count` outputs per `count` inputs, and `count()`/
  `rate()` are inherited unchanged from `SampleBuffer` (proof:
  `tunertransform.cpp:50-67,131-138`; a `// A future resampler … would have to change
  this` comment confirms none exists). **So `orig_sample = segStart + local_sample`,
  with no `*M` factor.** The segment we extract is the tuned/filtered IQ at full `Fs`,
  mixed so the tuner band sits at DC.
- **`Annotation::frequencyRange` is ABSOLUTE Hz** (load stores `core:freq_lower/upper
  _edge` verbatim, centre is subtracted only at paint time —
  `inputsource.cpp:541-543`, `spectrogramplot.cpp:310-313`). `sampleRange` is an
  **absolute, inclusive `[min,max]`** sample index; on disk `core:sample_count =
  max-min+1` (`inputsource.cpp:1001-1014`).

Resulting maps applied to each returned annotation:
- Time: `absStart = segStart + core:sample_start`; `sampleRange = {absStart,
  absStart + core:sample_count - 1}` (inclusive).
- Frequency: segment meta `captures[0].core:frequency = inputFreq + tunerOffsetHz()`
  (absolute tuned centre). The plugin emits **absolute** `freq_lower/upper_edge`,
  stored straight into `frequencyRange`. If the plugin **omits** freq edges (allowed —
  time-only detection), inspectrum fills them from the live **tuner pass-band**
  (`centre ± 0.5·tunerBandwidthHz()`).

The parse+map is still factored into a free function `parsePluginAnnotations()` and
**unit-tested by a standalone harness** (segStart offset, inclusive-max, freq
passthrough, `#RRGGBBAA→#AARRGGBB` colour rotation, pass-band fallback), plus a
segment round-trip test (`writeSegmentSigmf()` → reopen with `InputSource` → assert
count/rate/centre).

## UI surface

The app currently has **no menu bar** (all controls live in the `SpectrogramControls`
dock; file-open is a dock signal). So integration is two-pronged, both routing through
one `PlotView::runPlugin(const PluginManifest&)`:

- **Primary: right-click context menu** on the spectrogram — a **Run plugin ▸ <name>**
  submenu added next to the existing "Add annotation here" / "Extract symbols" /
  "Export samples…" actions (`plotview.cpp` `contextMenuEvent`, ~`:763`). This is where
  all the scope state already lives (`selectedSamples`, `viewRange`, `tunerOffsetHz()`,
  `tunerBandwidthHz()`).
- **Secondary: a `Tools` menu bar** created in `MainWindow` (the first menu in the app,
  via `menuBar()->addMenu`), with the same per-plugin actions + **Reload plugins**.
  Rebuilt on each `openFile` (cleared, not appended) at the `dock->setFileInfo` hook.
- On launch: scope dialog (Selection / View / Whole — Selection disabled when
  `!cursorsEnabled`; mirrors the export dialog's three-way pattern at
  `plotview.cpp:1417-1427`), then the auto-generated param dialog if the manifest
  declares `params`.
- Async run: segment extraction runs on a **`QtConcurrent`** worker (with a
  `QFutureWatcher` + an atomic cancel flag), and the **`QProcess`** it feeds is itself
  async, so the GUI event loop stays live throughout — the busy `QProgressDialog` +
  **Cancel** aborts both the extraction and the process, backed by a timeout `QTimer`.
  A single-flight guard refuses overlapping runs; large scopes are size-warned.
- Returned annotations are added via the existing `addAnnotation()` path, so they are
  immediately editable (drag handles), dirty-tracked, and savable to SigMF / archive.
  Default box colour for auto-detected annotations is a distinct cyan so they read as
  machine-generated until the user edits them.

## Files

| File | Change |
|------|--------|
| `src/plugin.h` / `src/plugin.cpp` | **new** — manifest model + loader, segment extract → temp SigMF writer, `QProcess` runner (async, timeout, stderr→dialog, cancel), response parse + coordinate map |
| `src/plotview.cpp` / `.h` | scope dialog, menu/context wiring, `addAnnotation()` of mapped results |
| `src/mainwindow.cpp` | build **Tools → Run plugin** menu from discovered manifests |
| `src/spectrogramcontrols.*` | (optional) a "Plugins…" affordance / rescan; not required for v1 |
| `src/CMakeLists.txt` | append `plugin.cpp` to `inspectrum_sources` |
| `doc/plugins.md` | **new** — manifest + wire-protocol reference for plugin authors |
| `examples/plugins/energy-detect.py` | **new** — reference plugin (energy-gate call detector) + smoke test |

## Build stages (each committed on `main`)

1. **Handoff + mapping core (headless, tested).**
   `Plugin` class: extract window from `TunerTransform` output, write temp SigMF
   segment, run a stub `QProcess`, parse response, map coords. Standalone harness
   verifies the segment round-trips (sample count, rate, centre) and the inverse
   coordinate map (incl. decimation) against known inputs. No GUI yet.
2. **Manifest discovery + menu.**
   Load `~/.config/inspectrum/plugins/*.json`, build the Tools menu + context entry,
   param dialog generation.
3. **Run UX.**
   Scope dialog, async run with cancel/timeout, stderr error dialog, add returned
   annotations through `addAnnotation()`.
4. **Docs + reference plugin.**
   `doc/plugins.md` and the Python energy-gate detector; end-to-end smoke test
   (open file → run plugin → annotations appear → save → reopen).

## Edge cases / risks

- **Large selections at full rate.** A wide whole-file selection at `Fs` can produce
  a big temp file. Mitigate: warn above a size threshold; consider optional
  extract-time decimation (which then activates the `M` map above).
- **Plugin never exits / hangs.** Timeout + Cancel both kill the process; partial
  stdout is discarded.
- **Malformed plugin output.** Strict JSON parse; on failure, show stderr + a parse
  error, add nothing.
- **Annotation count explosion.** A noisy detector could return thousands; cap with a
  warning ("plugin returned N; add all / cancel?").
- **Frequency-edge omission.** If the plugin omits freq edges, default the annotation
  to the tuner pass-band.
- **Security.** Plugins are arbitrary local executables the user installed — same
  trust level as any CLI tool they run; no sandboxing in v1. Documented as such.

## Out of scope for v1

- IQEngine HTTP service plugins (remote/containerised) — file-CLI only for now.
- Sync-word / correlation detection beyond what a plugin author implements
  themselves (inspectrum just provides the samples + collects annotations).
- Streaming/incremental results — plugin runs to completion, then we ingest.
