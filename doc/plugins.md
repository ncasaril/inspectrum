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

Try the bundled reference detector:

```sh
mkdir -p ~/.config/inspectrum/plugins
cp examples/plugins/energy-detect.json ~/.config/inspectrum/plugins/
# edit "exec" in that copy to the absolute path of examples/plugins/energy-detect.py
chmod +x examples/plugins/energy-detect.py    # needs python3 + numpy
```

## Manifest format

```json
{
  "name": "Energy burst detector",
  "exec": "/usr/local/bin/inspectrum-energy-detect",
  "args": ["--mode", "calls"],
  "sample_type": "cf32",
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
| `params`      | parameters surfaced as a dialog before each run (optional) |

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
  (relative to sample 0 of the extracted segment). inspectrum maps them to absolute
  file indices: `abs = segStart + core:sample_start`.
- `core:freq_lower_edge`, `core:freq_upper_edge` — optional, **absolute Hz**. SigMF
  requires both or neither. If omitted, inspectrum fills both from the tuner pass-band.
- `core:label`, `core:comment` — optional text. (`core:label` ≤ ~20 chars by SigMF
  convention; `core:comment` is the longer note.)
- `core:generator`, `core:uuid` — optional, passed through.
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
  timeout. Only one plugin runs at a time.

## Security

Plugins are arbitrary local executables **you** install — same trust level as any CLI
tool you run. There is no sandboxing. Only install manifests pointing at code you trust.

## Writing a plugin

See `examples/plugins/energy-detect.py` for a complete, ~150-line reference: it reads
the meta path from `argv[1]`, `custom_params` from stdin, loads the `cf32` data with
numpy, energy-gates against the segment peak, and emits one annotation per detected
burst (omitting freq edges so inspectrum uses the pass-band). Any language works — the
contract is just argv + stdin + stdout JSON.
