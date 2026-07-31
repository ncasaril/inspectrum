# inspectrum analysis plugins

inspectrum can hand a **filtered section** of a recording to an **external process**
that analyses it and returns **annotations** (bursts, calls, sync words, …). The
returned annotations appear in the normal annotation UI — editable (drag the handles),
dirty-tracked, and savable back to SigMF (sidecar or tar+zstd archive) like any
hand-drawn annotation.

The signal handed to a plugin is the **tuned/filtered IQ** the derived plots see (the
spectrogram's `TunerTransform` output when the tuner is on, otherwise the raw input),
extracted over the region you choose. It is mixed to baseband — the tuner centre sits
at 0 Hz — and carried at the file's full sample rate.

## Installing a plugin

Drop a manifest JSON in:

```
~/.config/inspectrum/plugins/*.json
```

It then appears under **Tools → Run plugin ▸ <name>** and in the spectrogram's
right-click **Run plugin ▸** submenu. Use **Tools → Reload plugins** after adding or
editing a manifest (the right-click menu rediscovers automatically).

Try the bundled reference plugins (python3 + numpy):

```sh
mkdir -p ~/.config/inspectrum/plugins
cp examples/plugins/energy-detect.json examples/plugins/fsk-analyze.json ~/.config/inspectrum/plugins/
# edit "exec" in each copy to the absolute path of the matching .py under examples/plugins/
chmod +x examples/plugins/energy-detect.py examples/plugins/fsk-analyze.py
```

## Manifest format

```json
{
  "name": "Energy burst detector",
  "exec": "/usr/local/bin/inspectrum-energy-detect",
  "args": ["--mode", "calls"],
  "sample_type": "cf32",
  "wants_band": true,
  "params": [
    { "key": "threshold_db", "type": "float", "label": "Threshold (dB)", "default": -10 }
  ]
}
```

| field         | meaning |
|---------------|---------|
| `name`        | menu label (required) |
| `exec`        | executable: absolute path or PATH-resolvable (required) |
| `args`        | fixed args prepended before the meta-file path (optional) |
| `sample_type` | accepted input type; only `cf32` is offered today (default `cf32`) |
| `wants_band`  | drag a box on the spectrogram to pick the band + time before running (default `false`) |
| `long_running` | disable the run timeout; the plugin runs until it exits or you cancel (default `false`) |
| `params`      | parameters surfaced as a dialog before each run (optional) |

Set `"wants_band": true` for a band-sensitive plugin. Running it then arms a
**drag-a-box** mode on the spectrogram: the box you drag sets the band from its
**vertical** extent (centre frequency + bandwidth) and the region from its **horizontal**
extent (time), in one gesture — Esc or right-click cancels. inspectrum points the tuner at
that band and hands the plugin exactly that slice, mixed to baseband and filtered to the
box width. Because the box already bounds the bandwidth, the slice is **decimated** to fit
it (largest power-of-2 decimation keeping the rate at least 2x the box width), so it is
*not* at the file's full rate — read `sample_rate` from the context JSON rather than
assuming the host rate, and note that annotation indices are in those decimated samples.
The box frequency edges also become the default
`core:freq_lower_edge`/`upper_edge` for annotations that don't set their own. Plugins that
don't care about the band omit the flag and receive the current tuner output (or the raw
input when the tuner is off) over the region chosen in the usual dialog, as before.

Each `params` entry: `key` (the JSON key passed to the plugin), `type` (`float` `int`
`bool` `string` `enum`), `label` (dialog text, defaults to `key`), `default`, and
`choices` (string list, for `enum`). For `int`/`float`, optional `min`/`max` set the
spin-box bounds and `decimals` the float precision — declare them when a value would
otherwise be clamped or rounded by the default range (±1e9 int, ±1e12 / 6-decimal
float). Duplicate `key`s are rejected (the later one is dropped with a warning).

## Wire protocol

inspectrum extracts the chosen region, writes a temporary SigMF segment, and invokes:

```
<exec> [args...] <segment.sigmf-meta>      # stdin = context.json, stdout = annotations.json
```

- **argv**: the fixed `args`, then the path to a freshly written
  `segment.sigmf-meta`. Its `segment.sigmf-data` sibling is `cf32_le` (interleaved
  little-endian float32 I,Q), with `core:sample_rate` and `captures[0].core:frequency`
  (the absolute tuned centre, Hz) in the meta.
- **stdin** (`context.json`):
  ```json
  { "sample_rate": 384000, "center_freq": 391012500, "custom_params": { "threshold_db": -10 } }
  ```
  `custom_params` holds the values entered in the param dialog, keyed by `key`.
- **stdout** (`annotations.json`):
  ```json
  { "annotations": [
    { "core:sample_start": 12000, "core:sample_count": 4096,
      "core:freq_lower_edge": 391000000, "core:freq_upper_edge": 391025000,
      "core:label": "call", "core:comment": "voice burst" }
  ]}
  ```

### Annotation fields

- `core:sample_start`, `core:sample_count` — **required**, integers, **segment-local**
  (relative to sample 0 of the extracted segment, in the units of the segment you were
  handed — i.e. decimated samples when the segment was decimated). inspectrum maps them
  to absolute file indices: `abs = segStart + core:sample_start * decim`.
- `core:freq_lower_edge`, `core:freq_upper_edge` — optional, **absolute Hz**. SigMF
  requires both or neither. If omitted, inspectrum fills both from the tuner pass-band
  (or, when the tuner is off, the full input band).
- `core:label`, `core:comment` — optional text (`core:label` <= ~20 chars by SigMF
  convention; `core:comment` is the longer note). Both are shown/editable.
- `core:description` — optional longer free-form text, mapped to the annotation's
  description field (distinct from `core:label`/`core:comment`).
- `core:generator`, `core:uuid` — optional; accepted but currently ignored (not
  stored on the annotation, so not re-emitted on save).
- `presentation:color` — optional `"#RRGGBBAA"`; otherwise a default cyan marks the
  annotation as machine-generated until you edit it.

This is the IQEngine plugin **annotation schema carried over a CLI** — not the IQEngine
HTTP service API — so a plugin's detection core can be shared with a real IQEngine
plugin. Unknown extra keys are ignored.

### Errors and lifecycle

- A **non-zero exit** or **crash** ⇒ no annotations added; the plugin's **stderr** is
  shown in an error dialog.
- Malformed stdout JSON ⇒ error shown, nothing added. Annotation entries missing
  `core:sample_start`/`core:sample_count` are skipped (the rest still apply).
- Runs are **asynchronous** with a busy dialog + **Cancel** (kills the process) and a
  timeout (disabled by `"long_running": true`). Only one plugin runs at a time.

### Progress reporting

A plugin can update the busy dialog's label while it works. Write a line to **stderr**
beginning with the **RS byte `0x1E`**; the rest of that line replaces the dialog text and
is *not* treated as error output. Every other stderr line is collected as normal (and
shown if the run fails). Lines must be newline-terminated to be recognised.

```python
import sys
def progress(msg):
    sys.stderr.write("\x1e" + msg + "\n")
    sys.stderr.flush()

progress("analysing burst 3/17")
```

This pairs well with `long_running`: a plugin with no timeout should report progress so
the dialog does not look hung.

## Security

Plugins are arbitrary local executables **you** install — same trust level as any CLI
tool you run. There is no sandboxing. Only install manifests pointing at code you trust.

## Writing a plugin

See `examples/plugins/energy-detect.py` for a complete, ~150-line reference: it reads
the meta path from `argv[-1]` (the last argument, after any fixed `args`),
`custom_params` from stdin, loads the `cf32` data with
numpy, energy-gates against the segment peak, and emits one annotation per detected
burst (omitting freq edges so inspectrum uses the pass-band / full input band). Any language works — the
contract is just argv + stdin + stdout JSON.

### Bundled: FSK/MSK analyser

`examples/plugins/fsk-analyze.py` is a fuller example: a 2FSK/MSK burst analyser.
Each energy-gated burst is band-limited to its occupied spectrum (block-PSD
estimate + brick-wall filter, so wideband noise can't swamp the discriminator;
signals occupying most of Nyquist are upsampled internally so low-oversampling
captures down to ~3 samples/symbol still analyse), then analysed for the tone pair (shift and carrier
offset), the symbol rate (from discriminator zero-crossing intervals), the
modulation index (h = tone separation / Rb; h ≈ 0.5 is labelled **MSK**,
otherwise **2FSK**) and the data bits — decoded run-length between crossings,
so timing can't drift over long bursts. Results land in the label
(`MSK 4.8kBd`) and a one-stat-per-line comment, e.g.

```
fsk-analyze:
  Rb=4800.6 Bd (h=0.50)
  shift ±1188.2 Hz
  offset -2 Hz
  bits[240]=0xA53C…
```

The decoded bits are shown as **MSB-first hex** (first decoded bit = top bit;
a trailing partial nibble is right-padded), truncated to the `max_bits` param
and to a global ~2M-character budget across all annotations. Bursts with a recovered rate also demonstrate emitting
absolute `core:freq_lower_edge`/`upper_edge` (a Carson-rule band around the
tones) and `presentation:color`. Unmodulated bursts are labelled `carrier` and
a tone pair with no transitions to derive a rate from is labelled `FSK`
(`Rb n/a`) — both omit the freq edges so inspectrum fills the pass-band.
Gated regions with no spectral peak 10 dB above the floor (or shorter than 32
samples) are treated as noise and skipped. Gaussian pulse shaping (GFSK) reads
slightly low on shift/h even after interior-based refinement — very soft
shaping (BT ≤ 0.3) may be labelled 2FSK with h ≈ 0.4 — and strong in-band
spurs corrupt the estimates, so tune onto the signal first in crowded spectrum.
The symbol rate is accurate to a few percent on typical data but degrades on
payloads dominated by long same-symbol runs (heavy bit imbalance, or
repetitive framing with few single-symbol transitions); the tones, deviation,
modulation type and bits stay usable there, so treat `Rb` as approximate. A
synthetic regression suite covering these cases lives alongside the plugin in
`examples/plugins/test_fsk_analyze.py` (`python3 examples/plugins/test_fsk_analyze.py`,
needs numpy).

`custom_params`:

| key               | meaning |
|-------------------|---------|
| `threshold_db`    | burst gate relative to the segment peak (float, default -15) |
| `min_duration_ms` | drop bursts shorter than this (float, default 0.5) |
| `merge_gap_ms`    | merge bursts separated by less than this (float, default 0.5) |
| `symbol_rate_hz`  | force the symbol rate; 0 = estimate per burst (float, default 0) |
| `max_bits`        | bits of decoded data shown as hex in the comment (int, default 96) |

### Bundled: audio playback (WFM/NFM/AM)

`examples/plugins/audio-play.py` demonstrates a plugin whose output is *sound*,
not just annotations. It sets `"wants_band": true`, so you **drag a box over a
station** on the spectrogram; inspectrum tunes to that band, mixes it to
baseband and hands the plugin the filtered IQ, which it demodulates and plays
through the system's audio device (`aplay` / `pw-play` / `paplay`, whichever
works first). The box's horizontal extent is how much you hear — drag a short
span for a quick listen.

Modes (the `mode` param): **WFM** (broadcast FM, 15 kHz audio, 75 µs
de-emphasis), **NFM** (narrowband voice, 4 kHz audio, 50 µs, optional squelch),
**AM** (envelope detector). It resamples the demodulated audio to 48 kHz,
DC-blocks the carrier offset, peak-normalises, and returns one annotation
covering the played region with a one-stat-per-line comment (mode, duration,
measured peak deviation).

The manifest sets `"long_running": true`, so no watchdog timeout applies: with
`loop` on (the default) the run keeps replaying until you hit **Cancel** in the
busy dialog, which kills this process and the player child with it. `max_seconds`
(default 30) caps the span played *per pass*, so a long box stays manageable
rather than committing you to one very long playback. The plugin reports
`Preparing audio` and `Playing (loop N)...` through the
[progress protocol](#progress-reporting), so you can tell the two phases apart
even when a passage is silent. Needs `python3` + `numpy` + `scipy` and any of the
players above.

| key             | meaning |
|-----------------|---------|
| `mode`          | `WFM` \| `NFM` \| `AM` (default `WFM`) |
| `loop`          | replay until the run is cancelled (bool, default `true`) |
| `deemphasis_us` | FM de-emphasis time constant; `auto` picks 75/50 by mode, `0` disables |
| `audio_lpf_hz`  | audio low-pass cutoff (float, 0 = mode default) |
| `gain_db`       | extra gain after peak-normalisation (float, default 0) |
| `squelch_db`    | mute audio below this many dB under the segment peak (float, 0 = off) |
| `max_seconds`   | cap the played duration (float, default 30) |
