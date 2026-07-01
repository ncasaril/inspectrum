#!/usr/bin/env python3
#
#  Copyright (C) 2026, Niklas Casaril <niklas@casaril.com>
#
#  This file is part of inspectrum.
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
"""Reference inspectrum analysis plugin: energy-gated burst / call detector.

Contract (see doc/plugins.md):
  argv[1]  : path to the segment's .sigmf-meta  (cf32_le .sigmf-data alongside it)
  stdin    : JSON { "sample_rate", "center_freq", "custom_params": {...} }
  stdout   : JSON { "annotations": [ {core:sample_start, core:sample_count, ...}, ... ] }

Sample indices in the output are SEGMENT-LOCAL (relative to sample 0 of the
extracted segment); inspectrum maps them back to absolute file indices. We omit
core:freq_lower_edge/upper_edge so inspectrum fills them from the tuner pass-band.

custom_params:
  threshold_db   : gate level relative to the segment's peak power (default -10 dB)
  min_duration_ms: drop detections shorter than this (default 1.0 ms)
  merge_gap_ms   : merge detections separated by less than this (default 0.5 ms)
  label          : label applied to each detection (default "burst")
"""

import json
import os
import sys

import numpy as np


def load_meta(meta_path):
    with open(meta_path, "r") as f:
        meta = json.load(f)
    g = meta.get("global", {})
    caps = meta.get("captures", [{}])
    data_name = g.get("core:dataset")
    if not data_name:
        # Fall back to the conventional sibling name.
        base = os.path.splitext(meta_path)[0]
        data_name = os.path.basename(base) + ".sigmf-data"
    data_path = os.path.join(os.path.dirname(meta_path), data_name)
    sample_rate = float(g.get("core:sample_rate", 0.0))
    center_freq = float(caps[0].get("core:frequency", 0.0)) if caps else 0.0
    datatype = g.get("core:datatype", "cf32_le")
    return data_path, sample_rate, center_freq, datatype


def find_runs(mask):
    """Yield (start, end) half-open index ranges of contiguous True in mask."""
    if mask.size == 0:
        return
    # Edges where the boolean changes.
    diff = np.diff(mask.astype(np.int8))
    starts = list(np.where(diff == 1)[0] + 1)
    ends = list(np.where(diff == -1)[0] + 1)
    if mask[0]:
        starts.insert(0, 0)
    if mask[-1]:
        ends.append(mask.size)
    for s, e in zip(starts, ends):
        yield int(s), int(e)


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: energy-detect.py <segment.sigmf-meta>\n")
        return 2
    # inspectrum appends the meta path as the LAST argument (after any fixed args),
    # so read argv[-1] rather than argv[1].
    meta_path = sys.argv[-1]

    # Context (custom params) on stdin; tolerate an empty stdin.
    raw = sys.stdin.read()
    ctx = json.loads(raw) if raw.strip() else {}
    params = ctx.get("custom_params", {}) or {}

    threshold_db = float(params.get("threshold_db", -10.0))
    min_duration_ms = float(params.get("min_duration_ms", 1.0))
    merge_gap_ms = float(params.get("merge_gap_ms", 0.5))
    label = str(params.get("label", "burst"))

    data_path, sample_rate, center_freq, datatype = load_meta(meta_path)
    if datatype != "cf32_le":
        sys.stderr.write("energy-detect: expected cf32_le, got %s\n" % datatype)
        return 1
    # Prefer the rate handed in via context if the meta lacks it.
    if sample_rate <= 0:
        sample_rate = float(ctx.get("sample_rate", 0.0))

    # Stream the segment in bounded chunks via memmap rather than loading it whole:
    # inspectrum can extract multi-GB segments from 100GB+ captures, and float32
    # power keeps peak RAM to ~one chunk regardless of segment size.
    nbytes = os.path.getsize(data_path)
    n_samples = nbytes // 8  # complex64 == 8 bytes
    if n_samples == 0:
        print(json.dumps({"annotations": []}))
        return 0
    x = np.memmap(data_path, dtype=np.complex64, mode="r", shape=(n_samples,))
    CHUNK = 1 << 20  # 1 Msample/chunk

    def chunk_power(off):
        c = x[off:off + CHUNK]
        return c.real.astype(np.float32) ** 2 + c.imag.astype(np.float32) ** 2

    # Pass 1: global peak power (no whole-segment allocation).
    peak = 0.0
    for off in range(0, n_samples, CHUNK):
        p = chunk_power(off)
        if p.size:
            peak = max(peak, float(p.max()))
    if peak <= 0:
        print(json.dumps({"annotations": []}))
        return 0

    thresh = peak * (10.0 ** (threshold_db / 10.0))
    min_samples = max(1, int(min_duration_ms * 1e-3 * sample_rate)) if sample_rate > 0 else 1
    merge_samples = int(merge_gap_ms * 1e-3 * sample_rate) if sample_rate > 0 else 0

    # Pass 2: collect above-threshold runs per chunk in absolute indices. A run split
    # across a chunk boundary has gap 0 and is rejoined by the merge pass below.
    runs = []
    for off in range(0, n_samples, CHUNK):
        mask = chunk_power(off) > thresh
        for s, e in find_runs(mask):
            runs.append((off + s, off + e))

    # Merge runs separated by a gap <= merge_samples (also stitches boundary splits).
    merged = []
    for s, e in runs:
        if merged and s - merged[-1][1] <= merge_samples:
            merged[-1][1] = e
        else:
            merged.append([s, e])

    annotations = []
    for s, e in merged:
        if (e - s) < min_samples:
            continue
        dur_ms = (e - s) / sample_rate * 1e3 if sample_rate > 0 else 0.0
        annotations.append({
            "core:sample_start": int(s),
            "core:sample_count": int(e - s),
            "core:label": label,
            "core:comment": "energy-detect: %.2f ms" % dur_ms,
            "core:generator": "inspectrum energy-detect.py",
        })

    json.dump({"annotations": annotations}, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
