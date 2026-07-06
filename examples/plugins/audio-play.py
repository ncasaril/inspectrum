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
"""inspectrum audio playback plugin: demodulate a tuned carrier and play it.

Reads the tuned/filtered baseband IQ inspectrum hands it — a station mixed to
0 Hz by the tuner (drag a box over the signal: this manifest sets
"wants_band": true) — demodulates it as broadcast FM, narrowband FM, or AM, and
plays the recovered audio through the system's audio device. One annotation is
returned marking the region that was played.

Contract (see doc/plugins.md):
  argv[-1] : path to the segment's .sigmf-meta (cf32_le .sigmf-data alongside)
  stdin    : JSON { "sample_rate", "center_freq", "custom_params": {...} }
  stdout   : JSON { "annotations": [...] }        -- sample indices SEGMENT-LOCAL

custom_params:
  mode          : "WFM" | "NFM" | "AM"                       (default "WFM")
  loop          : repeat the audio until the run is cancelled (default true)
  deemphasis_us : FM de-emphasis time constant, 0 disables   (default 75 for
                  WFM, 50 for NFM; "auto" picks per mode)
  audio_lpf_hz  : audio low-pass cutoff, 0 = mode default     (default 0)
  gain_db       : extra gain applied after peak-normalisation (default 0)
  squelch_db    : mute audio whose local envelope is this many dB below the
                  segment peak; 0 disables                    (default 0)
  max_seconds   : cap the played span per pass                (default 30)

The manifest sets "long_running" so the host disables its watchdog timeout; the
run then plays (looping by default) until you hit Cancel, which kills this
process and the player child with it (PR_SET_PDEATHSIG). Progress is reported to
the host busy dialog via \x1e-prefixed stderr lines ("Preparing audio", then
"Playing (loop N)...") so you can tell the prepare and playback phases apart even
when a passage is silent.
"""

import json
import os
import shutil
import signal
import subprocess
import sys

import numpy as np
from scipy.signal import firwin, lfilter, resample_poly

AUDIO_RATE = 48000
# Player command templates, tried in order; each reads raw interleaved s16le
# mono at AUDIO_RATE from stdin. play() falls through to the next on failure.
PLAYERS = [
    ("aplay",   ["aplay", "-q", "-t", "raw", "-f", "S16_LE",
                 "-r", str(AUDIO_RATE), "-c", "1", "-"]),
    ("pw-play", ["pw-play", "--format", "s16", "--rate", str(AUDIO_RATE),
                 "--channels", "1", "-"]),
    ("paplay",  ["paplay", "--raw", "--format=s16le",
                 "--rate=%d" % AUDIO_RATE, "--channels=1"]),
]

_player_proc = None  # so the signal handler can kill it on Cancel
_player_argv = None  # the player command found to work, reused across loop passes


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
    center_freq = float(caps[0].get("core:frequency", 0.0)) if caps else 0.0
    datatype = g.get("core:datatype", "cf32_le")
    return data_path, sample_rate, center_freq, datatype


def resample_ratio(fs_in, fs_out):
    """Integer up/down factors for resample_poly, robust to non-integer fs."""
    from fractions import Fraction
    fr = Fraction(int(round(fs_out)), max(1, int(round(fs_in)))).limit_denominator(100000)
    return max(1, fr.numerator), max(1, fr.denominator)


def to_audio_rate(sig, fs):
    # Cheap integer pre-decimation for very high input rates (e.g. a 20 MHz
    # capture), each stage anti-aliased, then one fine resample to AUDIO_RATE.
    # A single resample_poly 20e6->48e3 would need a ~50k-tap filter; staging
    # keeps every stage short so a multi-second box stays responsive.
    x = sig
    rate = float(fs)
    while rate > 8 * AUDIO_RATE:
        d = int(min(16, rate // (4 * AUDIO_RATE)))
        if d < 2:
            break
        x = resample_poly(x, 1, d)
        rate /= d
    up, down = resample_ratio(rate, AUDIO_RATE)
    if up == down:
        return x.astype(np.float32)
    return resample_poly(x, up, down).astype(np.float32)


def demod(x, fs, mode, deemph_us, lpf_hz):
    """IQ -> real audio at AUDIO_RATE for one of WFM / NFM / AM."""
    is_fm = mode != "AM"
    if not is_fm:
        # Envelope detector; the DC block below removes the carrier level.
        base = np.abs(x).astype(np.float32)
    else:
        # Quadrature FM discriminator: phase step per sample -> instantaneous freq.
        d = x[1:] * np.conj(x[:-1])
        inst = np.angle(d).astype(np.float32)         # rad/sample
        base = np.concatenate(([0.0], inst)).astype(np.float32)

    audio = to_audio_rate(base, fs)

    # Audio-band low-pass (kills the 19 kHz FM stereo pilot for WFM, tightens NFM).
    if lpf_hz <= 0:
        lpf_hz = {"WFM": 15000.0, "NFM": 4000.0, "AM": 5000.0}[mode]
    lpf_hz = min(lpf_hz, 0.45 * AUDIO_RATE)
    taps = firwin(129, lpf_hz / (0.5 * AUDIO_RATE))
    audio = lfilter(taps, [1.0], audio).astype(np.float32)

    # DC block: removes the residual carrier offset (FM) / carrier level (AM).
    audio = audio - float(np.mean(audio))

    # Peak FM deviation, measured on the audio-band discriminator output so that
    # broadband click/noise spikes (which sit outside the voice band) don't inflate
    # it. The values are rad per source-sample, so scale by fs/2pi to get Hz.
    dev_hz = float(np.percentile(np.abs(audio), 99) * fs / (2.0 * np.pi)) if is_fm else 0.0

    # FM de-emphasis (single-pole IIR) at the audio rate.
    if is_fm and deemph_us > 0:
        tau = deemph_us * 1e-6
        alpha = 1.0 - np.exp(-1.0 / (AUDIO_RATE * tau))
        audio = lfilter([alpha], [1.0, -(1.0 - alpha)], audio).astype(np.float32)

    return audio, dev_hz


def apply_squelch(audio, x, fs, squelch_db):
    """Zero audio wherever the input envelope sits below the gate (dB vs peak)."""
    if squelch_db >= 0:
        return audio, False
    env = np.abs(x).astype(np.float32)
    # Smooth the envelope over ~2 ms, then resample the mask to the audio grid.
    win = max(1, int(0.002 * fs))
    smooth = np.convolve(env, np.ones(win, np.float32) / win, mode="same")
    peak = float(smooth.max()) if smooth.size else 0.0
    if peak <= 0:
        return audio, False
    gate = peak * (10.0 ** (squelch_db / 20.0))
    open_mask = (smooth >= gate).astype(np.float32)
    m = to_audio_rate(open_mask, fs)
    m = np.clip(m[:audio.size], 0.0, 1.0)
    if m.size < audio.size:
        m = np.concatenate([m, np.zeros(audio.size - m.size, np.float32)])
    gated = audio * m
    return gated, bool(np.any(m < 0.5))


def to_pcm16(audio, gain_db):
    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    if peak > 0:
        audio = audio / peak * 0.9
    audio = audio * (10.0 ** (gain_db / 20.0))
    audio = np.clip(audio, -1.0, 1.0)
    return (audio * 32767.0).astype("<i2")


PROGRESS_MARKER = "\x1e"  # inspectrum reads \x1e-prefixed stderr lines as progress


def progress(text):
    """Report a phase to the host busy dialog (a marked stderr line)."""
    try:
        sys.stderr.write(PROGRESS_MARKER + text + "\n")
        sys.stderr.flush()
    except Exception:
        pass


def _child_setup():
    # Die with the parent: inspectrum's Cancel SIGKILLs us (uncatchable), so ask
    # the kernel to signal the player when this process goes away — otherwise the
    # player would keep sounding after Cancel. Linux-only; harmless elsewhere.
    try:
        import ctypes
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    except Exception:
        pass


def play_once(pcm):
    """Play one pass; return the player name. Remembers the working player so a
    loop doesn't re-probe the failing ones each pass."""
    global _player_proc, _player_argv
    buf = pcm.tobytes()
    step = AUDIO_RATE * 2  # ~0.5 s of s16 mono per write, so Cancel is responsive
    candidates = ([_player_argv] if _player_argv else
                  [a for _, a in PLAYERS if shutil.which(a[0])])
    last_err = "no audio player found (looked for aplay, pw-play, paplay)"
    for argv in candidates:
        try:
            _player_proc = subprocess.Popen(
                argv, stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                preexec_fn=_child_setup)
        except OSError as e:
            last_err = "%s: %s" % (argv[0], e)
            continue
        try:
            for i in range(0, len(buf), step):
                _player_proc.stdin.write(buf[i:i + step])
            _player_proc.stdin.close()
            rc = _player_proc.wait()
        except BrokenPipeError:
            rc = _player_proc.wait()  # player exited early
        if rc == 0:
            _player_argv = argv
            return argv[0]
        last_err = "%s exited %d" % (argv[0], rc)
    raise RuntimeError(last_err)


def _on_term(signum, frame):
    # inspectrum's Cancel kills this process; take the player child down too.
    if _player_proc and _player_proc.poll() is None:
        try:
            _player_proc.terminate()
        except Exception:
            pass
    sys.exit(0)


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: audio-play.py <segment.sigmf-meta>\n")
        return 2
    signal.signal(signal.SIGTERM, _on_term)
    signal.signal(signal.SIGINT, _on_term)

    meta_path = sys.argv[-1]
    raw = sys.stdin.read()
    ctx = json.loads(raw) if raw.strip() else {}
    params = ctx.get("custom_params", {}) or {}

    mode = str(params.get("mode", "WFM")).upper()
    if mode not in ("WFM", "NFM", "AM"):
        sys.stderr.write("audio-play: unknown mode %r\n" % mode)
        return 1
    deemph_raw = params.get("deemphasis_us", "auto")
    if isinstance(deemph_raw, str) and deemph_raw.lower() == "auto":
        deemph_us = {"WFM": 75.0, "NFM": 50.0, "AM": 0.0}[mode]
    else:
        deemph_us = float(deemph_raw)
    lpf_hz = float(params.get("audio_lpf_hz", 0.0))
    gain_db = float(params.get("gain_db", 0.0))
    squelch_db = float(params.get("squelch_db", 0.0))
    max_seconds = float(params.get("max_seconds", 30.0))
    loop = bool(params.get("loop", True))

    data_path, sample_rate, center_freq, datatype = load_meta(meta_path)
    if datatype != "cf32_le":
        sys.stderr.write("audio-play: expected cf32_le, got %s\n" % datatype)
        return 1
    if sample_rate <= 0:
        sample_rate = float(ctx.get("sample_rate", 0.0))
    if sample_rate <= 0:
        sys.stderr.write("audio-play: missing sample_rate\n")
        return 1

    nbytes = os.path.getsize(data_path)
    n_total = nbytes // 8  # complex64
    if n_total == 0:
        print(json.dumps({"annotations": []}))
        return 0

    # Cap the played span (per loop pass) so a long box stays manageable.
    progress("Preparing audio")
    n_cap = int(max_seconds * sample_rate) if max_seconds > 0 else n_total
    n = min(n_total, n_cap)
    truncated = n < n_total
    x = np.memmap(data_path, dtype=np.complex64, mode="r", shape=(n_total,))[:n]
    x = np.asarray(x, dtype=np.complex64)

    audio, dev_hz = demod(x, sample_rate, mode, deemph_us, lpf_hz)
    audio, gated = apply_squelch(audio, x, sample_rate, squelch_db)
    pcm = to_pcm16(audio, gain_db)
    dur_s = n / sample_rate

    # Playback. When loop is set (default), repeat until the host cancels the run
    # (which kills this process -> the player dies with it). play_once returns after
    # each pass; a play failure stops the loop and is reported in the annotation.
    played_by = None
    err = None
    pass_n = 0
    while True:
        pass_n += 1
        if loop:
            progress("Playing (loop %d, %.1f s) — Cancel to stop" % (pass_n, dur_s))
        else:
            progress("Playing (%.1f s)" % dur_s)
        try:
            played_by = play_once(pcm)
        except Exception as e:
            err = str(e)
            sys.stderr.write("audio-play: %s\n" % err)
            break
        if not loop:
            break

    # Reached on single-shot completion or a play failure (a looping run that plays
    # fine never gets here — it ends via Cancel, so the host adds no annotation).
    parts = ["%s @ %.6g MHz" % (mode, center_freq / 1e6)]
    parts.append("%.2f s audio (%.0f kHz IQ)" % (dur_s, sample_rate / 1e3))
    if mode != "AM":
        parts.append("peak dev ~%.1f kHz" % (dev_hz / 1e3))
        if deemph_us > 0:
            parts.append("de-emphasis %.0f us" % deemph_us)
    if gated:
        parts.append("squelch gated")
    if truncated:
        parts.append("capped to %.0f s (raise max_seconds)" % max_seconds)
    parts.append("played via %s" % played_by if played_by else "NOT played: %s" % err)

    ann = {
        "core:sample_start": 0,
        "core:sample_count": int(n),
        "core:label": "%s audio" % mode,
        "core:comment": "audio-play:\n  " + "\n  ".join(parts),
        "core:generator": "inspectrum audio-play.py",
        "presentation:color": "#33CC66A0",
    }
    json.dump({"annotations": [ann]}, sys.stdout)
    sys.stdout.write("\n")
    return 0 if err is None else 1


if __name__ == "__main__":
    sys.exit(main())
