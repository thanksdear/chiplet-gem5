#!/usr/bin/env python3
"""Plot latency vs injection rate curves (paper quality).

Usage: python plot_latency_curves.py <input_csv> <traffic_pattern> <output_pdf>
"""

import sys
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# ---- Visual encoding (strict) ----
STYLES = {
    "MTR": {
        "color": '#7f7f7f',
        "marker": '^',
        "ls": '-',
        "label": 'MTR',
        "markersize": 7,
        "zorder": 2,
    },
    "RC": {
        "color": '#1f77b4',
        "marker": 's',
        "ls": '-',
        "label": 'RC',
        "markersize": 6,
        "zorder": 3,
    },
    "IPDR": {
        "color": '#2ca02c',
        "marker": 'D',
        "ls": '-',
        "label": 'IPDR',
        "markersize": 6,
        "zorder": 4,
    },
    "IPDR+HAR": {
        "color": '#17becf',
        "marker": 'X',
        "ls": '-',
        "label": 'IPDR+HAR',
        "markersize": 7,
        "zorder": 6,
    },
    "LBDR": {
        "color": '#E69F00',
        "marker": 'p',
        "ls": '-',
        "label": 'LBDR',
        "markersize": 7,
        "zorder": 5,
    },
    "UHAF-base": {
        "color": '#ff7f0e',
        "marker": 'v',
        "ls": '-',
        "label": 'UHAF-base',
        "markersize": 7,
        "zorder": 5,
    },
    "uhafa=0": {
        "color": '#1f77b4',
        "marker": 's',
        "ls": '-',
        "label": 'UHAF (Stall-only)',
        "markersize": 6,
        "zorder": 5,
    },
    "uhafa=1": {
        "color": '#9467bd',
        "marker": 'D',
        "ls": '-',
        "label": 'UHAF (Separate Monitors)',
        "markersize": 6,
        "zorder": 6,
    },
    "UHAFa=0": {
        "color": '#1f77b4',
        "marker": 's',
        "ls": '-',
        "label": 'UHAF (Stall-only)',
        "markersize": 6,
        "zorder": 5,
    },
    "UHAFa=1": {
        "color": '#9467bd',
        "marker": 'D',
        "ls": '-',
        "label": 'UHAF (Separate Monitors)',
        "markersize": 6,
        "zorder": 6,
    },
    "Steered Bubble": {
        "color": '#9467bd',
        "marker": 'P',
        "ls": '-',
        "label": 'Steered Bubble',
        "markersize": 6,
        "zorder": 6,
    },
    "UHAF": {
        "color": '#d62728',
        "marker": 'o',
        "ls": '-',
        "label": 'UHAF',
        "markersize": 7,
        "zorder": 7,
    },
    "UHAF-01": {
        "color": '#7f7f7f',
        "marker": '^',
        "ls": '-',
        "label": 'Bias (0,1)',
        "markersize": 6,
        "zorder": 3,
    },
    "UHAF-11": {
        "color": '#1f77b4',
        "marker": 's',
        "ls": '-',
        "label": 'Bias (1,1)',
        "markersize": 6,
        "zorder": 4,
    },
    "UHAF-12": {
        "color": '#d62728',
        "marker": 'o',
        "ls": '-',
        "label": 'Bias (2,1)',
        "markersize": 7,
        "zorder": 6,
    },
    "UHAF-22": {
        "color": '#2ca02c',
        "marker": 'D',
        "ls": '-',
        "label": 'Bias (2,2)',
        "markersize": 6,
        "zorder": 5,
    },
}

def setup_rc():
    plt.rcParams.update({
        "font.size": 13,
        "font.family": "serif",
        "font.serif": ["Nimbus Roman", "DejaVu Serif"],
        "mathtext.fontset": "stix",
        "axes.linewidth": 1.0,
        "lines.linewidth": 1.5,
        "lines.markersize": 6,
        "legend.frameon": False,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })

# ---- Scheme groups ----
SCHEME_GROUPS = {
    "ipdr_har": ["IPDR", "IPDR+HAR", "UHAF"],
    "all":  ["MTR", "RC", "IPDR", "UHAF-base", "Steered Bubble", "UHAF"],
    "algo": ["MTR", "RC", "IPDR", "LBDR", "Steered Bubble", "UHAF"],
    "base": ["UHAF-base", "UHAF"],
    "ablation": ["UHAF-base", "uhafa=0", "uhafa=1", "UHAF"],
    "ablation_2vc": ["UHAF-base", "UHAFa=0", "UHAFa=1", "UHAF"],
    "bias": ["UHAF-11", "UHAF-12", "UHAF-22", "UHAF-01"],
}

def plot(csv_path, traffic, out_pdf, schemes=None, ymin=0, ymax=150):
    setup_rc()
    df = pd.read_csv(csv_path)
    df = df[df["traffic"] == traffic].copy()
    if df.empty:
        print(f"No data for traffic={traffic}")
        sys.exit(1)

    if schemes is None:
        schemes = SCHEME_GROUPS["all"]

    fig, ax = plt.subplots(figsize=(5, 3.5))

    for scheme in schemes:
        if scheme not in STYLES:
            continue
        sty = STYLES[scheme]
        sub = df[df["routing"] == scheme].sort_values("injection_rate")
        if sub.empty:
            continue
        x = sub["injection_rate"].values
        y = sub["avg_latency"].values

        kw = dict(color=sty["color"], marker=sty["marker"],
                  linestyle=sty["ls"], label=sty["label"],
                  linewidth=1.5,
                  markersize=sty.get("markersize", 6),
                  zorder=sty.get("zorder", 3))
        if "fillstyle" in sty:
            kw["fillstyle"] = sty["fillstyle"]
        if "markeredgecolor" in sty:
            kw["markeredgecolor"] = sty["markeredgecolor"]
            kw["markeredgewidth"] = sty.get("markeredgewidth", 1.0)
        ax.plot(x, y, **kw)

    ax.set_xlabel("Injection rate (packets/node/cycle)")
    ax.set_ylabel("Average latency (cycles)")
    ax.set_ylim(ymin, ymax)
    if ymin == 0 and ymax == 75:
        ax.set_yticks([0, 25, 50, 75])
    elif ymin == 25 and ymax == 50:
        ax.set_yticks([25, 30, 35, 40, 45, 50])

    xmax = df["injection_rate"].max() + 0.005
    ax.set_xlim(0, xmax)

    ax.grid(True, linestyle=":", alpha=0.4)
    ax.legend(loc="upper left", fontsize=9, handlelength=2.5)

    fig.tight_layout()
    fig.savefig(out_pdf, bbox_inches="tight")
    png_path = out_pdf.replace(".pdf", ".png")
    fig.savefig(png_path, dpi=300, bbox_inches="tight")
    print(f"Saved: {out_pdf}, {png_path}")
    plt.close(fig)


def plot_group(csv_path, traffic, out_base, group, ymin=0, ymax=150):
    """Plot a scheme group, saving to out_base + '_' + group + '.pdf'."""
    schemes = SCHEME_GROUPS.get(group)
    if schemes is None:
        print(f"Unknown group: {group}. "
              f"Choose from {list(SCHEME_GROUPS.keys())}")
        sys.exit(1)
    out_pdf = out_base.replace(".pdf", f"_{group}.pdf")
    plot(csv_path, traffic, out_pdf, schemes, ymin, ymax)


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage:")
        print("  python plot_latency_curves.py <csv> <traffic> <output.pdf>")
        print("  python plot_latency_curves.py <csv> <traffic> <output.pdf> "
              "[--group <group>] [--ymin <cycles>] [--ymax <cycles>]")
        sys.exit(1)

    csv_path = sys.argv[1]
    traffic = sys.argv[2]
    out_pdf = sys.argv[3]
    ymin = 0.0
    ymax = 150.0

    if "--ymin" in sys.argv:
        yi = sys.argv.index("--ymin")
        ymin = float(sys.argv[yi + 1])

    if "--ymax" in sys.argv:
        yi = sys.argv.index("--ymax")
        ymax = float(sys.argv[yi + 1])
        if ymax <= 0:
            raise ValueError("--ymax must be positive")
    if ymin >= ymax:
        raise ValueError("--ymin must be smaller than --ymax")

    if "--group" in sys.argv:
        gi = sys.argv.index("--group")
        group = sys.argv[gi + 1]
        plot_group(csv_path, traffic, out_pdf, group, ymin, ymax)
    else:
        plot(csv_path, traffic, out_pdf, ymin=ymin, ymax=ymax)
