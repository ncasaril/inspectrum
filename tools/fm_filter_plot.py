#!/usr/bin/env python3
"""Plot raw float32 sample dumps from fm_filter_compare side by side.

Usage:
  python3 tools/fm_filter_plot.py /tmp/fm_out/*.f32

By default plots the first 200k samples; pass --start / --count to scrub.
"""

import argparse
import os
import sys

import matplotlib.pyplot as plt
import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", help="float32 sample dumps")
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--count", type=int, default=200_000)
    ap.add_argument("--fs", type=float, default=10e6, help="sample rate (Hz)")
    args = ap.parse_args()

    fig, axes = plt.subplots(len(args.files), 1, figsize=(14, 2.0 * len(args.files)),
                             sharex=True)
    if len(args.files) == 1:
        axes = [axes]
    for ax, path in zip(axes, args.files):
        data = np.fromfile(path, dtype=np.float32)
        end = min(args.start + args.count, len(data))
        sl = data[args.start:end]
        t = np.arange(args.start, end) / args.fs
        ax.plot(t, sl, linewidth=0.5)
        ax.set_title(os.path.basename(path), fontsize=9)
        ax.grid(True, alpha=0.3)
        ax.set_ylabel("value")
    axes[-1].set_xlabel("time (s)")
    fig.tight_layout()
    plt.show()


if __name__ == "__main__":
    main()
