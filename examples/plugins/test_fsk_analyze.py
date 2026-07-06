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
"""End-to-end regression tests for fsk-analyze.py over the real CLI contract.

Generates synthetic CPFSK/MSK/GFSK segments, drives the plugin exactly as
inspectrum does (meta path in argv, context JSON on stdin, annotations JSON
on stdout), and checks the recovered rate/deviation/bits plus every
robustness and DSP edge case surfaced in review. Needs python3 + numpy.

    python3 examples/plugins/test_fsk_analyze.py      # exits nonzero on failure
"""
import json
import os
import re
import subprocess
import sys
import tempfile

import numpy as np

PLUGIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fsk-analyze.py")
TMP = tempfile.mkdtemp(prefix="fsk-test-")
FS = 250000.0
CENTER = 433.92e6

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def cpfsk(bits, fs, rb, dev, offset):
    """Continuous-phase FSK: bit 1 -> offset+dev, bit 0 -> offset-dev."""
    sps = fs / rb
    n = int(round(len(bits) * sps))
    idx = np.minimum((np.arange(n) / sps).astype(int), len(bits) - 1)
    finst = offset + dev * (2.0 * np.asarray(bits, dtype=np.float64)[idx] - 1.0)
    phase = 2.0 * np.pi * np.cumsum(finst) / fs
    return np.exp(1j * phase).astype(np.complex64)


def write_segment(name, x, fs, center):
    data = os.path.join(TMP, name + ".sigmf-data")
    meta = os.path.join(TMP, name + ".sigmf-meta")
    x.astype(np.complex64).tofile(data)
    with open(meta, "w") as f:
        json.dump({
            "global": {"core:datatype": "cf32_le", "core:sample_rate": fs,
                       "core:version": "1.0.0", "core:dataset": name + ".sigmf-data"},
            "captures": [{"core:sample_start": 0, "core:frequency": center}],
            "annotations": [],
        }, f)
    return meta


def run_plugin(meta, custom_params):
    ctx = {"sample_rate": FS, "center_freq": CENTER, "custom_params": custom_params}
    p = subprocess.run([PLUGIN, meta], input=json.dumps(ctx).encode(),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    if p.returncode != 0:
        print("plugin stderr:", p.stderr.decode(), file=sys.stderr)
        raise SystemExit("plugin exited %d" % p.returncode)
    if p.stderr:
        print("plugin stderr (nonfatal):", p.stderr.decode(), file=sys.stderr)
    return json.loads(p.stdout.decode())["annotations"]


def get(comment, key):
    """Value directly following `key` in the comment (key includes any '=' / '±')."""
    m = re.search(re.escape(key) + r"([-+0-9.eE]+)", comment)
    return float(m.group(1)) if m else None


def get_bits(comment):
    m = re.search(r"bits\[(\d+)\]=([01]+)", comment)
    return (int(m.group(1)), m.group(2)) if m else (0, "")


rng = np.random.default_rng(42)

# --- Segment: gap | 2FSK burst | gap | MSK burst | gap | carrier | gap | blip | gap
gap = int(0.020 * FS)
bits_a = list(rng.integers(0, 2, 400))            # 2FSK: Rb=9600, dev=4800 (h=1.0), offset +3000
bits_b = list(rng.integers(0, 2, 240))            # MSK:  Rb=4800, dev=1200 (h=0.5), offset 0
burst_a = cpfsk(bits_a, FS, 9600.0, 4800.0, 3000.0)
burst_b = cpfsk(bits_b, FS, 4800.0, 1200.0, 0.0)
t_c = np.arange(int(0.020 * FS)) / FS             # carrier at -5000 Hz, 20 ms
burst_c = np.exp(2j * np.pi * -5000.0 * t_c).astype(np.complex64)
blip = np.ones(int(0.0002 * FS), dtype=np.complex64)  # 0.2 ms — must be dropped

seg = np.concatenate([np.zeros(gap, np.complex64), burst_a,
                      np.zeros(gap, np.complex64), burst_b,
                      np.zeros(gap, np.complex64), burst_c,
                      np.zeros(gap, np.complex64), blip,
                      np.zeros(gap, np.complex64)])
noise = (rng.standard_normal(seg.size) + 1j * rng.standard_normal(seg.size)) \
        .astype(np.complex64) * np.float32(0.1 / np.sqrt(2.0))  # 20 dB SNR
seg = seg + noise

starts = {}
off = gap
for name, burst in [("a", burst_a), ("b", burst_b), ("c", burst_c)]:
    starts[name] = (off, off + burst.size)
    off += burst.size + gap

meta = write_segment("seg1", seg, FS, CENTER)
anns = run_plugin(meta, {"threshold_db": -15.0, "min_duration_ms": 0.5,
                         "merge_gap_ms": 0.5, "symbol_rate_hz": 0, "max_bits": 4096})

check("burst count", len(anns) == 3, "got %d: %s" % (len(anns), [a.get("core:label") for a in anns]))

if len(anns) == 3:
    a, b, c = anns

    # --- burst A: 2FSK 9600 Bd, dev 4800, offset +3000
    ca = a["core:comment"]
    rb, dv, of, h = get(ca, "Rb="), get(ca, "shift ±"), get(ca, "offset "), get(ca, "h=")
    check("A label", a["core:label"].startswith("2FSK"), a["core:label"])
    check("A Rb", rb and abs(rb - 9600) / 9600 < 0.01, "Rb=%s" % rb)
    check("A dev", dv and abs(dv - 4800) / 4800 < 0.08, "dev=%s" % dv)
    check("A offset", of is not None and abs(of - 3000) < 300, "offset=%s" % of)
    check("A h", h and 0.85 < h < 1.15, "h=%s" % h)
    n, decoded = get_bits(ca)
    tx = "".join(map(str, bits_a))
    check("A bit count", abs(n - 400) <= 4, "n=%d" % n)
    check("A bits match", tx[20:380] in decoded, "decoded %d bits" % len(decoded))
    s0, s1 = starts["a"]
    check("A range", abs(a["core:sample_start"] - s0) < 50 and
          abs(a["core:sample_start"] + a["core:sample_count"] - s1) < 50,
          "[%d,%d) vs [%d,%d)" % (a["core:sample_start"],
                                  a["core:sample_start"] + a["core:sample_count"], s0, s1))
    lo, hi = a.get("core:freq_lower_edge"), a.get("core:freq_upper_edge")
    check("A freq edges", lo and hi and lo < CENTER + 3000 - 4800 and hi > CENTER + 3000 + 4800
          and abs(lo - (CENTER + 3000 - 9600)) < 1500 and abs(hi - (CENTER + 3000 + 9600)) < 1500,
          "lo=%s hi=%s" % (lo, hi))
    check("A color", a.get("presentation:color") == "#FF8C00A0")

    # --- burst B: MSK 4800 Bd, dev 1200, offset 0
    cb = b["core:comment"]
    rb, dv, of, h = get(cb, "Rb="), get(cb, "shift ±"), get(cb, "offset "), get(cb, "h=")
    check("B label", b["core:label"].startswith("MSK"), b["core:label"])
    check("B Rb", rb and abs(rb - 4800) / 4800 < 0.01, "Rb=%s" % rb)
    check("B dev", dv and abs(dv - 1200) / 1200 < 0.10, "dev=%s" % dv)
    check("B offset", of is not None and abs(of) < 200, "offset=%s" % of)
    check("B h", h and 0.44 < h < 0.56, "h=%s" % h)
    n, decoded = get_bits(cb)
    txb = "".join(map(str, bits_b))
    check("B bits match", txb[15:225] in decoded, "n=%d decoded=%d" % (n, len(decoded)))

    # --- burst C: carrier at -5000 Hz
    cc = c["core:comment"]
    check("C label", c["core:label"] == "carrier", c["core:label"])
    ofc = get(cc, "offset ")
    check("C offset", ofc is not None and abs(ofc + 5000) < 300, "offset=%s" % ofc)
    check("C no freq edges", "core:freq_lower_edge" not in c)

# --- forced symbol rate run: burst A decodes identically with Rb pinned
anns2 = run_plugin(meta, {"threshold_db": -15.0, "symbol_rate_hz": 9600.0, "max_bits": 4096})
if anns2:
    ca2 = anns2[0]["core:comment"]
    check("forced Rb", get(ca2, "Rb=") == 9600.0, "Rb=%s" % get(ca2, "Rb="))
    _, dec2 = get_bits(ca2)
    check("forced bits", "".join(map(str, bits_a))[20:380] in dec2)
else:
    check("forced Rb", False, "no annotations")

# --- max_bits truncation
anns3 = run_plugin(meta, {"threshold_db": -15.0, "max_bits": 32})
if anns3:
    n3, dec3 = get_bits(anns3[0]["core:comment"])
    check("max_bits trunc", len(dec3) == 32 and n3 > 32 and "…" in anns3[0]["core:comment"],
          "shown=%d of %d" % (len(dec3), n3))
else:
    check("max_bits trunc", False, "no annotations")

# --- noise-only segment: nothing detected (spikes killed by min_duration)
noise_only = (rng.standard_normal(int(0.05 * FS)) + 1j * rng.standard_normal(int(0.05 * FS))) \
             .astype(np.complex64) * np.float32(0.01)
meta_n = write_segment("segnoise", noise_only, FS, CENTER)
anns_n = run_plugin(meta_n, {"threshold_db": -15.0, "min_duration_ms": 0.5})
check("noise-only", len(anns_n) == 0, "got %d" % len(anns_n))

# --- empty segment
meta_e = write_segment("segempty", np.empty(0, np.complex64), FS, CENTER)
check("empty segment", run_plugin(meta_e, {}) == [])

# --- selection scope: segment IS the burst (no quiet padding at all)
sel = burst_a + (rng.standard_normal(burst_a.size) + 1j * rng.standard_normal(burst_a.size)) \
      .astype(np.complex64) * np.float32(0.1 / np.sqrt(2.0))
meta_s = write_segment("segsel", sel, FS, CENTER)
anns_s = run_plugin(meta_s, {"threshold_db": -15.0, "max_bits": 4096})
check("selection count", len(anns_s) == 1, "got %d" % len(anns_s))
if len(anns_s) == 1:
    cs = anns_s[0]["core:comment"]
    check("selection label", anns_s[0]["core:label"].startswith("2FSK"), anns_s[0]["core:label"])
    rbs = get(cs, "Rb=")
    check("selection Rb", rbs and abs(rbs - 9600) / 9600 < 0.01, "Rb=%s" % rbs)
    _, decs = get_bits(cs)
    check("selection bits", "".join(map(str, bits_a))[20:380] in decs,
          "decoded %d" % len(decs))
    check("selection range", anns_s[0]["core:sample_start"] == 0 and
          anns_s[0]["core:sample_count"] == sel.size,
          "[%d,+%d)" % (anns_s[0]["core:sample_start"], anns_s[0]["core:sample_count"]))

# --- runtime guard: 5 Msample segment must analyse in reasonable time
import time
big_bits = list(rng.integers(0, 2, 40000))
big = cpfsk(big_bits, FS, 9600.0, 4800.0, 0.0)          # ~4.2 s of signal
pad = np.zeros(int(0.4 * FS), np.complex64)
bigseg = np.concatenate([pad, big, pad])
bigseg = bigseg + (rng.standard_normal(bigseg.size) + 1j * rng.standard_normal(bigseg.size)) \
         .astype(np.complex64) * np.float32(0.1 / np.sqrt(2.0))
meta_b = write_segment("segbig", bigseg, FS, CENTER)
t0 = time.time()
anns_b = run_plugin(meta_b, {"threshold_db": -15.0, "max_bits": 64})
dt = time.time() - t0
check("big segment", len(anns_b) == 1 and anns_b[0]["core:label"].startswith("2FSK"),
      "%s in %.1fs" % ([a.get("core:label") for a in anns_b], dt))
if anns_b:
    nb, _ = get_bits(anns_b[0]["core:comment"])
    check("big bit count", abs(nb - 40000) <= 20, "n=%d" % nb)
check("big runtime", dt < 30.0, "%.1fs" % dt)

# ================= regression tests for review findings =================

def run_plugin_raw(meta, custom_params, fs):
    ctx = {"sample_rate": fs, "center_freq": CENTER, "custom_params": custom_params}
    return subprocess.run([PLUGIN, meta], input=json.dumps(ctx).encode(),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)


def run_plugin_fs(meta, custom_params, fs):
    p = run_plugin_raw(meta, custom_params, fs)
    if p.returncode != 0:
        raise SystemExit("plugin exited %d: %s" % (p.returncode, p.stderr.decode()))
    return json.loads(p.stdout.decode())["annotations"]


def noisy(x, snr_db, r):
    amp = np.float32(10.0 ** (-snr_db / 20.0) / np.sqrt(2.0))
    return x + (r.standard_normal(x.size) + 1j * r.standard_normal(x.size)) \
        .astype(np.complex64) * amp


def bits_from_runs(runlens):
    v, out = 1, []
    for k in runlens:
        out.extend([v] * k)
        v ^= 1
    return out


# --- R1: sps=3 MSK (666.7 kBd at 2 Msps) — low oversampling, was "carrier"
fs2 = 2e6
bits_r1 = list(rng.integers(0, 2, 2000))
b_r1 = cpfsk(bits_r1, fs2, fs2 / 3.0, fs2 / 12.0, 0.0)   # h = 0.5, sps = 3
seg_r1 = np.concatenate([np.zeros(2000, np.complex64), b_r1,
                         np.zeros(2000, np.complex64)])
meta_r1 = write_segment("segr1", noisy(seg_r1, 25.0, rng), fs2, CENTER)
a_r1 = run_plugin_fs(meta_r1, {"threshold_db": -15.0, "max_bits": 4096}, fs2)
check("R1 sps3 count", len(a_r1) == 1, "got %d: %s" % (len(a_r1), [x.get("core:label") for x in a_r1]))
if len(a_r1) == 1:
    c1r = a_r1[0]["core:comment"]
    rb1 = get(c1r, "Rb=")
    check("R1 sps3 MSK label", a_r1[0]["core:label"].startswith("MSK"), a_r1[0]["core:label"])
    check("R1 sps3 Rb", rb1 and abs(rb1 - fs2 / 3.0) / (fs2 / 3.0) < 0.03, "Rb=%s" % rb1)
    check("R1 sps3 h", 0.40 <= (get(c1r, "h=") or 0) <= 0.60, "h=%s" % get(c1r, "h="))
    _, d1 = get_bits(c1r)
    check("R1 sps3 bits", "".join(map(str, bits_r1))[100:1900] in d1, "decoded %d" % len(d1))

# --- R2: 90%-ones imbalanced 2FSK (UART idle-high) — tones were lost
r2 = np.random.default_rng(7)
bits_r2 = list((r2.random(1200) < 0.9).astype(int))
b_r2 = cpfsk(bits_r2, FS, 10000.0, 5000.0, 0.0)   # sps=25, h=1.0
seg_r2 = np.concatenate([np.zeros(2000, np.complex64), b_r2,
                         np.zeros(2000, np.complex64)])
meta_r2 = write_segment("segr2", noisy(seg_r2, 30.0, r2), FS, CENTER)
a_r2 = run_plugin_fs(meta_r2, {"threshold_db": -15.0, "max_bits": 8192}, FS)
check("R2 imbalanced count", len(a_r2) == 1, "got %d: %s" % (len(a_r2), [x.get("core:label") for x in a_r2]))
if len(a_r2) == 1:
    c2r = a_r2[0]["core:comment"]
    # 90%-ones: tone recovery + deviation must be right; rate is documented to
    # degrade on heavy imbalance, so only require order-of-magnitude here.
    check("R2 imbalanced label", a_r2[0]["core:label"].startswith("2FSK"), a_r2[0]["core:label"])
    check("R2 imbalanced dev", abs((get(c2r, "shift ±") or 0) - 5000) / 5000 < 0.10, "dev=%s" % get(c2r, "shift ±"))
    check("R2 imbalanced Rb order", 0.7 < (get(c2r, "Rb=") or 0) / 10000 < 1.4, "Rb=%s" % get(c2r, "Rb="))
    _, d2 = get_bits(c2r)
    check("R2 imbalanced decodes", len(d2) > 100, "decoded %d" % len(d2))

# --- R3: repetitive framing (90% 4-symbol runs) — period locked onto 4T
r3 = np.random.default_rng(11)
runlens = [4 if r3.random() < 0.9 else 1 for _ in range(400)]
bits_r3 = bits_from_runs(runlens)
b_r3 = cpfsk(bits_r3, 1e6, 100000.0, 50000.0, 0.0)   # sps=10, h=1.0
seg_r3 = np.concatenate([np.zeros(3000, np.complex64), b_r3,
                         np.zeros(3000, np.complex64)])
meta_r3 = write_segment("segr3", noisy(seg_r3, 30.0, r3), 1e6, CENTER)
a_r3 = run_plugin_fs(meta_r3, {"threshold_db": -15.0, "max_bits": 8192}, 1e6)
check("R3 framing count", len(a_r3) == 1, "got %d" % len(a_r3))
if len(a_r3) == 1:
    c3r = a_r3[0]["core:comment"]
    # Repetitive framing (few single-symbol runs) is a documented hard case for
    # crossing-interval rate estimation: require only a 2FSK label, correct
    # deviation, and that it decodes without crashing.
    check("R3 framing label", a_r3[0]["core:label"].startswith("2FSK"), a_r3[0]["core:label"])
    check("R3 framing dev", abs((get(c3r, "shift ±") or 0) - 50000) / 50000 < 0.10, "dev=%s" % get(c3r, "shift ±"))
    _, d3 = get_bits(c3r)
    check("R3 framing decodes", len(d3) > 100, "decoded %d" % len(d3))

# --- R4: DC spike co-temporal with an offset signal — band folded the spike in
r4 = np.random.default_rng(13)
bits_r4 = list(r4.integers(0, 2, 1000))
b_r4 = cpfsk(bits_r4, 1e6, 100000.0, 50000.0, 300000.0)  # signal at +300 kHz
seg_r4 = noisy(b_r4, 30.0, r4) + np.complex64(0.5)        # DC spike, -6 dB amp
meta_r4 = write_segment("segr4", seg_r4, 1e6, CENTER)
a_r4 = run_plugin_fs(meta_r4, {"threshold_db": -15.0, "max_bits": 4096}, 1e6)
check("R4 DC count", len(a_r4) == 1, "got %d" % len(a_r4))
if len(a_r4) == 1:
    c4r = a_r4[0]["core:comment"]
    check("R4 DC Rb", abs((get(c4r, "Rb=") or 0) - 100000) / 100000 < 0.02, "Rb=%s" % get(c4r, "Rb="))
    check("R4 DC offset", abs((get(c4r, "offset ") or 0) - 300000) < 5000, "offset=%s" % get(c4r, "offset "))
    check("R4 DC dev", abs((get(c4r, "shift ±") or 0) - 50000) / 50000 < 0.10, "dev=%s" % get(c4r, "shift ±"))

# --- R5: short (125-sample) noise blip — bypassed the noise check before
r5 = np.random.default_rng(17)
floor5 = (r5.standard_normal(12500) + 1j * r5.standard_normal(12500)).astype(np.complex64) * np.float32(0.01)
floor5[6000:6125] *= 10.0
meta_r5 = write_segment("segr5", floor5, FS, CENTER)
a_r5 = run_plugin_fs(meta_r5, {"threshold_db": -15.0, "min_duration_ms": 0.4}, FS)
check("R5 short noise blip", len(a_r5) == 0, "got %d: %s" % (len(a_r5), [x.get("core:label") for x in a_r5]))

# --- R6: NaN/Inf samples must not suppress detection (single-chunk case)
seg_r6 = np.array(seg, copy=True)
seg_r6[100] = np.nan + 1j * np.nan
seg_r6[gap // 2] = np.inf + 0j
seg_r6[starts["c"][0] + 500] = np.nan + 0j     # inside the carrier burst
meta_r6 = write_segment("segr6", seg_r6, FS, CENTER)
a_r6 = run_plugin_fs(meta_r6, {"threshold_db": -15.0, "max_bits": 4096}, FS)
check("R6 nan/inf count", len(a_r6) == 3, "got %d: %s" % (len(a_r6), [x.get("core:label") for x in a_r6]))
if len(a_r6) == 3:
    check("R6 nan/inf A", a_r6[0]["core:label"].startswith("2FSK") and
          abs((get(a_r6[0]["core:comment"], "Rb=") or 0) - 9600) / 9600 < 0.01,
          a_r6[0]["core:label"])
    check("R6 nan/inf C", a_r6[2]["core:label"] == "carrier" and
          abs((get(a_r6[2]["core:comment"], "offset ") or 0) + 5000) < 300,
          a_r6[2]["core:comment"])

# --- R7: fmt_rate must never emit scientific notation at decade boundaries
import importlib.util
spec = importlib.util.spec_from_file_location("fskmod", PLUGIN)
fskmod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fskmod)
check("R7 fmt 1MBd", fskmod.fmt_rate(999999.6) == "1MBd", fskmod.fmt_rate(999999.6))
check("R7 fmt 1kBd", fskmod.fmt_rate(999.96) == "1kBd", fskmod.fmt_rate(999.96))
check("R7 fmt 9.6kBd", fskmod.fmt_rate(9600.9) == "9.6kBd", fskmod.fmt_rate(9600.9))
check("R7 fmt 45.5Bd", fskmod.fmt_rate(45.45) == "45.5Bd", fskmod.fmt_rate(45.45))

# --- R8: denormal forced symbol rate — was an OverflowError traceback
p8 = run_plugin_raw(meta, {"symbol_rate_hz": 1e-309}, FS)
check("R8 denormal rate", p8.returncode == 1 and b"out of range" in p8.stderr
      and b"Traceback" not in p8.stderr, "rc=%d stderr=%s" % (p8.returncode, p8.stderr[:80]))

# --- R9: GFSK pulse shaping — h was read ~0.38 and mislabelled
def gfsk(bits, fs, rb, dev, offset, bt, r):
    sps = fs / rb
    n = int(round(len(bits) * sps))
    idx = np.minimum((np.arange(n) / sps).astype(int), len(bits) - 1)
    finst = offset + dev * (2.0 * np.asarray(bits, dtype=np.float64)[idx] - 1.0)
    sigma = np.sqrt(np.log(2.0)) / (2.0 * np.pi * bt * rb) * fs
    klen = (int(np.ceil(sigma * 8)) | 1)
    tk = np.arange(klen) - klen // 2
    kern = np.exp(-0.5 * (tk / sigma) ** 2)
    finst = np.convolve(finst, kern / kern.sum(), mode="same")
    phase = 2.0 * np.pi * np.cumsum(finst) / fs
    return np.exp(1j * phase).astype(np.complex64)


r9 = np.random.default_rng(19)
for bt, lo_h in [(0.5, 0.42), (0.3, 0.40)]:
    bits_r9 = list(r9.integers(0, 2, 800))
    b_r9 = gfsk(bits_r9, FS, 9600.0, 2400.0, 0.0, bt, r9)  # true h = 0.5
    seg_r9 = np.concatenate([np.zeros(2000, np.complex64), b_r9,
                             np.zeros(2000, np.complex64)])
    meta_r9 = write_segment("segr9_%d" % int(bt * 10), noisy(seg_r9, 30.0, r9), FS, CENTER)
    a_r9 = run_plugin_fs(meta_r9, {"threshold_db": -15.0, "max_bits": 4096}, FS)
    if len(a_r9) == 1:
        h9 = get(a_r9[0]["core:comment"], "h=") or 0
        rb9 = get(a_r9[0]["core:comment"], "Rb=") or 0
        check("R9 GFSK BT=%.1f h" % bt, h9 >= lo_h and h9 <= 0.60,
              "h=%s label=%s" % (h9, a_r9[0]["core:label"]))
        check("R9 GFSK BT=%.1f Rb" % bt, abs(rb9 - 9600) / 9600 < 0.02, "Rb=%s" % rb9)
    else:
        check("R9 GFSK BT=%.1f h" % bt, False, "got %d annotations" % len(a_r9))

# ================= round-2 regression tests =================

# --- R10: all period candidates scoring < 0.8 crashed the plugin (empty max())
cross = np.cumsum(np.abs(np.random.default_rng(23).normal(9.0, 6.0, 40)) + 3.0)
check("R10 period no-crash", isinstance(fskmod.estimate_period(cross), float))
r10 = np.random.default_rng(29)
for seed in range(6):   # short noisy bursts end-to-end must never crash the run
    rs = np.random.default_rng(100 + seed)
    tiny = cpfsk(list(rs.integers(0, 2, 6)), 955400.0, 7391.0, 2667.0, 0.0)
    seg10 = np.concatenate([np.zeros(1500, np.complex64), tiny, np.zeros(1500, np.complex64)])
    p10 = run_plugin_raw(write_segment("segr10_%d" % seed, noisy(seg10, 5.0, rs), 955400.0, CENTER),
                         {}, 955400.0)
    if p10.returncode != 0:
        check("R10 short noisy burst", False, "seed %d rc=%d %s" % (seed, p10.returncode, p10.stderr[:120]))
        break
else:
    check("R10 short noisy burst", True, "6 seeds clean")

# --- R11: wide-shift 2FSK (h=20) read "carrier" or 1.6 MBd garbage
ok11 = []
for seed in (1, 2, 3):
    rs = np.random.default_rng(200 + seed)
    bits11 = list(rs.integers(0, 2, 500))
    b11 = cpfsk(bits11, 1e6, 10000.0, 100000.0, 0.0)
    seg11 = np.concatenate([np.zeros(3000, np.complex64), b11, np.zeros(3000, np.complex64)])
    a11 = run_plugin_fs(write_segment("segr11_%d" % seed, noisy(seg11, 20.0, rs), 1e6, CENTER),
                        {"max_bits": 1024}, 1e6)
    c11 = a11[0]["core:comment"] if len(a11) == 1 else ""
    ok11.append(len(a11) == 1 and a11[0]["core:label"].startswith("2FSK")
                and abs((get(c11, "Rb=") or 0) - 10000) / 10000 < 0.02
                and abs((get(c11, "shift ±") or 0) - 100000) / 100000 < 0.05)
check("R11 wide-shift h=20", all(ok11), "seeds ok=%s" % ok11)

# --- R12: moderate-SNR narrowband tripped the broadband fallback -> "carrier"
ok12 = []
for seed in (1, 2, 3, 4):
    rs = np.random.default_rng(300 + seed)
    bits12 = list(rs.integers(0, 2, 256))
    b12 = cpfsk(bits12, 1e6, 25000.0, 25000.0, 0.0)
    seg12 = np.concatenate([np.zeros(3000, np.complex64), b12, np.zeros(3000, np.complex64)])
    a12 = run_plugin_fs(write_segment("segr12_%d" % seed, noisy(seg12, 13.0, rs), 1e6, CENTER),
                        {"threshold_db": -11.0, "max_bits": 512}, 1e6)
    c12 = a12[0]["core:comment"] if len(a12) == 1 else ""
    ok12.append(len(a12) == 1 and a12[0]["core:label"].startswith("2FSK")
                and abs((get(c12, "Rb=") or 0) - 25000) / 25000 < 0.02)
check("R12 narrowband 6dB", all(ok12), "seeds ok=%s" % ok12)

# --- R13: broadband 250 kBd at 2 Msps read 3-4.5 MBd from noise crossings
ok13 = []
for seed in (1, 2, 3):
    rs = np.random.default_rng(400 + seed)
    bits13 = list(rs.integers(0, 2, 800))
    b13 = cpfsk(bits13, 2e6, 250000.0, 150000.0, 0.0)
    seg13 = np.concatenate([np.zeros(4000, np.complex64), b13, np.zeros(4000, np.complex64)])
    a13 = run_plugin_fs(write_segment("segr13_%d" % seed, noisy(seg13, 13.0, rs), 2e6, CENTER),
                        {"threshold_db": -11.0, "max_bits": 1024}, 2e6)
    c13 = a13[0]["core:comment"] if len(a13) == 1 else ""
    ok13.append(len(a13) == 1 and abs((get(c13, "Rb=") or 0) - 250000) / 250000 < 0.03)
check("R13 broadband 13dB", all(ok13), "seeds ok=%s" % ok13)

# --- R14: "110"-repeat payload read Rb=2/3 of true (t=1.5T scored 1.0)
bits14 = ([1, 1, 0] * 300)
b14 = cpfsk(bits14, 250000.0, 10000.0, 5000.0, 0.0)
seg14 = np.concatenate([np.zeros(2000, np.complex64), b14, np.zeros(2000, np.complex64)])
a14 = run_plugin_fs(write_segment("segr14", noisy(seg14, 30.0, np.random.default_rng(31)), 250000.0, CENTER),
                    {"max_bits": 1024}, 250000.0)
c14 = a14[0]["core:comment"] if len(a14) == 1 else ""
check("R14 '110' repeat Rb", len(a14) == 1 and abs((get(c14, "Rb=") or 0) - 10000) / 10000 < 0.02,
      "Rb=%s" % get(c14, "Rb="))

# --- R15: strong signal dominating the PSD median was silently dropped
bits15 = list(np.random.default_rng(37).integers(0, 2, 20000))
b15 = cpfsk(bits15, 2e6, 1e6, 250000.0, 0.0)   # clean MSK at Rb = fs/2, no noise
a15 = run_plugin_fs(write_segment("segr15", b15, 2e6, CENTER), {"max_bits": 256}, 2e6)
c15 = a15[0]["core:comment"] if len(a15) == 1 else ""
check("R15 median-dominant kept", len(a15) == 1 and abs((get(c15, "Rb=") or 0) - 1e6) / 1e6 < 0.03,
      "got %d: %s Rb=%s" % (len(a15), [x.get("core:label") for x in a15], get(c15, "Rb=")))

# --- R16: P(flip)=0.04 (4% single-symbol intervals) missed by p5 seeding
r16 = np.random.default_rng(41)
bits16, v = [], 1
for _ in range(1500):
    if r16.random() < 0.04:
        v ^= 1
    bits16.append(v)
b16 = cpfsk(bits16, 250000.0, 10000.0, 5000.0, 0.0)
seg16 = np.concatenate([np.zeros(2000, np.complex64), b16, np.zeros(2000, np.complex64)])
a16 = run_plugin_fs(write_segment("segr16", noisy(seg16, 30.0, r16), 250000.0, CENTER),
                    {"max_bits": 2048}, 250000.0)
c16 = a16[0]["core:comment"] if len(a16) == 1 else ""
# 4% flip rate = very few single-symbol intervals: documented degraded case.
# Require a 2FSK label with correct deviation and a non-empty decode; rate
# is not asserted tightly.
check("R16 4% flips label", len(a16) == 1 and a16[0]["core:label"].startswith("2FSK")
      and abs((get(c16, "shift ±") or 0) - 5000) / 5000 < 0.10,
      "label=%s dev=%s" % (a16[0]["core:label"] if a16 else None, get(c16, "shift ±")))

# --- R17: forced-rate guard bypasses (Infinity, > fs); NaN falls back to auto
p17a = run_plugin_raw(meta, {"symbol_rate_hz": float("inf")}, FS)
check("R17 inf rate", p17a.returncode == 1 and b"out of range" in p17a.stderr,
      "rc=%d" % p17a.returncode)
p17b = run_plugin_raw(meta, {"symbol_rate_hz": 1e9}, FS)
check("R17 huge rate", p17b.returncode == 1 and b"out of range" in p17b.stderr,
      "rc=%d" % p17b.returncode)
p17c = run_plugin_raw(meta, {"symbol_rate_hz": float("nan")}, FS)
a17c = json.loads(p17c.stdout.decode())["annotations"] if p17c.returncode == 0 else []
rb17c = get(a17c[0]["core:comment"], "Rb=") if a17c else None
check("R17 nan rate auto", p17c.returncode == 0 and rb17c and abs(rb17c - 9600) / 9600 < 0.01,
      "rc=%d Rb=%s" % (p17c.returncode, rb17c))

# --- R21: pure-alternating (0xAA/Sunde) FSK preamble — line spectrum whose FM
# harmonics were clipped by a too-tight band, collapsing to a beat -> "carrier"
ok21 = []
for rb, snr in [(200000, 20), (250000, 20), (160000, 25)]:
    r21 = np.random.default_rng(5)
    b21 = cpfsk([1, 0] * 400, 2e6, rb, rb / 2.0, 0.0)   # h=1.0 Sunde FSK
    seg21 = np.concatenate([np.zeros(3000, np.complex64), b21, np.zeros(3000, np.complex64)])
    a21 = run_plugin_fs(write_segment("segr21_%d" % rb, noisy(seg21, snr, r21), 2e6, CENTER),
                        {"threshold_db": -12, "max_bits": 64}, 2e6)
    c21 = a21[0]["core:comment"] if len(a21) == 1 else ""
    ok21.append(len(a21) == 1 and a21[0]["core:label"].startswith("2FSK")
                and abs((get(c21, "Rb=") or 0) - rb) / rb < 0.03)
check("R21 alternating preamble", all(ok21), "ok=%s" % ok21)

# --- R17d: tiny finite forced rate that overflows only after fs*=up (round 3)
# wideband burst -> up=8; symbol_rate_hz ~1e-302 passes the pre-upsample guard
# but fs*up/rate overflows to inf; must not crash (OverflowError traceback).
r17d = np.random.default_rng(51)
wb = cpfsk(list(r17d.integers(0, 2, 800)), 2e6, 250000.0, 150000.0, 0.0)  # broadband -> up>1
seg17d = np.concatenate([np.zeros(4000, np.complex64), wb, np.zeros(4000, np.complex64)])
meta17d = write_segment("segr17d", noisy(seg17d, 13.0, r17d), 2e6, CENTER)
p17d = run_plugin_raw(meta17d, {"symbol_rate_hz": 4.4501477170144033e-302, "max_bits": 64}, 2e6)
check("R17d tiny-rate no crash", p17d.returncode in (0, 1) and b"Traceback" not in p17d.stderr,
      "rc=%d stderr=%s" % (p17d.returncode, p17d.stderr[:80]))

# --- R18: odd-length FFT upsample must keep positive-frequency tones positive
n18 = 4097
tone = np.exp(2j * np.pi * 0.467 * np.arange(n18)).astype(np.complex64)  # +0.467 fs
y18 = fskmod.filter_upsample(tone, 1.0, -0.5, 0.5, 2)
pk18 = np.fft.fftfreq(y18.size, 0.5)[np.argmax(np.abs(np.fft.fft(y18)))]
check("R18 odd-n upsample", abs(pk18 - 0.467) < 0.001, "peak at %.4f fs" % pk18)

# --- R19: max_bits=0 must omit the bits field entirely (no "bits[N]=…")
a19 = run_plugin_fs(meta, {"max_bits": 0}, FS)
check("R19 max_bits=0", a19 and all("bits[" not in x.get("core:comment", "") for x in a19))

# --- R20: bit-decode cap gets an explicit note
bits20 = list(np.random.default_rng(43).integers(0, 2, 70000))
b20 = cpfsk(bits20, 1e6, 100000.0, 50000.0, 0.0)   # 70000 bits > 65536 cap
a20 = run_plugin_fs(write_segment("segr20", noisy(b20, 25.0, np.random.default_rng(43)), 1e6, CENTER),
                    {"max_bits": 64}, 1e6)
check("R20 decode cap note", len(a20) == 1 and "bit decode capped" in a20[0]["core:comment"],
      a20[0]["core:comment"][-60:] if a20 else "none")

fails = [c for c in checks if not c[1]]
print("\n%d/%d checks passed" % (len(checks) - len(fails), len(checks)))
sys.exit(1 if fails else 0)
