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
"""inspectrum analysis plugin: 2FSK/MSK burst analyser.

Each energy-gated burst is band-limited to its occupied spectrum (block-PSD
estimate + brick-wall filter, so wideband noise outside the signal cannot
swamp the discriminator) and, when the signal occupies most of Nyquist,
upsampled by FFT zero-padding so low-oversampling captures (down to ~3
samples/symbol, e.g. 666 kBd at 2 Msps) still analyse. It then estimates:
  - the two tone frequencies -> shift (deviation) and carrier offset,
  - the symbol rate (from zero-crossing intervals of the discriminator),
  - the modulation index h = tone separation / symbol rate = 2*shift/Rb
    (h ~= 0.5 -> labelled MSK, otherwise 2FSK),
  - the data bits (run-length slicing between crossings; 1 = higher tone).
Bursts with a recovered rate carry the estimated occupied band in
core:freq_lower_edge/upper_edge (absolute Hz); "carrier" (no tone pair) and
"FSK" (tone pair but no transitions to rate from) annotations omit the edges
so inspectrum fills them from the pass-band. A gated region with no spectral
peak 10 dB above the floor — or too short (< 32 samples) for a spectral
estimate — is discarded as noise.

Contract (see doc/plugins.md):
  argv[-1] : path to the segment's .sigmf-meta (cf32_le .sigmf-data alongside)
  stdin    : JSON { "sample_rate", "center_freq", "custom_params": {...} }
  stdout   : JSON { "annotations": [...] }  -- sample indices SEGMENT-LOCAL

custom_params:
  threshold_db   : burst gate relative to segment peak power (default -15 dB)
  min_duration_ms: drop bursts shorter than this (default 0.5 ms)
  merge_gap_ms   : merge bursts separated by less than this (default 0.5 ms)
  symbol_rate_hz : force the symbol rate; 0 = estimate per burst (default 0)
  max_bits       : bits of decoded data shown in the comment (default 96)

Limitations: 2-level FSK only. A burst whose tone spread exceeds ~1/3 of the
tone separation is flagged "multi-level?" in the comment. Gaussian pulse
shaping (GFSK) biases the measured shift low — tones are re-estimated from
multi-symbol run interiors to compensate, but very soft shaping (BT <= 0.3)
can still read h ~= 0.4 and be labelled 2FSK. Strong in-band spurs sharing
the burst's spectrum corrupt the estimates: tune the spectrogram onto the
signal first in crowded captures.

The symbol rate is recovered from discriminator zero-crossing intervals,
which is accurate (within a few %) on typical random-ish data down to ~3
samples/symbol, but DEGRADES on payloads dominated by long same-symbol runs:
heavy bit imbalance (e.g. idle-high framing, ~10-20% low) and repetitive
framing with few single-symbol runs (can lock onto a small multiple of the
true rate). The tone frequencies, deviation, modulation type and decoded
bits remain usable in those cases; treat Rb as approximate. Analysis is
capped at 4 Msamples and 65536 decoded bits per burst (a note is appended to
the comment when either cap is hit).
"""

import json
import os
import sys

import numpy as np

MAX_ANALYSIS = 1 << 22     # samples of a burst actually analysed
MAX_DECODE_BITS = 1 << 16  # hard cap on decoded bits per burst
MAX_BURSTS = 100000        # gate scan stops here (matches the host's cap)
MAX_TOTAL_BIT_CHARS = 2000000  # bits shown across ALL comments (bounds stdout)
CHUNK = 1 << 20            # gating chunk (samples)


def load_meta(meta_path):
    with open(meta_path, "r") as f:
        meta = json.load(f)
    g = meta.get("global", {})
    caps = meta.get("captures", [{}])
    data_name = g.get("core:dataset")
    if not data_name:
        base = os.path.splitext(meta_path)[0]
        data_name = os.path.basename(base) + ".sigmf-data"
    data_path = os.path.join(os.path.dirname(meta_path), data_name)
    sample_rate = float(g.get("core:sample_rate", 0.0))
    center_freq = float(caps[0]["core:frequency"]) \
        if caps and "core:frequency" in caps[0] else None
    datatype = g.get("core:datatype", "cf32_le")
    return data_path, sample_rate, center_freq, datatype


def find_runs(mask):
    """Yield (start, end) half-open index ranges of contiguous True in mask."""
    if mask.size == 0:
        return
    diff = np.diff(mask.astype(np.int8))
    starts = list(np.where(diff == 1)[0] + 1)
    ends = list(np.where(diff == -1)[0] + 1)
    if mask[0]:
        starts.insert(0, 0)
    if mask[-1]:
        ends.append(mask.size)
    for s, e in zip(starts, ends):
        yield int(s), int(e)


def movavg(v, w):
    """Centred moving average of width w, edge values repeated (O(n) via cumsum)."""
    w = int(w)
    if w <= 1 or v.size <= w:
        return v
    c = np.cumsum(np.concatenate([[0.0], v.astype(np.float64)]))
    out = (c[w:] - c[:-w]) / w
    lpad = (v.size - out.size) // 2
    rpad = v.size - out.size - lpad
    return np.concatenate([np.full(lpad, out[0]), out, np.full(rpad, out[-1])])


def min_power_window(excess, frac):
    """Narrowest contiguous bin range [a, b) holding `frac` of total power.

    Two-pointer sweep over the non-negative per-bin excess. Because the two
    humps of a symmetric wide-shift FSK pair — or a strong tone and its weak
    partner in heavily unbalanced data — split the power, a high fraction is
    forced to span both; a genuinely broadband signal yields nearly the whole
    band; a narrowband signal stays tight.
    """
    total = float(excess.sum())
    if total <= 0:
        return None
    target = frac * total
    cum = np.concatenate([[0.0], np.cumsum(excess)])
    best = (0, excess.size)
    i = 0
    for j in range(1, excess.size + 1):
        while cum[j] - cum[i] >= target and i < j:
            if (j - i) < (best[1] - best[0]):
                best = (i, j)
            i += 1
    return best


def signal_band(x, fs):
    """Occupied band (lo, hi) Hz from a block-averaged PSD.

    Returns None when nothing rises 10 dB above the median floor (noise) or
    the burst is too short (< 32 samples) to tell signal from noise. When a
    signal is present the band is the narrowest window holding 95% of the
    HOT-bin power (measuring hot power only decouples the width from
    broadband noise); isolated narrow spurs well below the main signal (an
    SDR DC spike) are dropped first. A constant-envelope signal so wide it
    becomes its own noise floor (e.g. MSK near Rb = fs/2, no hot bin) gets
    the full band.
    """
    block = 1024
    while block > x.size // 2:
        block //= 2
    if block < 16:
        return None
    nb = x.size // block
    v = x[:nb * block].reshape(nb, block)
    psd = np.mean(np.abs(np.fft.fft(v, axis=1)) ** 2, axis=0)
    freqs = np.fft.fftfreq(block, 1.0 / fs)
    order = np.argsort(freqs)
    psd, freqs = psd[order], freqs[order]
    floor = float(np.median(psd))
    hot = psd > 10.0 * floor
    if not hot.any():
        # Flat spectrum: either noise, or a signal filling most of Nyquist.
        # Constant envelope tells them apart: FSK has |x| ~= const (CV ~ 0),
        # noise is Rayleigh (CV ~ 0.52).
        env = np.abs(x[:nb * block])
        mean_env = float(env.mean())
        if nb >= 16 and mean_env > 0 and float(env.std()) / mean_env < 0.3:
            return (-0.5 * fs, 0.5 * fs)
        return None
    excess = np.clip(psd - floor, 0.0, None) * hot
    regions = list(find_runs(hot))
    powers = [float(excess[s:e].sum()) for s, e in regions]
    peak = max(powers)
    for (s, e), pw in zip(regions, powers):
        # A narrow region holding little power next to a much stronger signal
        # is a spur (e.g. a 1-bin DC spike), not an FSK tone — drop it.
        if (e - s) <= 2 and pw < 0.5 * peak:
            excess[s:e] = 0.0
    win = min_power_window(excess, 0.95)
    if win is None:
        return None
    lo, hi = float(freqs[win[0]]), float(freqs[win[1] - 1])
    # Pad by ~0.6 of the window width: the 95%-power window brackets the FSK
    # tone lines, but a line-spectrum burst (alternating 0101… preamble)
    # carries its FM squarewave harmonics further out, and clipping them
    # collapses the signal to a two-tone beat that misreads as a carrier.
    pad = 0.6 * (hi - lo) + 2.0 * fs / block
    return (lo - pad, hi + pad)


def filter_upsample(x, fs, lo, hi, up):
    """Brick-wall to [lo, hi] Hz and upsample by FFT zero-padding."""
    n = x.size
    X = np.fft.fft(x)
    freqs = np.fft.fftfreq(n, 1.0 / fs)
    X[(freqs < lo) | (freqs > hi)] = 0
    if up <= 1:
        return np.fft.ifft(X)
    Xu = np.zeros(n * up, dtype=complex)
    h = (n + 1) // 2  # DC + positive bins (n//2 would misplace one for odd n)
    Xu[:h] = X[:h]
    Xu[-(n - h):] = X[h:]
    return np.fft.ifft(Xu) * up


def two_tones(f):
    """1-D 2-means with median centroids: (low, high, spread) or None.

    Medians resist the sweep through mid frequencies during symbol
    transitions, which pulls plain means inward. The wide [2, 98] percentile
    seeding keeps one seed in each tone even for heavily unbalanced data
    (e.g. idle-high UART framing with ~90% ones). spread is the larger
    within-cluster MAD -- used to spot carriers and multi-level FSK.
    """
    lo, hi = np.percentile(f, [2.0, 98.0])
    if not (np.isfinite(lo) and np.isfinite(hi)) or lo >= hi:
        return None
    c0, c1 = float(lo), float(hi)
    below = above = None
    for _ in range(12):
        mid = 0.5 * (c0 + c1)
        below = f[f <= mid]
        above = f[f > mid]
        if below.size == 0 or above.size == 0:
            return None
        n0 = float(np.median(below))
        n1 = float(np.median(above))
        if n0 == c0 and n1 == c1:
            break
        c0, c1 = n0, n1
    mad0 = float(np.median(np.abs(below - c0)))
    mad1 = float(np.median(np.abs(above - c1)))
    return c0, c1, max(mad0, mad1)


def zero_crossings(v):
    """Sub-sample zero-crossing positions of v via linear interpolation."""
    s = np.signbit(v)
    idx = np.where(s[:-1] != s[1:])[0]
    if idx.size == 0:
        return np.empty(0)
    denom = v[idx] - v[idx + 1]
    frac = np.where(denom != 0, v[idx] / np.where(denom == 0, 1.0, denom), 0.0)
    return idx + frac


def drop_close_pairs(cross, min_gap):
    """Remove pairs of crossings closer than min_gap (noise double-crossings).

    Dropping crossings in pairs keeps the alternating sign structure valid.
    """
    out = []
    i = 0
    while i < len(cross):
        if i + 1 < len(cross) and cross[i + 1] - cross[i] < min_gap:
            i += 2
        else:
            out.append(cross[i])
            i += 1
    return np.array(out)


def _period_candidates(d):
    """Fit a symbol period from each of several seeds over intervals d.

    Returns [(T, inlier_fraction, mean_abs_residual), ...]. Seeds span low
    percentiles (rare single-symbol runs in repetitive payloads) up to the
    median; each is re-fit as T = sum(d)/sum(round(d/T)) since crossing
    intervals of NRZ data are integer multiples of the symbol period.
    """
    seeds = [float(np.percentile(d, q)) for q in (2.0, 5.0, 20.0, 50.0)]
    seeds.append(float(np.min(d)))
    cands = []
    for t in seeds:
        if t <= 0:
            continue
        for _ in range(5):
            k = np.maximum(1.0, np.round(d / t))
            good = np.abs(d / k - t) < 0.3 * t
            if np.count_nonzero(good) >= 2:
                t = float(np.sum(d[good]) / np.sum(k[good]))
            else:
                t = float(np.sum(d) / np.sum(k))
            if t <= 0:
                break
        if t <= 0:
            continue
        k = np.maximum(1.0, np.round(d / t))
        inl = np.abs(d / k - t) < 0.3 * t
        frac = float(np.mean(inl))
        resid = float(np.mean(np.abs(d[inl] / k[inl] - t) / t)) if inl.any() else 1.0
        cands.append((t, frac, resid))
    return cands


def _pick_period(cands):
    """Largest T that fits nearly all intervals with a low residual.

    A too-small T (T/2, T/3) fits everything too, because round(d/T) absorbs
    any interval — hence "largest". The residual gate rejects a 1.5T grid
    (systematic 0.5T residual on a {1T, 2T} mix scores near zero at 0.3T but
    would otherwise sneak through on frac alone). Falls back to the
    best-fitting candidate when none clear the absolute gate.
    """
    if not cands:
        return 0.0
    good = [t for t, frac, resid in cands if frac >= 0.85 and resid < 0.1]
    if good:
        return max(good)
    best = max(frac for _, frac, _ in cands)
    if best < 0.5:
        return 0.0  # nothing fits even half the intervals: no usable rate
    return max(t for t, frac, _ in cands if frac >= best - 0.02)


def estimate_period(cross, min_d=3.0):
    """Symbol period (samples) from crossing intervals, or 0 on failure.

    min_d filters noise double-crossings and must scale with any upsampling
    (the caller passes ~1.5 * the upsample factor). Accurate for random data
    down to ~3 samples/symbol; heavily unbalanced (>>50% one symbol) or
    repetitive payloads with few single-symbol runs bias the estimate (see
    the module docstring) but keep the correct order of magnitude.
    """
    d = np.diff(cross)
    d = d[d >= min_d]
    if d.size < 2:
        return 0.0
    return _pick_period(_period_candidates(d))


def runs_between(centered, cross):
    """Yield (ia, ib, span) sample ranges of the runs delimited by crossings."""
    bounds = np.concatenate([[0.0], cross, [float(centered.size)]])
    for a, b in zip(bounds[:-1], bounds[1:]):
        ia, ib = int(np.ceil(a)), int(np.floor(b))
        if ib > ia:
            yield ia, ib, b - a


def decode_bits(centered, cross, t):
    """Slice bits from the runs between crossings; '1' = higher tone.

    Run lengths are rounded to whole symbols, so a small symbol-rate error
    cannot accumulate into timing drift across the burst.
    """
    out = []
    total = 0
    for ia, ib, span in runs_between(centered, cross):
        k = int(round(span / t))
        if k <= 0:
            continue
        k = min(k, MAX_DECODE_BITS - total)
        out.append(("1" if np.median(centered[ia:ib]) > 0 else "0") * k)
        total += k
        if total >= MAX_DECODE_BITS:
            break
    return "".join(out)


def refine_tones(centered, cross, t, c0, c1):
    """Re-estimate the tone pair from multi-symbol run interiors.

    Single-symbol pulses never reach full deviation under Gaussian shaping
    (GFSK), biasing whole-cluster medians low; interiors of runs >= 2
    symbols do reach it. Returns possibly-updated (c0, c1).
    """
    margin = int(round(0.5 * t))
    ones, zeros = [], []
    for ia, ib, _ in runs_between(centered, cross):
        ia, ib = ia + margin, ib - margin
        if ib <= ia:
            continue
        seg = centered[ia:ib]
        (ones if np.median(seg) > 0 else zeros).append(seg)
    if not ones or not zeros:
        return c0, c1
    mid = 0.5 * (c0 + c1)
    return (mid + float(np.median(np.concatenate(zeros))),
            mid + float(np.median(np.concatenate(ones))))


def fmt_rate(rb):
    # Round to 3 significant figures BEFORE picking the unit, so a rate that
    # rounds up to a decade boundary becomes "1MBd", not "1e+03kBd".
    r = float("%.3g" % rb)
    if r >= 1e6:
        return "%gMBd" % (r / 1e6)
    if r >= 1e3:
        return "%gkBd" % (r / 1e3)
    return "%gBd" % r


def analyze_burst(x, fs, forced_rate, max_bits):
    """Analyse one burst of complex samples; returns the annotation fields."""
    truncated = x.size > MAX_ANALYSIS
    if truncated:
        x = x[:MAX_ANALYSIS]
    if x.size < 32:
        return None
    finite = np.isfinite(x)
    if not finite.all():
        x = np.where(finite, x, 0)  # a stray NaN/Inf must not poison the FFTs

    # Band-limit to the occupied spectrum so out-of-band noise cannot swamp
    # the discriminator (matters when the segment wasn't tuner-filtered).
    band = signal_band(x, fs)
    if band is None:
        return None  # gated on a noise fluctuation, not a signal
    # If the signal occupies most of Nyquist the symbol rate can approach
    # fs/2; upsample so crossing intervals stay well above the noise filter.
    bw = band[1] - band[0]
    up = 1
    while up < 8 and fs * up < 8.0 * bw:
        up *= 2
    if up > 1 and x.size > MAX_ANALYSIS // up:
        x = x[:MAX_ANALYSIS // up]
        truncated = True
    x = filter_upsample(x, fs, band[0], band[1], up)
    fs = fs * up

    # Instantaneous frequency (Hz) from the phase step between samples.
    f = np.angle(x[1:] * np.conj(x[:-1])) * (fs / (2.0 * np.pi))

    # Pass A: light smoothing -> coarse tones and period.
    fa = movavg(f, 3)
    tones = two_tones(fa)
    if tones is None:
        return None
    c0, c1, spread = tones
    if (c1 - c0) < 4.0 * spread:
        # No resolvable tone pair (a 2-means split of a single noisy tone
        # yields separation ~= 1.4 sigma against spread ~= 0.4 sigma).
        return {"kind": "carrier", "offset": float(np.median(f))}
    # 1.5*up (not 3*up): at ~2 samples/symbol the single-symbol crossing
    # intervals are only 2*up long and must survive the noise filter.
    min_d = max(3.0, 1.5 * up)
    t = fs / forced_rate if forced_rate > 0 else \
        estimate_period(zero_crossings(fa - 0.5 * (c0 + c1)), min_d)
    if not np.isfinite(t) or t <= 0:
        # No usable rate, or a forced rate so tiny that fs/rate overflowed
        # once fs was scaled by the upsample factor. Treat as "no rate".
        t = 0.0

    # Pass B: smooth to ~T/6, re-estimate tones, then final crossings/decode.
    bits = ""
    if t > 0:
        fb = movavg(f, min(max(int(round(t / 6.0)), 3), 257))
        tones = two_tones(fb) or tones
        c0, c1, spread = tones
        centered = fb - 0.5 * (c0 + c1)
        cross = drop_close_pairs(zero_crossings(centered), 0.4 * t)
        if forced_rate <= 0 and cross.size >= 3:
            t = estimate_period(cross, min_d) or t
        if cross.size >= 1:
            bits = decode_bits(centered, cross, t)
            c0, c1 = refine_tones(centered, cross, t, c0, c1)

    dev = 0.5 * (c1 - c0)
    offset = 0.5 * (c1 + c0)
    rb = fs / t if t > 0 else 0.0
    h = (2.0 * dev / rb) if rb > 0 else 0.0

    if rb > 0 and abs(h - 0.5) <= 0.08:
        kind = "MSK"
    elif rb > 0:
        kind = "2FSK"
    else:
        kind = "FSK"  # tones resolved but no rate (no transitions)

    notes = []
    if spread > 0.35 * (c1 - c0):
        notes.append("multi-level?")
    if truncated:
        notes.append("analysed first %.0f ms" % (x.size / fs * 1e3))
    if len(bits) >= MAX_DECODE_BITS:
        notes.append("bit decode capped at %d" % MAX_DECODE_BITS)
    return {
        "kind": kind, "rb": rb, "dev": dev, "offset": offset, "h": h,
        "bits": bits, "notes": notes, "max_bits": max_bits,
    }


def burst_annotation(res, s, e, fs, center_freq):
    """Format one burst's analysis into an annotation dict."""
    ann = {
        "core:sample_start": int(s),
        "core:sample_count": int(e - s),
        "core:generator": "inspectrum fsk-analyze.py",
        "presentation:color": "#FF8C00A0",
    }
    if res["kind"] == "carrier":
        ann["core:label"] = "carrier"
        ann["core:comment"] = "fsk-analyze: carrier, offset %+.0f Hz" % res["offset"]
        return ann

    parts = ["shift ±%.1f Hz" % res["dev"], "offset %+.0f Hz" % res["offset"]]
    if res["rb"] > 0:
        parts.insert(0, "Rb=%.1f Bd (h=%.2f)" % (res["rb"], res["h"]))
        label = "%s %s" % (res["kind"], fmt_rate(res["rb"]))
        # Carson-style occupied band around the tone pair, absolute Hz.
        half_bw = res["dev"] + 0.5 * res["rb"]
        ann["core:freq_lower_edge"] = center_freq + res["offset"] - half_bw
        ann["core:freq_upper_edge"] = center_freq + res["offset"] + half_bw
    else:
        parts.insert(0, "Rb n/a (no transitions)")
        label = res["kind"]
    shown = res["bits"][:max(res["max_bits"], 0)] if res["bits"] else ""
    if shown:
        ell = "…" if len(res["bits"]) > len(shown) else ""
        parts.append("bits[%d]=%s%s" % (len(res["bits"]), shown, ell))
    parts.extend(res["notes"])
    ann["core:label"] = label
    ann["core:comment"] = "fsk-analyze: " + ", ".join(parts)
    return ann


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: fsk-analyze.py <segment.sigmf-meta>\n")
        return 2
    meta_path = sys.argv[-1]

    raw = sys.stdin.read()
    ctx = json.loads(raw) if raw.strip() else {}
    params = ctx.get("custom_params", {}) or {}

    threshold_db = float(params.get("threshold_db", -15.0))
    min_duration_ms = float(params.get("min_duration_ms", 0.5))
    merge_gap_ms = float(params.get("merge_gap_ms", 0.5))
    forced_rate = float(params.get("symbol_rate_hz", 0.0))
    max_bits = int(params.get("max_bits", 96))

    data_path, sample_rate, center_freq, datatype = load_meta(meta_path)
    if datatype != "cf32_le":
        sys.stderr.write("fsk-analyze: expected cf32_le, got %s\n" % datatype)
        return 1
    if not np.isfinite(sample_rate) or sample_rate <= 0:
        sample_rate = float(ctx.get("sample_rate", 0.0))
    if not np.isfinite(sample_rate) or sample_rate <= 0:
        sys.stderr.write("fsk-analyze: no sample rate in meta or context\n")
        return 1
    if center_freq is None:
        center_freq = float(ctx.get("center_freq", 0.0))
    if not np.isfinite(center_freq):
        center_freq = 0.0
    if forced_rate > 0:
        # numpy division: a denormal rate yields inf here instead of raising
        # OverflowError the way plain float division would.
        ratio = float(np.float64(sample_rate) / np.float64(forced_rate))
        if not (np.isfinite(forced_rate) and np.isfinite(ratio) and ratio >= 1.0):
            sys.stderr.write("fsk-analyze: symbol_rate_hz out of range\n")
            return 1

    nbytes = os.path.getsize(data_path)
    n_samples = nbytes // 8  # complex64
    if n_samples == 0:
        print(json.dumps({"annotations": []}))
        return 0
    x = np.memmap(data_path, dtype=np.complex64, mode="r", shape=(n_samples,))

    min_samples = max(1, int(min_duration_ms * 1e-3 * sample_rate))
    merge_samples = int(merge_gap_ms * 1e-3 * sample_rate)
    # Smooth the gate power over ~1/4 of the minimum burst so isolated noise
    # peaks can't trip it (they'd otherwise chain up via the merge pass).
    win_gate = max(1, min(min_samples // 4, 4096))

    def chunk_power(off):
        c = x[off:off + CHUNK]
        p = c.real.astype(np.float32) ** 2 + c.imag.astype(np.float32) ** 2
        # NaN/Inf samples occur in real captures; they must not poison the
        # smoothing cumsum or the peak (Inf would gate out the whole file).
        np.nan_to_num(p, copy=False, nan=0.0, posinf=0.0, neginf=0.0)
        return movavg(p, win_gate)

    # Energy gate, chunked as in energy-detect.py: peak pass, then threshold
    # pass. Runs are merged and length-filtered inline so memory stays bounded
    # even when the gate flickers on every other sample.
    peak = 0.0
    for off in range(0, n_samples, CHUNK):
        p = chunk_power(off)
        if p.size:
            peak = max(peak, float(p.max()))
    if peak <= 0:
        print(json.dumps({"annotations": []}))
        return 0
    thresh = peak * (10.0 ** (threshold_db / 10.0))

    bursts = []
    pend = None
    capped = False
    for off in range(0, n_samples, CHUNK):
        for s, e in find_runs(chunk_power(off) > thresh):
            s, e = off + s, off + e
            if pend is not None and s - pend[1] <= merge_samples:
                pend[1] = e
                continue
            if pend is not None and pend[1] - pend[0] >= min_samples:
                bursts.append(pend)
                if len(bursts) >= MAX_BURSTS:
                    capped = True
                    break
            pend = [s, e]
        if capped:
            break
    if not capped and pend is not None and pend[1] - pend[0] >= min_samples:
        bursts.append(pend)
    if capped:
        sys.stderr.write("fsk-analyze: stopped after %d bursts\n" % MAX_BURSTS)

    annotations = []
    bit_budget = MAX_TOTAL_BIT_CHARS
    for s, e in bursts:
        res = analyze_burst(np.asarray(x[s:e]), sample_rate, forced_rate,
                            min(max_bits, bit_budget))
        if res is None:
            continue
        bit_budget -= min(max(res.get("max_bits", 0), 0), len(res.get("bits") or ""))
        annotations.append(burst_annotation(res, s, e, sample_rate, center_freq))

    json.dump({"annotations": annotations}, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
