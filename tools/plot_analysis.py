#!/usr/bin/env python3
"""Plot a glina analysis JSON (pitch curve, note blobs, frame kinds), Melodyne-style.

usage: plot_analysis.py analysis.json out.png [--truth truth.json] [--title "..."]
"""
import argparse
import json
import math

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch


def hz2midi(f):
    return 69 + 12 * math.log2(f / 440.0) if f > 0 else float("nan")


NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

ap = argparse.ArgumentParser()
ap.add_argument("analysis")
ap.add_argument("out")
ap.add_argument("--truth")
ap.add_argument("--title", default="")
args = ap.parse_args()

a = json.load(open(args.analysis))
hop_s = a["hop"] / a["sampleRate"]
fr = a["frames"]
t = [i * hop_s for i in range(len(fr))]
midi = [hz2midi(f[0]) for f in fr]
edited = [f[4] if len(f) > 4 and f[4] > 0 else float("nan") for f in fr]

fig, (ax, ax2) = plt.subplots(2, 1, figsize=(13, 6.2), sharex=True,
                              gridspec_kw={"height_ratios": [4, 1]})
fig.patch.set_facecolor("#14161a")
for x in (ax, ax2):
    x.set_facecolor("#1b1e24")
    x.tick_params(colors="#aab")
    for s in x.spines.values():
        s.set_color("#334")

lo = min([n["center"] for n in a["notes"]] or [55]) - 3
hi = max([n["center"] for n in a["notes"]] or [75]) + 3
for m in range(int(lo), int(hi) + 1):
    black = NAMES[m % 12].endswith("#")
    ax.axhspan(m - 0.5, m + 0.5, color="#20242b" if black else "#1b1e24", zorder=0)

for n in a["notes"]:
    w = n["end"] - n["start"]
    ax.add_patch(FancyBboxPatch((n["start"], n["center"] - 0.32), w, 0.64,
                                boxstyle="round,pad=0,rounding_size=0.06",
                                fc="#e8a33d", ec="#ffcf80", alpha=0.35, lw=1, zorder=1))
if args.truth:
    tr = json.load(open(args.truth))
    for n in tr["notes"]:
        ax.plot([n["start"], n["end"]], [n["midi"]] * 2, color="#7fd17f", lw=1.2, ls="--", zorder=2)
ax.plot(t, midi, color="#e8e8f0", lw=1.0, zorder=3, label="detected pitch")
if any(not math.isnan(v) for v in edited):
    ax.plot(t, edited, color="#4fb3ff", lw=1.3, zorder=4, label="edited pitch")
ax.set_ylim(lo, hi)
ax.set_yticks(range(int(lo), int(hi) + 1))
ax.set_yticklabels([f"{NAMES[m % 12]}{m // 12 - 1}" for m in range(int(lo), int(hi) + 1)], fontsize=7)
ax.set_title(args.title or f"{len(a['notes'])} notes, key {a['key']}", color="#dde")
ax.legend(facecolor="#1b1e24", labelcolor="#ccd", edgecolor="#334", fontsize=8, loc="upper left")

kinds = {0: ("silence", "#2a2e36"), 1: ("voiced", "#e8a33d"), 2: ("sibilant", "#c45ad6"), 3: ("breath", "#5a8fd6")}
db = [f[2] for f in fr]
ax2.plot(t, db, color="#99a", lw=0.8)
for k, (name, col) in kinds.items():
    xs = [t[i] for i, f in enumerate(fr) if f[3] == k]
    ax2.scatter(xs, [-95] * len(xs), s=4, color=col, label=name, marker="s")
ax2.set_ylim(-100, 0)
ax2.set_ylabel("dB", color="#aab")
ax2.legend(facecolor="#1b1e24", labelcolor="#ccd", edgecolor="#334", fontsize=7, ncol=4, loc="upper right")
ax2.set_xlabel("time, s", color="#aab")
plt.tight_layout()
plt.savefig(args.out, dpi=110, facecolor=fig.get_facecolor())
