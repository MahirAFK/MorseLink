#!/usr/bin/env python3
r"""
MorseLink - the report's figures and tables from every test folder.

What it reads (anywhere under the folder it is given, subfolders included):
  - the phones' session files  ML_<session>_<rover|base>_<ID>.jsonl
    (read with analyse_tests.py, which must sit next to this file);
  - the run-down dumps (`test dump` text saved from the serial monitor);
  - the phyphox recordings of the motor test: folders with "Raw Data.csv"
    (and meta\time.csv) named M1, M2 and Strength;
  - Meter\Meter_test.txt: the current readings (1 ohm shunt);
  - participants.csv (any folder): the questionnaires, one row per participant
    (copy participants_TEMPLATE.csv, fill it in, rename it).

What it writes (next to this file, replaced on every run):
  figures\   one PDF per figure (for LaTeX) and a PNG of each (to look at)
  tables\    one .tex table body per table (\input in the report) and a CSV
  numbers.txt  the numbers quoted in the report's text, with where they came from

Set-up: Python 3.8+ with matplotlib (pip install matplotlib; it brings numpy).
Keep this file and analyse_tests.py in the Results folder, the test folders
(Baseline, Dynamic 1, Point sweep, RundownB, IMU, Meter, ...) next to them,
and run it, or double-click it:
    python make_figures.py                 (reads the folder it is in)
    python make_figures.py <results folder> [-o <output folder>]

Every function says what it takes, what it returns and what it changes.
"""

import argparse
import bisect
import csv
import datetime
import math
import os
import re
import statistics
import sys
import traceback
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # analyse_tests.py is found next to this file (also under python -I)
import analyse_tests as at  # noqa: E402  (the session reader, message pairing, lost periods, dumps)

try:
    import matplotlib
    matplotlib.use("Agg")  # no window: files only
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
    from matplotlib.legend_handler import HandlerTuple
    import numpy as np
except ImportError:  # the tables and numbers still need numpy-free code paths: stop clearly instead
    plt = None

# ---------------------------------------------------------------- the look of every figure

# Categorical colours in a fixed order (validated: adjacent pairs pass the
# colour-blindness checks; the first three pass all pairs). A series keeps its
# colour in every figure: SF9/17 dBm is always slot 1, SF9/22 slot 2, SF12/22 slot 3.
C = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
INK, INK2, MUTED, GRID = "#0b0b0b", "#52514e", "#8a8984", "#e6e5e1"
SETTING_STYLE = {  # (SF, dBm) -> colour, marker (the marker is the second channel for print)
    (9, 17): (C[0], "o"),
    (9, 22): (C[1], "s"),
    (12, 22): (C[2], "^"),
}
FIG_W = 6.2  # inches: the report's text width is 6.27 in, so fonts print at their size


def style():
    """Set matplotlib's defaults for every figure: thin marks, hairline solid
    grid, no top/right frame, 9 pt text. Changes matplotlib's global rcParams."""
    plt.rcParams.update({
        "font.family": "DejaVu Sans", "font.size": 8.5, "axes.titlesize": 9, "axes.labelsize": 8.5,
        "axes.titleweight": "bold", "axes.titlelocation": "left",
        "axes.edgecolor": "#bdbcb8", "axes.linewidth": 0.6, "axes.labelcolor": INK,
        "xtick.color": INK2, "ytick.color": INK2, "xtick.labelsize": 8, "ytick.labelsize": 8,
        "xtick.major.width": 0.6, "ytick.major.width": 0.6, "xtick.major.size": 3, "ytick.major.size": 3,
        "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "grid.linestyle": "-",
        "axes.axisbelow": True, "axes.spines.top": False, "axes.spines.right": False,
        "legend.frameon": False, "legend.fontsize": 7.5, "legend.handlelength": 1.8,
        "lines.linewidth": 1.5, "lines.solid_capstyle": "round", "lines.markersize": 4.5,
        "savefig.dpi": 200, "savefig.bbox": "tight", "savefig.pad_inches": 0.03,
        "pdf.fonttype": 42, "figure.dpi": 100,
    })


def save(fig, out, name):
    """Write one figure as figures/<name>.pdf (for LaTeX) and .png (to look
    at), then close it. Returns the PDF's path."""
    d = os.path.join(out, "figures")
    os.makedirs(d, exist_ok=True)
    pdf = os.path.join(d, name + ".pdf")
    fig.savefig(pdf)
    fig.savefig(os.path.join(d, name + ".png"))
    plt.close(fig)
    return pdf


# ---------------------------------------------------------------- small helpers

NUMBERS = []  # (key, value, where it came from): written to numbers.txt for the report's text


def note(key, value, source):
    """Remember a number quoted in the report (numbers.txt). Returns value."""
    NUMBERS.append((key, value, source))
    return value


def median(v):
    """Median of a list, None when empty."""
    return statistics.median(v) if v else None


def quantile(v, p):
    """The p-quantile (0..1) by the nearest-rank rule (as analyse_tests.q), None when empty."""
    if not v:
        return None
    s = sorted(v)
    return s[min(len(s) - 1, int(p * len(s)))]


def wilson(k, n, z=1.96):
    """95 % Wilson confidence interval (lo, hi) in % for k successes of n; (None, None) for n = 0."""
    if n == 0:
        return None, None
    p = k / n
    den = 1 + z * z / n
    mid = (p + z * z / (2 * n)) / den
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / den
    return 100 * max(0.0, mid - half), 100 * min(1.0, mid + half)


def fmt(x, nd=0, dash="--"):
    """A number for a LaTeX table cell ('--' for None/''), nd decimals, minus as $-$."""
    if x is None or x == "":
        return dash
    s = f"{x:.{nd}f}"
    return s.replace("-", "$-$") if s.startswith("-") else s


def write_table(out, name, header, rows, latex_rows=None):
    """Write tables/<name>.csv (header + rows) and, when latex_rows is given,
    tables/<name>.tex: the body lines of a booktabs tabular (the report's
    chapter file holds the tabular's column spec, caption and label)."""
    d = os.path.join(out, "tables")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)
    if latex_rows is not None:
        with open(os.path.join(d, name + ".tex"), "w", encoding="utf-8") as f:
            f.write("% made by make_figures.py: do not edit by hand (rerun the script)\n")
            for line in latex_rows:
                f.write(line + "\n")


# ---------------------------------------------------------------- LoRa airtime

def airtime_ms(payload_bytes, sf, bw_hz=125000, cr=1, preamble=8, crc=True, explicit=True):
    """Time on air of one LoRa packet in ms (Semtech SX1262 datasheet,
    section 6.1.4): payload_bytes includes MorseLink's 10-byte header. The
    low-data-rate optimisation is on at SF11 and SF12 (as radio_sx1262.c).
    Pure: no side effects."""
    t_sym = (2 ** sf) / bw_hz * 1000.0
    de = 1 if sf >= 11 else 0
    ih = 0 if explicit else 1
    num = 8 * payload_bytes - 4 * sf + 28 + 16 * (1 if crc else 0) - 20 * ih
    n_payload = 8 + max(math.ceil(num / (4 * (sf - 2 * de))) * (cr + 4), 0)
    return (preamble + 4.25) * t_sym + n_payload * t_sym


HEADER_BYTES = 10   # MorseLink's packet header (proto.h)
RECEIPT_BYTES = 12  # a MSG_RECEIPT: header + the 2-byte reference


# ---------------------------------------------------------------- loading

def load_sessions(root):
    """Every session under root as {session ID: {"rover": Side, "base": Side}}.
    Label corrections (corrections.txt in any folder) are loaded first, as
    analyse_tests does for its own data folder. Changes at.CORRECTIONS."""
    folders = [dp for dp, _, names in os.walk(root) if "corrections.txt" in names]
    at.read_corrections(folders)
    sessions = defaultdict(dict)
    for f in sorted(at.find_files([root])):
        s = at.Side(f)
        sessions[s.session][s.role] = s
    return sessions


def message_rows(sides):
    """All message copies of one session (both directions), as analyse_tests.messages() rows."""
    rover, base = sides.get("rover"), sides.get("base")
    rows = []
    for snd, rcv in ((rover, base), (base, rover)):
        if snd:
            rows += at.messages(snd, rcv, rover, base)
    return rows


def test_of(sides):
    """The test a session ran ("dynamic", "point", ...)."""
    s = sides.get("rover") or sides.get("base")
    return s.meta.get("test") or s.start.get("test", "")


def outcome_counts(rows):
    """(made, delivered, failed, skipped, first-try deliveries) of a list of message rows."""
    d = [r for r in rows if r["outcome"] == "delivered"]
    return (len(rows), len(d), sum(r["outcome"] == "failed" for r in rows),
            sum(r["outcome"] == "skipped" for r in rows), sum(1 for r in d if r["tries"] == 1))


# ---------------------------------------------------------------- 1. latency: LoRa against the relay

def fig_latency(sessions, out):
    """Figure: (a) LoRa round trip against message size, first-try deliveries
    of the relay night (2 m apart, SF9 / 17 dBm), with the airtime model;
    (b) the distribution (CDF) of the round trip for LoRa and the relay, on
    the relay night and on the 400 m field baseline. Also writes the
    baselines table. Returns nothing; writes figures/tables and notes numbers."""
    night = sessions.get("20261015-002416")
    field = sessions.get("20261024-051030")
    if not night:
        print("  latency: the relay night (20261015-002416) is missing; skipped")
        return
    rows_n = message_rows(night)
    rows_f = message_rows(field) if field else []

    # (a) round trip against size, LoRa first tries only (no retry in it)
    lora1 = [r for r in rows_n if r["path"] == "LoRa" and r["outcome"] == "delivered" and r["tries"] == 1]
    sizes = sorted({r["size"] for r in lora1})
    by_size = defaultdict(list)
    for r in lora1:
        by_size[r["size"]].append(r["rtt_ms"])
    # the model: message airtime + receipt airtime + a fixed overhead (fitted: median residual)
    model_air = {s: airtime_ms(HEADER_BYTES + s, 9) + airtime_ms(RECEIPT_BYTES, 9) for s in sizes}
    resid = [r["rtt_ms"] - model_air[r["size"]] for r in lora1]
    overhead = median(resid)
    note("relay night: LoRa first-try copies", len(lora1), "20261015-002416")
    note("round-trip overhead above the two airtimes (median, ms)", round(overhead), "fit, relay night")
    note("round trip 5 chars (median ms)", median(by_size.get(5, [])), "relay night")
    note("round trip 100 chars (median ms)", median(by_size.get(100, [])), "relay night")
    note("airtime 5 / 100 chars + receipt (ms)", (round(model_air.get(5, 0)), round(model_air.get(100, 0))), "model")

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(FIG_W, 2.55), gridspec_kw={"width_ratios": [1.05, 1]})
    # light dots: every first-try copy; the median per size as a line
    xs = [r["size"] + (hash((r["seq"], r["from"])) % 7 - 3) * 0.08 for r in lora1]  # tiny jitter for overlap
    ax1.scatter(xs, [r["rtt_ms"] for r in lora1], s=3, color=C[0], alpha=0.18, linewidths=0, rasterized=True,
                label="each message (first try)")
    meds = [median(by_size[s]) for s in sizes]
    ax1.plot(sizes, meds, color=C[0], lw=1.6, label="median per size")
    ax1.plot(sizes, [model_air[s] for s in sizes], color=INK2, lw=1.0, ls=(0, (4, 2)),
             label="airtime: message + receipt")
    ax1.set_xlabel("message length (characters)")
    ax1.set_ylabel("LoRa round trip (ms)")
    ax1.set_title("(a) Round trip against length, SF9")
    ax1.set_xlim(0, 103)
    ax1.set_ylim(250, 1000)
    ax1.legend(loc="upper left")

    # (b) CDFs
    def cdf(ax, vals, color, ls, label):
        """Draw one empirical CDF of round trips (ms, shown in s) on ax."""
        v = sorted(vals)
        if not v:
            return
        y = [(i + 1) / len(v) for i in range(len(v))]
        ax.plot([x / 1000 for x in v], y, color=color, ls=ls, lw=1.5, label=f"{label} (n = {len(v)})")

    def rtts(rows, path):
        """The round trips (ms) of the delivered copies of one path."""
        return [r["rtt_ms"] for r in rows if r["path"] == path and r["outcome"] == "delivered"]

    cdf(ax2, rtts(rows_n, "LoRa"), C[0], "-", "LoRa, 2 m")
    cdf(ax2, rtts(rows_n, "relay"), C[1], "-", "relay, 2 m")
    if rows_f:
        cdf(ax2, rtts(rows_f, "LoRa"), C[0], (0, (4, 2)), "LoRa, 400 m")
        cdf(ax2, rtts(rows_f, "relay"), C[1], (0, (4, 2)), "relay, 400 m")
    ax2.set_xscale("log")
    ax2.set_xlim(0.25, 12)
    ax2.set_xticks([0.3, 0.5, 1, 2, 5, 10])
    ax2.set_xticklabels(["0.3", "0.5", "1", "2", "5", "10"])
    ax2.set_xlabel("round trip (s, log scale)")
    ax2.set_ylabel("share of delivered messages")
    ax2.set_title("(b) Round-trip distribution")
    ax2.legend(loc="lower right")
    fig.tight_layout(w_pad=1.5)
    save(fig, out, "latency")

    # The baselines table: relay night (2 m) and the field baseline (400 m), per path
    hdr = ["run", "distance", "setting", "path", "made", "delivered", "rate %", "CI lo", "CI hi", "first try %",
           "rtt median ms", "rtt p95 ms", "rssi median", "snr median"]
    rows_out, tex = [], []
    for name, dist, rows, sides in (("Relay night (12 h)", "2 m, desk", rows_n, night),
                                    ("Field baseline (40 min)", "400 m, open field", rows_f, field)):
        if not rows:
            continue
        s0 = sides.get("rover") or sides.get("base")
        for path in ("LoRa", "relay"):
            grp = [r for r in rows if r["path"] == path]
            made, dl, fl, sk, first = outcome_counts(grp)
            lo, hi = wilson(dl, dl + fl)
            rtt = [r["rtt_ms"] for r in grp if r["outcome"] == "delivered"]
            rs = [r["rssi"] for r in grp if r["rssi"] != ""]
            sn = [r["snr"] for r in grp if r["snr"] != ""]
            rate = 100 * dl / (dl + fl) if dl + fl else None
            ft = 100 * first / (dl + fl) if dl + fl else None
            rows_out.append([name, dist, f"SF{s0.sf}/{s0.tx_dbm}", path, made, dl, fmt(rate, 1), fmt(lo, 1),
                             fmt(hi, 1), fmt(ft, 1), median(rtt), quantile(rtt, .95), median(rs), median(sn)])
            label = {"LoRa": name, "relay": f"{dist}, SF{s0.sf}, {s0.tx_dbm}\\,dBm"}[path]
            tex.append(f"{label} & {path} & {dl}/{made} & {fmt(rate, 1)} ({fmt(lo, 1)}--{fmt(hi, 1)}) & {fmt(ft, 1)} & "
                       f"{fmt(median(rtt))} & {fmt(quantile(rtt, .95))} & "
                       f"{(fmt(median(rs)) + ' / ' + fmt(median(sn), 1)) if path == 'LoRa' else '--'} \\\\")
        tex.append("\\midrule")
    if tex and tex[-1] == "\\midrule":
        tex.pop()
    write_table(out, "baselines", hdr, rows_out, tex)


# ---------------------------------------------------------------- 2. point tests: ladder and obstacles

# The ladder (open field on campus, both boards 1 m up): sessions whose label is
# a bare distance ("25m", "60m again"). The obstacle pairs, as agreed with the
# owner (25 Oct): the 5 m car-open run is the clear 5 m reference (line of sight,
# same 1 m height) for the floor and the brick wall too.
LADDER = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*m(\s+again)?\s*$", re.I)
OBSTACLES = [  # (obstacle label, reference label, what the report calls it)
    ("car inside 5m", "car open 5m", "Car body (rover inside a closed car)"),
    ("glass door with aluminum frame closed 10m", "glass door open 10m", "Glass door in an aluminium frame (closed vs open)"),
    ("through concrete floor 5m", "car open 5m", "Concrete floor (one storey)"),
    ("through brick wall 5m", "car open 5m", "Brick wall"),
]
INVALID_POINT = {  # sessions left out, and why (for the report's method section)
    "20261021-220851": "the base board restarted at the start (it never ran the test)",
}
SETTINGS = [(9, 17), (9, 22), (12, 22)]
PARK = ("park", 400.0)  # the park spot's label and its distance from the owner's map (GPS: 349 m, poor fixes)
NOISE_DBM = -113  # the boards' median noise floor in the field (STAT noise_dbm; set from the data below)
SNR_LIMIT = {9: -12.5, 10: -15.0, 11: -17.5, 12: -20.0}


def point_table(sessions):
    """Every valid point-test row (analyse_tests.point_rows), keyed by label:
    {label: [rows]}. Sessions with one phone file are kept only when they
    delivered something (the failed starts at 60 / 100 / 150 m were the base
    phone not starting its board). Also returns the field noise floor median."""
    by_label = defaultdict(list)
    noise = []
    for sid, sides in sorted(sessions.items()):
        if test_of(sides) != "point" or sid in INVALID_POINT:
            continue
        rows = at.point_rows(sid, sides)
        if not any((r["delivered"] or 0) for r in rows):
            continue
        if len(sides) < 2 and at.label_of(sides).lower() not in [o[0] for o in OBSTACLES]:
            continue  # a ladder start whose base never ran
        lab = at.label_of(sides).strip()
        if lab.lower() in ("t", "test", ""):
            continue  # quick checks of the set-up between spots, not measurements
        by_label[lab] += rows
        for s in sides.values():
            noise += [r["noise_dbm"] for r in s.ev("STAT") if "noise_dbm" in r]
    return by_label, (median(noise) if noise else NOISE_DBM)


def rssi_of(rows, sf, dbm):
    """Median of the per-direction median RSSIs of one spot at one setting (None if not heard)."""
    v = [r["rssi_median"] for r in rows if (r["sf"], r["dbm"]) == (sf, dbm) and r["rssi_median"] != ""]
    return statistics.mean(v) if v else None


def snr_of(rows, sf, dbm):
    """Mean of the per-direction median SNRs of one spot at one setting (None if not heard)."""
    v = [r["snr_median"] for r in rows if (r["sf"], r["dbm"]) == (sf, dbm) and r["snr_median"] != ""]
    return statistics.mean(v) if v else None


def fig_points(sessions, out):
    """Figure: the distance ladder, (a) RSSI against distance with the
    log-distance fit per setting, (b) the SNR margin above each SF's decoding
    limit. Tables: the ladder's fits (n, A, R^2, predicted range), every spot
    per setting, and the obstacles' losses. Returns nothing."""
    by_label, noise = point_table(sessions)
    note("field noise floor (median of STAT noise_dbm, point tests)", noise, "STAT lines")

    # --- the ladder
    ladder = defaultdict(list)  # distance -> rows
    for lab, rows in by_label.items():
        m = LADDER.match(lab)
        if m:
            ladder[float(m.group(1))] += rows
    dists = sorted(ladder)
    note("ladder distances (m)", dists, "labels")

    # One path-loss exponent n for all three settings, each with its own
    # intercept: RSSI = A_setting - 10 n log10(d). The physics says the
    # slope can't depend on the setting (only the transmit power moves the
    # line); per-setting slopes from five spots of 10 messages scatter widely
    # (2.3-3.5), so they are reported only as a check.
    pts = []  # (setting index, d, rssi): every direction and session
    for k, (sf, dbm) in enumerate(SETTINGS):
        for d in dists:
            for r in ladder[d]:
                if (r["sf"], r["dbm"]) == (sf, dbm) and r["rssi_median"] != "":
                    pts.append((k, d, r["rssi_median"]))
    X = np.zeros((len(pts), len(SETTINGS) + 1))
    y = np.array([p[2] for p in pts], dtype=float)
    for i, (k, d, _) in enumerate(pts):
        X[i, k] = 1.0
        X[i, -1] = -10 * math.log10(d)
    beta, res, rank, _ = np.linalg.lstsq(X, y, rcond=None)
    n_common = float(beta[-1])
    fitted = X @ beta
    dof = max(1, len(pts) - X.shape[1])
    s2 = float(np.sum((y - fitted) ** 2) / dof)
    cov = s2 * np.linalg.inv(X.T @ X)
    n_se = math.sqrt(cov[-1, -1])
    r2 = 1 - float(np.sum((y - fitted) ** 2) / np.sum((y - y.mean()) ** 2))
    rmse = math.sqrt(float(np.mean((y - fitted) ** 2)))
    note("ladder joint fit: n, s.e., R2, rmse dB, points", (round(n_common, 2), round(n_se, 2), round(r2, 3),
                                                          round(rmse, 1), len(pts)), "ladder fit")
    per_setting = {}
    for k, (sf, dbm) in enumerate(SETTINGS):
        sel = [(math.log10(d), v) for kk, d, v in pts if kk == k]
        if len({x for x, _ in sel}) >= 2:
            per_setting[(sf, dbm)] = at.path_loss_fit(sel)
            note(f"per-setting slope SF{sf}/{dbm} (check): n, A, R2", tuple(round(v, 2) for v in per_setting[(sf, dbm)]),
                 "ladder fit")

    # (a) and (b) side by side at half width; (a) tall, so the three fit lines and the
    # points separate, with (b) and the caption stacked beside it in the report
    fig, ax1 = plt.subplots(figsize=(FIG_W * 0.5, 5.4))
    fig_b, ax2 = plt.subplots(figsize=(FIG_W * 0.5, 2.75))
    xs = np.logspace(math.log10(20), math.log10(500), 50)
    for k, (sf, dbm) in enumerate(SETTINGS):
        col, mk = SETTING_STYLE[(sf, dbm)]
        sel = [(d, v) for kk, d, v in pts if kk == k]
        ax1.scatter([p[0] for p in sel], [p[1] for p in sel], s=30, marker=mk, color=col, edgecolors="white",
                    linewidths=0.6, zorder=3)
        # SF12's fit lies almost on SF9/22 dBm's (same power), so it is dashed to let both show
        ax1.plot(xs, beta[k] - 10 * n_common * np.log10(xs), color=col, lw=1.3, ls=(0, (5, 3)) if sf == 12 else "-",
                 label=f"SF{sf}, {dbm} dBm")
        marg = []
        for d in dists:
            v = [r["snr_median"] - SNR_LIMIT[sf] for r in ladder[d]
                 if (r["sf"], r["dbm"]) == (sf, dbm) and r["snr_median"] != ""]
            if v:
                marg.append((d, statistics.mean(v)))
        ax2.plot([m[0] for m in marg], [m[1] for m in marg], color=col, marker=mk, ms=4.5, mec="white", mew=0.6,
                 label=f"SF{sf}, {dbm} dBm")
        park = by_label.get(PARK[0], [])
        pr = [r["rssi_median"] for r in park if (r["sf"], r["dbm"]) == (sf, dbm) and r["rssi_median"] != ""]
        pm = [r["snr_median"] - SNR_LIMIT[sf] for r in park if (r["sf"], r["dbm"]) == (sf, dbm) and r["snr_median"] != ""]
        if pr:
            ax1.scatter([PARK[1] * 1.04] * len(pr), pr, s=34, marker=mk, facecolors="none", edgecolors=col,
                        linewidths=1.0, zorder=3)
        if pm:
            ax2.scatter([PARK[1] * 1.04], [statistics.mean(pm)], s=26, marker=mk, facecolors="none", edgecolors=col,
                        linewidths=1.1, zorder=3)
            field = [m_[1] for m_ in marg if m_[0] == PARK[1]]
            note(f"park vs field at 400 m, SF{sf}/{dbm}: park margin, field margin (dB)",
                 (round(statistics.mean(pm), 1), round(field[0], 1) if field else None), "point tests")
    for ax in (ax1, ax2):
        ax.set_xscale("log")
        ax.xaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
        ax.set_xticks([20, 50, 100, 200, 500])
        ax.set_xticklabels(["20", "50", "100", "200", "500"])
        ax.set_xlim(18, 550)
        ax.set_xlabel("distance (m, log scale)")
    ax1.set_ylabel("RSSI at the receiver (dBm)")
    ax1.set_title("(a) Signal against distance")
    ax1.text(0.97, 0.95, f"common fit: n = {n_common:.1f} $\\pm$ {n_se:.1f}", transform=ax1.transAxes,
             ha="right", va="top", fontsize=7.5, color=INK2)
    ax2.axhline(0, color=INK2, lw=0.8)
    ax2.text(530, 0.8, "decoding limit", color=INK2, fontsize=7.5, va="bottom", ha="right")
    ax2.set_ylim(-2, 30)
    ax2.set_ylabel("SNR above the SF's limit (dB)")
    ax2.set_title("(b) Margin left")
    # the park's three hollow markers side by side in one legend entry
    park_h = tuple(Line2D([], [], ls="none", marker=SETTING_STYLE[k][1], mfc="none", mec=SETTING_STYLE[k][0],
                          mew=1.0, ms=5) for k in SETTINGS)
    for ax, loc, anchor in ((ax1, "lower left", (0.0, 0.0)), (ax2, "lower left", (0.0, 0.04))):
        hs, ls_ = ax.get_legend_handles_labels()
        if by_label.get(PARK[0]):
            hs.append(park_h)
            ls_.append("park, 400 m")
        ax.legend(hs, ls_, loc=loc, bbox_to_anchor=anchor, fontsize=7.5,
                  handler_map={tuple: HandlerTuple(ndivide=None, pad=0.6)})
    fig.tight_layout()
    save(fig, out, "ladder_a")
    fig_b.tight_layout()
    save(fig_b, out, "ladder_b")

    # the fit's table, with the range it predicts: where the RSSI meets the
    # noise floor + the SF's SNR limit (the effective sensitivity)
    hdr = ["setting", "n (common)", "A (1 m) dBm", "per-setting n (check)", "sensitivity dBm", "predicted range m"]
    rows_out, tex = [], []
    for k, (sf, dbm) in enumerate(SETTINGS):
        a = float(beta[k])
        sens = noise + SNR_LIMIT[sf]
        rng = 10 ** ((a - sens) / (10 * n_common))
        own = per_setting.get((sf, dbm), (None,))[0]
        rows_out.append([f"SF{sf}/{dbm}", round(n_common, 2), round(a, 1), round(own, 2) if own else "",
                         round(sens, 1), round(rng)])
        tex.append(f"SF{sf}, {dbm} dBm & {fmt(a, 1)} & {fmt(own, 1)} & {fmt(sens, 1)} & "
                   f"{rng / 1000:.1f}\\,km \\\\")
        note(f"ladder SF{sf}/{dbm}: A (dBm at 1 m), sensitivity, predicted range m", (round(a, 1), round(sens, 1),
                                                                                    round(rng)), "joint fit")
    write_table(out, "pathloss", hdr, rows_out, tex)

    # --- every spot per setting (delivery, RSSI, SNR margin): the appendix table
    hdr = ["spot", "setting", "delivered", "sent", "rssi (mean of directions)", "snr margin"]
    rows_out, tex = [], []
    order = sorted(by_label, key=lambda l: (0 if LADDER.match(l) else 1,
                                           float(LADDER.match(l).group(1)) if LADDER.match(l) else 0, l))
    for lab in order:
        rows = by_label[lab]
        first = True
        for sf, dbm in SETTINGS:
            sel = [r for r in rows if (r["sf"], r["dbm"]) == (sf, dbm)]
            dl = sum(r["delivered"] or 0 for r in sel)
            sent = sum(r["sent"] or 0 for r in sel if r["sent"] != "")
            known = all(r["sent"] != "" for r in sel)
            rs, sn = rssi_of(rows, sf, dbm), snr_of(rows, sf, dbm)
            marg = sn - SNR_LIMIT[sf] if sn is not None else None
            rows_out.append([lab, f"SF{sf}/{dbm}", dl, sent if known else "", rs, marg])
            shown = "park, about 400\\,m (trees, slopes)" if lab == PARK[0] else lab.replace("aluminum", "aluminium")
            tex.append(f"{shown if first else ''} & SF{sf}, {dbm} dBm & "
                       f"{dl}/{sent if known else '?'} & {fmt(rs, 1)} & {fmt(marg, 1)} \\\\")
            first = False
        tex.append("\\addlinespace[2pt]")
    if tex:
        tex.pop()
    write_table(out, "points_all", hdr, rows_out, tex)

    # --- obstacles: loss = reference RSSI - obstacle RSSI, same setting and direction
    hdr = ["obstacle", "distance m", "setting", "ref rssi", "obstacle rssi", "loss dB", "delivered"]
    rows_out, tex = [], []
    for obs, ref, name in OBSTACLES:
        o_rows = by_label.get(obs, [])
        r_rows = by_label.get(ref, [])
        if not o_rows or not r_rows:
            continue
        d = re.search(r"(\d+)\s*m", obs)
        losses = []
        cells = []
        for sf, dbm in SETTINGS:
            per_dir = []
            for frm in ("rover", "base"):
                ro = [r for r in o_rows if (r["sf"], r["dbm"], r["from_role"]) == (sf, dbm, frm) and r["rssi_median"] != ""]
                rr = [r for r in r_rows if (r["sf"], r["dbm"]) == (sf, dbm) and r["rssi_median"] != ""]
                if ro and rr:
                    # the reference's two directions are averaged: the boards swapped roles in some runs
                    per_dir.append(statistics.mean(x["rssi_median"] for x in rr) - ro[0]["rssi_median"])
            loss = statistics.mean(per_dir) if per_dir else None
            losses.append(loss)
            dl = sum(r["delivered"] or 0 for r in o_rows if (r["sf"], r["dbm"]) == (sf, dbm))
            cells.append(fmt(loss, 1))
            rows_out.append([name, d.group(1) if d else "", f"SF{sf}/{dbm}", rssi_of(r_rows, sf, dbm),
                             rssi_of(o_rows, sf, dbm), loss, dl])
        valid = [x for x in losses if x is not None]
        mean_loss = statistics.mean(valid) if valid else None
        note(f"obstacle loss {name} (dB, mean of settings)", round(mean_loss, 1) if mean_loss is not None else None,
             "point tests")
        tex.append(f"{name} & {d.group(1) if d else ''}\\,m & " + " & ".join(cells) + f" & {fmt(mean_loss, 1)} \\\\")
    write_table(out, "attenuation", hdr, rows_out, tex)

    # --- power and SF at the same spot: what 17 -> 22 dBm and SF9 -> SF12 changed
    d_pow, d_sf, d_marg_sf = [], [], []
    for lab, rows in by_label.items():
        for frm in ("rover", "base"):
            def get(sf, dbm, key):
                """This spot's value of `key` at one setting, in this direction (None if not heard)."""
                v = [r[key] for r in rows if (r["sf"], r["dbm"], r["from_role"]) == (sf, dbm, frm) and r[key] != ""]
                return v[0] if v else None
            a, b, c = get(9, 17, "rssi_median"), get(9, 22, "rssi_median"), get(12, 22, "rssi_median")
            if a is not None and b is not None:
                d_pow.append(b - a)
            if b is not None and c is not None:
                d_sf.append(c - b)
            ma, mc = get(9, 22, "snr_median"), get(12, 22, "snr_median")
            if ma is not None and mc is not None:
                d_marg_sf.append((mc - SNR_LIMIT[12]) - (ma - SNR_LIMIT[9]))
    r1 = lambda v: None if v is None else round(v, 1)  # noqa: E731  (one decimal for numbers.txt)
    note("RSSI change 17 -> 22 dBm (median, dB; all spots and directions)", (r1(median(d_pow)), len(d_pow)), "points")
    note("RSSI change SF9 -> SF12 at 22 dBm (median, dB)", (r1(median(d_sf)), len(d_sf)), "points")
    note("SNR margin gain SF9 -> SF12 at 22 dBm (median, dB)", (r1(median(d_marg_sf)), len(d_marg_sf)), "points")


# ---------------------------------------------------------------- 3. dynamic sessions

DYNAMIC = [  # (session, short name for the figure, environment as described by the owner)
    ("20261015-185651", "15 Oct, walk to a mall\nSF9, 17 dBm", "town: walk into a mall"),
    ("20261018-184526", "18 Oct, walk\nSF9, 22 dBm", "town: walk, including a mall"),
    ("20261018-194848", "18 Oct, drive\nSF9, 22 dBm", "town: drive (rover in a car)"),
    ("20261023-153101", "23 Oct, walk\nSF12, 22 dBm", "town: long-range walk, about 3.5 km, including a mall"),
    ("20261026-182908", "26 Oct, park walk\nSF9, 22 dBm", "park: walk among trees, slopes and bridges"),
]


def fig_dynamic(sessions, out):
    """Figure: every LoRa copy of the dynamic sessions at its GPS distance,
    delivered / failed / skipped because the link was lost, one row per
    session (log distance axis). Table: per session delivery, availability
    and signal. Returns nothing."""
    fig, ax = plt.subplots(figsize=(FIG_W, 2.25))
    yt, ylab = [], []
    hdr = ["session", "environment", "setting", "minutes", "lora made", "lora delivered", "lora failed",
           "lora skipped (lost)", "lora rate %", "lora availability % (rover)", "relay delivered", "relay made",
           "relay rate %", "median distance m", "farthest LoRa delivery m", "rssi median"]
    rows_out, tex = [], []
    for k, (sid, short, env) in enumerate(DYNAMIC):
        sides = sessions.get(sid)
        if not sides:
            continue
        rows = message_rows(sides)
        lora = [r for r in rows if r["path"] == "LoRa" and r["distance_m"] != ""]
        y = len(yt)  # (a session without GPS distances gets no row in the figure; the table keeps it)
        if lora:
            yt.append(y)
            ylab.append(short)
        for oc, col, mk, fc, lab in (("skipped", MUTED, "o", "none", "LoRa skipped: link lost"),
                                     ("failed", C[7], "x", C[7], "LoRa failed (no receipt)"),
                                     ("delivered", C[0], "o", C[0], "LoRa delivered")):
            sel = [r for r in lora if r["outcome"] == oc]
            jit = [y + ((hash((r["seq"], r["from"])) % 11) - 5) * 0.035 for r in sel]
            ax.scatter([max(r["distance_m"], 4) for r in sel], jit, s=10, marker=mk,
                       facecolors=fc if mk != "x" else None, edgecolors=col if mk == "o" else None,
                       color=col if mk == "x" else None, linewidths=0.7, label=lab if k == 0 else None, zorder=3)
        # table numbers
        L = [r for r in rows if r["path"] == "LoRa"]
        R = [r for r in rows if r["path"] == "relay"]
        made, dl, fl, sk, first = outcome_counts(L)
        rm, rd, rf, rs_, _ = outcome_counts(R)
        rover = sides.get("rover")
        periods = at.lost_periods(rover) if rover else []
        start_t = rover.start.get("t", rover.board[0]["t"]) if rover else 0
        stop = next((r for r in rover.board if r.get("ev") == "STOP"), None) if rover else None
        end_t = stop["t"] if stop else (rover.board[-1]["t"] if rover else 0)
        span = max(1, end_t - start_t)
        avail = 100.0 * (span - sum(p["end"] - p["start"] for p in periods)) / span
        dists = [r["distance_m"] for r in rows if r["distance_m"] != ""]
        far = max([r["distance_m"] for r in L if r["outcome"] == "delivered" and r["distance_m"] != ""], default=None)
        rssi = [r["rssi"] for r in L if r["rssi"] != ""]
        s0 = rover or sides.get("base")
        rate = 100 * dl / (dl + fl) if dl + fl else None
        rrate = 100 * rd / (rd + rf) if rd + rf else None
        rows_out.append([sid, env, f"SF{s0.sf}/{s0.tx_dbm}", round(span / 60000), made, dl, fl, sk, fmt(rate, 1),
                         round(avail, 1), rd, rm, fmt(rrate, 1), median(dists), far, median(rssi)])
        tex.append(f"{env[0].upper() + env[1:]}, SF{s0.sf}/{s0.tx_dbm}\\,dBm & {round(span / 60000)} & "
                   f"{dl}/{dl + fl} ({fmt(rate, 0)}\\,\\%) & {sk} & {fmt(avail, 0)}\\,\\% & "
                   f"{rd}/{rd + rf} ({fmt(rrate, 0)}\\,\\%) & {fmt(far)} \\\\")
        note(f"dynamic {sid}: LoRa delivered/(delivered+failed), skipped, availability %, farthest delivered m",
             (dl, dl + fl, sk, round(avail, 1), far), "messages + LINK lines")
    ax.set_xscale("log")
    ax.set_xlim(3.5, 6000)
    ax.set_xticks([10, 30, 100, 300, 1000, 3000])
    ax.set_xticklabels(["10", "30", "100", "300", "1 000", "3 000"])
    ax.set_yticks(yt)
    ax.set_yticklabels(ylab, fontsize=7.5)
    ax.set_ylim(len(yt) - 0.4, -0.6)
    ax.grid(axis="y", visible=False)
    ax.set_xlabel("distance between the boards (m, GPS, log scale)")
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, 1.13), ncol=3, fontsize=7.5)
    fig.tight_layout()
    save(fig, out, "dynamic")
    write_table(out, "dynamic", hdr, rows_out, tex)


# ---------------------------------------------------------------- 4. the meter readings (current per state)

# The battery voltage during the meter session (board DEA, the one with the
# motors) was not noted, but the session ran just before the field baseline of
# 24 Oct, whose first reading from DEA's own battery monitor was 4085 mV.
METER_V = 4.08
METER_V_ERR = 0.05
SHUNT_TOL = 0.05     # 1 ohm, 5 % resistor
METER_RES_MA = 0.1   # the voltmeter's 200 mV range: 0.1 mV = 0.1 mA

METER_NAMES = [  # (text in Meter_test.txt, what the report calls the state)
    ("Idle", "Processor on, radio in standby, Bluetooth off"),
    ("Idle + LoRa", "+ LoRa receiving (SF9)"),
    ("Idle + LoRa + Send", "+ sending messages (22 dBm)"),
    ("Idle + LoRa + BLE", "+ Bluetooth connected to the phone"),
    ("Idle + LoRa + BLE + Key", "+ key held down"),
    ("Idle + LoRa + BLE + Motor@25%", "+ one motor running (rail 3.0 V)"),
    ("Idle + LoRa + BLE + 2Motors@25%", "+ both motors running (rail 3.0 V)"),
]


def read_meter(root):
    """The current readings from Meter_test.txt anywhere under root, as
    [(label, mA)] in file order; [] if there is no file."""
    for dp, _, names in os.walk(root):
        for n in names:
            if n.lower().startswith("meter") and n.lower().endswith(".txt"):
                out = []
                for line in open(os.path.join(dp, n), encoding="utf-8", errors="replace"):
                    m = re.match(r"\s*(.+?)\s*==>\s*([\d.]+)\s*A", line)
                    if m:
                        out.append((m.group(1), 1000.0 * float(m.group(2))))
                return out
    return []


def table_meter(root, out):
    """Table: current and power per state, and what each part adds (the
    difference from the state it builds on). Returns {label: mA}."""
    readings = read_meter(root)
    if not readings:
        print("  meter: no Meter_test.txt; skipped")
        return {}
    ma = dict(readings)
    names = dict(METER_NAMES)
    base_of = {  # which reading each state adds to
        "Idle + LoRa": "Idle", "Idle + LoRa + Send": "Idle + LoRa", "Idle + LoRa + BLE": "Idle + LoRa",
        "Idle + LoRa + BLE + Key": "Idle + LoRa + BLE", "Idle + LoRa + BLE + Motor@25%": "Idle + LoRa + BLE",
        "Idle + LoRa + BLE + 2Motors@25%": "Idle + LoRa + BLE",
    }
    hdr = ["state", "current mA", "+/- mA", "power mW", "+/- mW", "adds mA", "adds mW"]
    rows_out, tex = [], []
    for lab, i in readings:
        err_i = i * SHUNT_TOL + METER_RES_MA
        p = METER_V * i
        err_p = p * math.sqrt((err_i / i) ** 2 + (METER_V_ERR / METER_V) ** 2)
        add = i - ma[base_of[lab]] if lab in base_of else None
        rows_out.append([names.get(lab, lab), round(i, 1), round(err_i, 1), round(p), round(err_p),
                         round(add, 1) if add is not None else "", round(METER_V * add) if add is not None else ""])
        tex.append(f"{names.get(lab, lab)} & {i:.1f} & {p:.0f} $\\pm$ {err_p:.0f} & "
                   f"{('+' + format(add, '.1f')) if add is not None else '--'} \\\\")
    write_table(out, "meter", hdr, rows_out, tex)
    note("meter: idle / +LoRa / +BLE / +one motor / +two motors (mA)",
         [round(ma.get(k, 0), 1) for k in ("Idle", "Idle + LoRa", "Idle + LoRa + BLE",
                                            "Idle + LoRa + BLE + Motor@25%", "Idle + LoRa + BLE + 2Motors@25%")],
         "Meter_test.txt")
    return ma


# ---------------------------------------------------------------- 5. battery run-downs

RUN_NAMES = {  # test -> (short name, what ran)
    "rundown-a": ("A", "listening, keepalives only, Bluetooth off"),
    "rundown-b": ("B", "Bluetooth + test traffic on LoRa and the relay"),
    "rundown-l": ("L", "test traffic on LoRa only, Bluetooth off"),
    "rundown-m": ("M", "the motors play PARIS over and over, Bluetooth off"),
}
RUN_COLOUR = {"A": C[0], "B": C[1], "L": C[2], "M": C[3]}
BOARD_LS = {"DEA": "-", "ABC": (0, (4, 1.6))}
CUTOFF_MV = 3226  # measured: board ABC switched itself off at 3226 mV (26 Oct), with its battery empty
RATED_AH = 3.0     # the cell's rating: the most it can hold


def capture_time(path):
    """When the monitor log was saved, from its name (log.morselink.YYYYMMDDHHMMSS.txt); None if not in the name."""
    m = re.search(r"(\d{14})", os.path.basename(path))
    return datetime.datetime.strptime(m.group(1), "%Y%m%d%H%M%S") if m else None


def smooth_mv(rows):
    """[(minute, smoothed mV)] of a dump's rows without charging: a 15-minute running median."""
    rows = [r for r in rows if r[2] == 0]
    sm = at.running_median([r[1] for r in rows])
    return [(r[0], v) for r, v in zip(rows, sm)]


def time_between(curve, hi_mv, lo_mv):
    """Hours a smoothed curve took to fall from hi_mv to lo_mv (None if it didn't cover both)."""
    t_hi = next((m for m, v in curve if v <= hi_mv), None)
    t_lo = next((m for m, v in curve if v <= lo_mv), None)
    if t_hi is None or t_lo is None or t_lo <= t_hi:
        return None
    return (t_lo - t_hi) / 60.0


# ---------------------------------------------------------------- 5a. the current each mode draws (model)

# Measured: the meter's currents (at METER_V). From the datasheet: the SX1262
# at 22 dBm draws about 118 mA while transmitting (its receive current is in
# the meter's "+LoRa" reading). From the logs: the motors' on-time (the motor
# night's dump records it each minute) and the airtime (STAT duty_pm).
TX_EXTRA_MA = 118 - 9      # transmitting instead of receiving
MOTOR_FULL_MA = 64         # one motor at full drive from 3.3 V (58 mA measured from 3.0 V, scaled by 3.3/3.0)
MORSE_DUTY = 0.44          # share of playing time a motor is on (PARIS: 22 of 50 units)
LISTEN_MA = 35.2           # meter: processor + LoRa receiving (fallbacks if the meter file is missing)
PHONE_MA = 42.0            # meter: + Bluetooth connected to the phone
KEEPALIVE_S = {9: 30, 10: 30, 11: 45, 12: 60}


def keepalive_ma(sf):
    """Average extra current of sending a keepalive (and receipting the partner's) at this SF, mA."""
    return TX_EXTRA_MA * airtime_ms(RECEIPT_BYTES, sf) / (KEEPALIVE_S.get(sf, 30) * 1000.0)


def traffic_ma(sf, every_s, chars=52.5):
    """Extra current of test traffic: a message of `chars` characters every `every_s` seconds from each board,
    and a receipt for each of the partner's, mA."""
    per_h = 3600.0 / every_s
    return TX_EXTRA_MA * per_h * (airtime_ms(HEADER_BYTES + chars, sf) + airtime_ms(RECEIPT_BYTES, sf)) / 3.6e6


def dump_ma(d, meter):
    """The current (mA at METER_V) a run-down drew, from its mode: A listening, B Bluetooth and traffic on both
    paths (a message every 10-20 s), L LoRa traffic only at SF12 (every 90-130 s), M the motors (their logged
    on-time). Returns (mA, how)."""
    m = d["meta"]
    sf = int(m.get("sf") or 9)
    listen = meter.get("Idle + LoRa", LISTEN_MA)
    phone = meter.get("Idle + LoRa + BLE", PHONE_MA)
    test = m.get("test")
    if test == "rundown-a":
        return listen + keepalive_ma(sf), "listening"
    if test == "rundown-b":
        return phone + keepalive_ma(sf) + traffic_ma(sf, 15), "phone + traffic"
    if test == "rundown-l":
        return listen + keepalive_ma(sf) + traffic_ma(sf, 110), "LoRa traffic"
    if test == "rundown-m":
        rows = [r for r in d["rows"] if len(r) >= 5]
        on = statistics.mean((r[3] + r[4]) / 60000.0 for r in rows[1:]) if len(rows) > 1 else 2 * MORSE_DUTY
        return listen + keepalive_ma(sf) + MOTOR_FULL_MA * on, f"motors on {on:.2f} of the time"
    return listen + keepalive_ma(sf), "?"


def charge_curve(series, ma_at):
    """Integrate a battery's charge along a series [(hours, mV)]: the board's regulators make it a roughly
    constant-power load, so the current at voltage V is the reference current x METER_V / V. `ma_at(h)` gives
    the reference current at hour h. Returns [(Ah used, V, Wh used)]."""
    q, e, out = 0.0, 0.0, []
    for i, (h, mv) in enumerate(series):
        v = mv / 1000.0
        if i:
            h0, mv0 = series[i - 1]
            dt = h - h0
            vm = (v + mv0 / 1000.0) / 2
            i_a = ma_at((h + h0) / 2) * METER_V / vm / 1000.0
            q += i_a * dt
            e += i_a * vm * dt
        out.append((q, v, e))
    return out


def fig_rundowns(root, sessions, out, meter):
    """Figure (two panels): (a) the run-downs from a full charge, voltage
    against time, one line per board and mode; (b) voltage against the charge
    used, for those runs and for both boards' one-charge histories of 21-22
    Oct, extended to the measured cut-off. Tables: the run-downs (slope, time
    from 4.10 to 4.00 V, draw relative to listening) and nothing else. Returns
    the usable energy bracket (Wh low, Wh high) and capacity bracket (Ah)."""
    dumps = at.dedupe_dumps([d for f in at.find_dumps([root]) for d in at.read_dumps(f)])
    full = [d for d in dumps if next((r[1] for r in d["rows"] if r[2] == 0), 0) >= 4100]

    fig, (ax, axq) = plt.subplots(1, 2, figsize=(FIG_W, 2.9), gridspec_kw={"width_ratios": [1, 1.05]})
    hdr = ["board", "run", "what ran", "SF/dBm", "hours", "start mV", "end mV", "slope mV/h (after 30 min)",
           "4.10->4.00 V hours", "draw vs A at same SF", "model mA"]
    rows_out, tex = [], []
    t_ref = {}  # SF -> hours 4.10 -> 4.00 V in run-down A
    info = []
    for d in sorted(full, key=lambda d: ("ABLM".index(RUN_NAMES.get(d["meta"].get("test"), ("?",))[0])
                                         if d["meta"].get("test") in RUN_NAMES else 9, d["meta"].get("board"))):
        m = d["meta"]
        short, what = RUN_NAMES.get(m.get("test"), ("?", m.get("test")))
        curve = smooth_mv(d["rows"])
        slope = at.linear_slope([(mi / 60.0, v) for mi, v in curve if mi >= 30])
        t41 = time_between(curve, 4100, 4000)
        sf = int(m.get("sf") or 9)
        if short == "A" and t41:
            t_ref[sf] = t41
        ma, how = dump_ma(d, meter)
        info.append((d, short, what, curve, slope, t41, sf, ma))
        col, ls = RUN_COLOUR.get(short, MUTED), BOARD_LS.get(m.get("board"), "-")
        ax.plot([mi / 60.0 for mi, _ in curve], [v / 1000 for _, v in curve], color=col, ls=ls, lw=1.4)
        qc = charge_curve([(mi / 60.0, v) for mi, v in curve], lambda h, ma=ma: ma)
        axq.plot([q for q, _, _ in qc], [v for _, v, _ in qc], color=col, ls=ls, lw=1.1, zorder=5)
        note(f"run-down {short} {m.get('board')}: model mA at {METER_V} V, Ah used, end V",
             (round(ma, 1), round(qc[-1][0], 2), round(qc[-1][1], 3)), how)
    for d, short, what, curve, slope, t41, sf, ma in info:
        m = d["meta"]
        rel = t_ref.get(sf) / t41 if (t41 and t_ref.get(sf)) else None
        hours = curve[-1][0] / 60.0
        rows_out.append([m.get("board"), short, what, f"SF{sf}/{m.get('tx_dbm')}", round(hours, 1), curve[0][1],
                         curve[-1][1], round(slope, 1) if slope else "", round(t41, 1) if t41 else "",
                         round(rel, 2) if rel else "", round(ma, 1)])
        tex.append(f"{short} & {m.get('board')} & SF{sf}, {m.get('tx_dbm')}\\,dBm & {hours:.1f} & "
                   f"{curve[0][1] / 1000:.2f}$\\to${curve[-1][1] / 1000:.2f} & {fmt(slope, 1)} & {fmt(t41, 1)} & "
                   f"{fmt(rel, 2)} \\\\")
        note(f"run-down {short} {m.get('board')} SF{sf}: hours, slope mV/h, 4.10->4.00 V h, draw vs A",
             (round(hours, 1), round(slope, 1) if slope else None, round(t41, 1) if t41 else None,
              round(rel, 2) if rel else None), os.path.basename(d["file"]))
    # what the meter's currents predict for the same ratios (B, L, M against A at the same SF)
    ma_of = {(i[1], i[6], i[0]["meta"].get("board")): i[7] for i in info}
    for (short, sf, board), ma in sorted(ma_of.items()):
        ref = ma_of.get(("A", sf, board)) or next((v for (s2, f2, _), v in ma_of.items() if s2 == "A" and f2 == sf), None)
        if ref and short != "A":
            note(f"model ratio {short}/A at SF{sf} ({board})", round(ma / ref, 2), "meter + model")
    write_table(out, "rundowns", hdr, rows_out, tex)
    ax.set_xlabel("time since unplugging the full battery (h)")
    ax.set_ylabel("battery voltage (V)")
    ax.set_xlim(0, 17)
    ax.set_ylim(3.75, 4.2)
    ax.set_title("(a) Run-downs from full")
    short_names = {"A": "A: listening", "B": "B: phone + traffic", "L": "L: LoRa traffic", "M": "M: motors playing"}
    handles = [Line2D([], [], color=RUN_COLOUR[k], lw=1.6, label=short_names[k]) for k in "ABLM"]
    handles += [Line2D([], [], color=INK2, ls=BOARD_LS[b], lw=1.2, label=f"board {b}") for b in ("DEA", "ABC")]
    ax.legend(handles=handles, loc="lower left", fontsize=6.5, ncol=1, handlelength=1.8)

    # --- one charge, 21-22 Oct: each board's dumps + the STAT readings of the point tests, as one history
    stat = defaultdict(list)  # board -> [(datetime, mV, duty per mille, ble)]
    for sid, sides in sessions.items():
        if not sid.startswith(("20261021", "20261022")):
            continue
        for sd in sides.values():
            for r in sd.ev("STAT"):
                if r.get("bat_mv"):
                    stat[sd.own].append((datetime.datetime.fromtimestamp(r["t"] / 1000), r["bat_mv"],
                                         r.get("duty_pm") or 0, r.get("ble", 1)))
    segs = defaultdict(list)  # board -> [(start, [(datetime, mV)], dump)]
    for d in dumps:
        m = d["meta"]
        cap = capture_time(d["file"])
        rows = [r for r in d["rows"] if r[2] == 0]
        if not rows or not cap:
            continue
        if m.get("session", "").startswith("2026"):  # started from the phone: the session ID is its start
            t0 = datetime.datetime.strptime(m["session"], "%Y%m%d-%H%M%S")
        else:  # started on the console: it ended when the monitor was opened
            t0 = cap - datetime.timedelta(minutes=rows[-1][0])
        if t0.date() not in (datetime.date(2026, 10, 21), datetime.date(2026, 10, 22)):
            continue
        sm = at.running_median([r[1] for r in rows])
        segs[m["board"]].append((t0, [(t0 + datetime.timedelta(minutes=r[0]), v) for r, v in zip(rows, sm)], d))
    listen = meter.get("Idle + LoRa", LISTEN_MA)
    phone = meter.get("Idle + LoRa + BLE", PHONE_MA)
    result = {}
    for board in ("DEA", "ABC"):
        parts = sorted(segs.get(board, []), key=lambda x: x[0])
        if not parts:
            continue
        origin = parts[0][0]
        # every reading, with the reference current that applied up to it
        pts = []  # (hours, mV, kind, mA low, mA high)
        for t0, series, d in parts:
            ma, _ = dump_ma(d, meter)
            pts += [((t - origin).total_seconds() / 3600, v, "dump", ma, ma) for t, v in series]
        for t, v, duty, ble in sorted(stat.get(board, [])):
            if t > origin:
                ma = (phone if ble else listen) + TX_EXTRA_MA * duty / 1000.0
                pts.append(((t - origin).total_seconds() / 3600, v, "stat", ma, ma))
        pts.sort()
        # between readings more than 30 min apart nothing was logged: the board was linked and listening, with
        # or without the phone (both are taken, as the low and the high estimate)
        series_lo, series_hi = [], []
        for i, (h, mv, kind, lo, hi) in enumerate(pts):
            series_lo.append((h, mv, lo))
            series_hi.append((h, mv, hi))
        def make(series, gap_ma):
            """Charge curve of one estimate: the gaps at gap_ma."""
            hs = [p[0] for p in series]
            def ma_at(h, hs=hs, series=series):
                """The reference current at hour h: the reading's own, or the gap's."""
                k = bisect.bisect_left(hs, h)
                k = min(max(k, 1), len(hs) - 1)
                if hs[k] - hs[k - 1] > 0.5:
                    return gap_ma
                return series[k][2]
            return charge_curve([(p[0], p[1]) for p in series], ma_at)
        q_lo = make(series_lo, listen + keepalive_ma(9))
        q_hi = make(series_hi, phone + keepalive_ma(9))
        # plot the middle estimate; the end point carries the bracket
        q_mid = [((a[0] + b[0]) / 2, a[1], (a[2] + b[2]) / 2) for a, b in zip(q_lo, q_hi)]
        # one continuous line per board (straight segments bridge the hours without readings)
        axq.plot([q for q, _, _ in q_mid], [v for _, v, _ in q_mid], color=INK, lw=1.3,
                 ls="-" if board == "DEA" else (0, (4, 1.6)), zorder=4)
        result[board] = (q_lo, q_hi, pts)
        note(f"one charge {board}: hours, last mV, Ah used (low, high), Wh used (low, high)",
             (round(pts[-1][0], 1), pts[-1][1], round(q_lo[-1][0], 2), round(q_hi[-1][0], 2),
              round(q_lo[-1][2], 2), round(q_hi[-1][2], 2)), "dumps + STAT + meter model")
    # --- the rest of the curve, to the measured cut-off: at least nothing (the knee could come at once), at
    # most the last slope continued in a straight line (a Li-ion curve only steepens towards the end)
    cap = None
    if "DEA" in result:
        q_lo, q_hi, pts = result["DEA"]
        last = [(q, v) for q, v, _ in q_hi if q >= q_hi[-1][0] - 0.15]
        slope = at.linear_slope(last)  # V per Ah (negative)
        v_end = q_hi[-1][1]
        tail = (v_end - CUTOFF_MV / 1000) / -slope if slope and slope < 0 else 0
        tail = min(tail, max(0.0, RATED_AH - q_hi[-1][0]))  # never more than the cell's rating
        e_tail = tail * (v_end + CUTOFF_MV / 1000) / 2
        cap = {"ah": (q_lo[-1][0], q_hi[-1][0] + tail), "wh": (q_lo[-1][2], q_hi[-1][2] + e_tail),
               "slope": slope, "tail": tail}
        # the projection drawn from the middle estimate, straight to the cut-off
        q_mid = [((x[0] + y[0]) / 2, x[1]) for x, y in zip(q_lo, q_hi)]
        q0, v0 = q_mid[-1]
        q_end = q0 + (v0 - CUTOFF_MV / 1000) / -slope if slope and slope < 0 else q0
        axq.plot([q0, q_end], [v0, CUTOFF_MV / 1000], color=INK2, lw=1.1, ls="--")
        axq.scatter([q_end], [CUTOFF_MV / 1000], s=22, marker="x", color=INK, zorder=6, linewidths=1.2)
        axq.annotate(f"projected end, about {q_end:.1f} Ah", (q_end, CUTOFF_MV / 1000), xytext=(-6, -5),
                     textcoords="offset points", fontsize=7, color=INK, ha="right", va="top")
        note("straight-line projection of DEA's history (middle estimate) to the cut-off: Ah", round(q_end, 2),
             "DEA history + measured cut-off")
        note("usable capacity to the cut-off: Ah low, Ah high; Wh low, Wh high; last slope V/Ah; tail Ah",
             (round(cap["ah"][0], 2), round(cap["ah"][1], 2), round(cap["wh"][0], 2), round(cap["wh"][1], 2),
              round(slope, 3), round(tail, 2)), "DEA history + measured cut-off")
    axq.axhline(CUTOFF_MV / 1000, color=INK2, lw=0.7)
    axq.text(0.03, CUTOFF_MV / 1000 + 0.012, f"cut-off measured on ABC, {CUTOFF_MV / 1000:.2f} V", fontsize=6.8,
             color=INK2, va="bottom")
    axq.set_xlabel("charge used (Ah, from the measured currents)")
    axq.set_ylim(3.1, 4.22)
    axq.set_xlim(0, 3.2)
    axq.set_title("(b) Voltage against charge used")
    axq.legend(handles=[Line2D([], [], color=INK, lw=1.3, label="DEA, one charge (21–22 Oct)"),
                        Line2D([], [], color=INK, lw=1.3, ls=(0, (4, 1.6)), label="ABC, one charge"),
                        Line2D([], [], color=INK2, lw=1.1, ls="--", label="straight-line projection"),
                        Line2D([], [], color=MUTED, lw=1.1, label="run-downs (colours as in a)")],
               loc="upper right", fontsize=6.3, handlelength=2.0)
    fig.tight_layout(w_pad=1.2)
    save(fig, out, "battery")
    return cap


# ---------------------------------------------------------------- 5b. battery life by use

def table_battery_life(meter, cap, out):
    """Table: estimated battery life for four patterns of use: the usable
    energy to the cut-off (low and high estimate) over the power each use
    draws (current at METER_V x METER_V). Returns [(use, mA, hours low, hours high)]."""
    if not meter or not cap:
        return []
    listen = meter.get("Idle + LoRa", LISTEN_MA)
    phone = meter.get("Idle + LoRa + BLE", PHONE_MA)
    ka9 = keepalive_ma(9)
    msg = TX_EXTRA_MA * (20 * airtime_ms(HEADER_BYTES + 20, 9) + 20 * airtime_ms(RECEIPT_BYTES, 9)) / 3.6e6
    play_s = 20 * 20 * 10 * 0.220   # 20 messages an hour of 20 characters, about 10 units a character, at Q4
    reading = MOTOR_FULL_MA * MORSE_DUTY * play_s / 3600.0
    uses = [
        ("Standby: listening, phone off", listen + ka9),
        ("Standby with the phone connected", phone + ka9),
        ("Messaging: phone connected, 20 messages an hour each way (20 characters, read at Q4)",
         phone + ka9 + msg + reading),
        ("Continuous reading: Morse playing all the time, phone off", listen + ka9 + MOTOR_FULL_MA * MORSE_DUTY),
    ]
    rows_out, tex, res = [], [], []
    wh_lo, wh_hi = cap["wh"]
    for name, ma in uses:
        p_w = ma / 1000.0 * METER_V
        lo, hi = wh_lo / p_w, wh_hi / p_w
        rows_out.append([name, round(ma, 1), round(p_w * 1000), round(lo), round(hi)])
        tex.append(f"{name} & {ma:.1f} & {p_w * 1000:.0f} & {lo:.0f}--{hi:.0f}\\,h ({lo / 24:.1f}--{hi / 24:.1f} days) \\\\")
        res.append((name, ma, lo, hi))
        note(f"battery life: {name}: mA, mW, hours low, hours high", (round(ma, 1), round(p_w * 1000), round(lo),
                                                                       round(hi)), "model")
    note("model pieces: keepalive mA, 20 msgs/h each way mA, reading 20 msgs/h mA",
         (round(ka9, 2), round(msg, 2), round(reading, 1)), "model")
    write_table(out, "battery_life", ["use", "mA at 4.08 V", "mW", "hours (low)", "hours (high)"], rows_out, tex)
    return res


# ---------------------------------------------------------------- 6. the motors (phyphox, TEST_PLAN 14.3)

# bench motor's pattern (bench.c): a 1 s start buzz, 2 s off; 3 s at
# 25 / 50 / 75 / 100 % with 2 s off after each; then for each unit ten dots
# (1 unit on, 1 off) and ten dashes (3 on, 1 off), each train followed by 2 s
# in all of silence; a 1 s end buzz.
BENCH_UNITS = [120, 140, 160, 180, 200, 220, 250, 280, 320, 100, 80, 60]
READ_SPEED = {120: "Q9", 140: "Q8", 160: "Q7", 180: "Q6", 200: "Q5", 220: "Q4", 250: "Q3", 280: "Q2", 320: "Q1"}
PEAK_MIN, TROUGH_MAX = 0.90, 0.25  # TEST_PLAN 14.3: a dot reaches 90 % of the steady level, a gap falls below 25 %


def bench_pattern():
    """The commanded pattern as [(start s, duration s, level 0..1, kind, unit ms)] and its total length (s).
    kind: start / steady / gap / dot / dot_gap / dash / dash_gap / end. Pure."""
    segs, t = [], 0.0

    def add(dur, lvl, kind, u=0):
        """Append one segment at the running time t (changes t)."""
        nonlocal t
        segs.append((t, dur, lvl, kind, u))
        t += dur
    add(1.0, 1.0, "start")
    add(2.0, 0.0, "gap")
    for lv in (64, 128, 191, 255):
        add(3.0, lv / 255, "steady")
        add(2.0, 0.0, "gap")
    for u in BENCH_UNITS:
        us = u / 1000
        for kind, on in (("dot", us), ("dash", 3 * us)):
            for _ in range(10):
                add(on, 1.0, kind, u)
                add(us, 0.0, kind + "_gap", u)
            add(2.0 - us, 0.0, "gap")
    add(1.0, 1.0, "end")
    return segs, t


def read_phyphox(folder):
    """(times s, |linear acceleration| m/s^2) of a phyphox export folder ("Raw Data.csv"), as numpy arrays."""
    with open(os.path.join(folder, "Raw Data.csv"), encoding="utf-8", errors="replace") as f:
        rows = list(csv.reader(f))[1:]
    a = np.array([[float(x) for x in r[:5]] for r in rows if len(r) >= 5])
    return a[:, 0], a[:, 4]


def envelope(mag, win=4):
    """The vibration's envelope: a running maximum over `win` samples (about
    19 ms at phyphox's 209 samples/s), centred. The accelerometer samples
    below the motor's 281 Hz, so single samples land at random phases; the
    maximum of a few recovers the amplitude."""
    e = np.array([mag[max(0, i - win + 1): i + 1].max() for i in range(len(mag))])
    sh = win // 2
    return np.concatenate([e[sh:], np.repeat(e[-1], sh)])


def align(t, env, segs):
    """The recording time (s) at which the pattern started: the offset that
    best correlates the commanded on/off with the envelope, searched around
    the first sustained buzz. Returns (offset, correlation)."""
    fs = 1 / float(np.median(np.diff(t)))
    thr = 0.3 * float(np.percentile(env, 99))
    above = env > thr
    k = int(0.5 * fs)
    on_i = next(i for i in range(len(t) - k) if above[i:i + k].all())
    best = (-2.0, 0.0)
    for off in np.arange(t[on_i] - 0.3, t[on_i] + 0.1, 0.002):
        c = np.zeros_like(t)
        for t0, d, lv, kind, u in segs:
            if lv > 0:
                c[(t >= off + t0) & (t < off + t0 + d)] = lv
        r = float(np.corrcoef(c, env)[0, 1])
        if r > best[0]:
            best = (r, float(off))
    return best[1], best[0]


def motor_metrics(t, env, segs, off):
    """What one recording says about its motor: steady levels (25..100 %,
    relative to 100 %), rise 10-90 % and fall 90-10 % (ms, median of the
    full-level buzzes), and per unit the dots' mean peak and the gaps'
    mean trough relative to the 100 % level. Returns a dict."""
    out = {"levels": {}, "rise": [], "fall": [], "units": {}}
    quiet = env[(t > off + 1.3) & (t < off + 2.8)]
    noise = float(np.median(quiet))

    def mid(t0, d):
        """Median envelope over the middle second of a segment starting at t0, lasting d."""
        m = (t >= off + t0 + d / 2 - 0.5) & (t < off + t0 + d / 2 + 0.5)
        return float(np.median(env[m]))
    steady = [(t0, d, lv) for t0, d, lv, kind, u in segs if kind == "steady"]
    full = mid(*steady[-1][:2])
    out["full"] = full
    for t0, d, lv in steady:
        out["levels"][round(lv * 100)] = mid(t0, d) / full
    for t0, d, lv, kind, u in segs:
        if kind not in ("start", "steady", "end") or lv < 0.99:
            continue
        top = mid(t0, d)
        lo, hi = noise + 0.1 * (top - noise), noise + 0.9 * (top - noise)
        m = (t >= off + t0 - 0.2) & (t < off + t0 + d + 0.5)
        tt, ee = t[m], env[m]
        i10, i90 = int(np.argmax(ee > lo)), int(np.argmax(ee > hi))
        j = int(np.where(tt > off + t0 + d - 0.05)[0][0])
        f90 = j + int(np.argmax(ee[j:] < hi))
        f10 = j + int(np.argmax(ee[j:] < lo))
        out["rise"].append(1000 * (tt[i90] - tt[i10]))
        out["fall"].append(1000 * (tt[f10] - tt[f90]))
    for u in BENCH_UNITS:
        dots = [(t0, d) for t0, d, lv, kind, uu in segs if kind == "dot" and uu == u]
        gaps = [(t0, d) for t0, d, lv, kind, uu in segs if kind == "dot_gap" and uu == u][:-1]
        pk = [float(env[(t >= off + t0) & (t < off + t0 + d + 0.02)].max()) / full for t0, d in dots]
        tr = [float(env[(t >= off + t0 + d * 0.5) & (t < off + t0 + d + 0.01)].min()) / full for t0, d in gaps]
        out["units"][u] = (statistics.mean(pk), statistics.mean(tr), max(tr))
    return out


def find_imu(root):
    """{name: folder} of the phyphox exports under root (folders holding "Raw Data.csv": M1, M2, Strength)."""
    found = {}
    for dp, _, names in os.walk(root):
        if "Raw Data.csv" in names:
            found[os.path.basename(dp)] = dp
    return found


def fig_motor(root, out):
    """Figure: (a) the envelope of the 120 ms (Q9) and 220 ms (Q4) dot
    trains on each motor against the commanded on/off; (b) how far each
    gap lets the motor stop (trough / steady level) against the unit.
    Table: rise and fall times, strength against drive, the fastest unit
    each motor keeps distinct. Returns nothing."""
    imu = find_imu(root)
    segs, total = bench_pattern()
    res = {}
    for name in ("M1", "M2", "M2_v064"):  # M2_v064: the replacement forearm motor, same pattern (firmware v0.6.4)
        if name not in imu:
            continue
        t, mag = read_phyphox(imu[name])
        env = envelope(mag)
        off, r = align(t, env, segs)
        res[name] = (t, env, off, motor_metrics(t, env, segs, off))
        fs = 1 / float(np.median(np.diff(t)))
        note(f"{name}: sample rate Hz, alignment corr", (round(fs, 1), round(r, 3)), "phyphox")
    if not res:
        print("  motor: no phyphox folders (M1, M2); skipped")
        return
    # the envelopes of two dot trains on each original motor (Appendix F)
    fig = plt.figure(figsize=(FIG_W, 2.9))
    gs = fig.add_gridspec(2, 1, hspace=0.55)
    for row, name in enumerate(("M1", "M2")):
        if name not in res:
            continue
        ax = fig.add_subplot(gs[row, 0])
        t, env, off, mt = res[name]
        for u, xpos in ((220, 0.0), (120, 4.8)):  # two trains side by side on one axis
            first = next(t0 for t0, d, lv, kind, uu in segs if kind == "dot" and uu == u)
            m = (t >= off + first - 0.1) & (t < off + first + 20 * u / 1000 + 0.1)
            xs = t[m] - (off + first) + xpos
            ax.plot(xs, env[m] / mt["full"], color=C[0] if name == "M1" else C[1], lw=0.9)
            cmd = [(t0 - first + xpos, d) for t0, d, lv, kind, uu in segs if kind == "dot" and uu == u]
            for c0, d in cmd:
                ax.axvspan(c0, c0 + d, color=GRID, lw=0, zorder=0)
            ax.text(xpos + 10 * u / 1000, 1.5, f"{u} ms ({READ_SPEED[u]})", ha="center", fontsize=7, color=INK2)
        ax.set_ylim(-0.05, 1.75)
        ax.set_xlim(-0.15, 4.8 + 2.5)
        ax.set_yticks([0, 0.5, 1.0])
        ax.set_ylabel(f"{name}\nrel. level", fontsize=8)
        ax.grid(axis="x", visible=False)
        if row == 0:
            ax.set_title("Ten dots at Q4 and at Q9")
        else:
            ax.set_xlabel("time from the first dot (s); grey: motor commanded on")
    save(fig, out, "motor_traces")

    hdr = ["motor", "rise 10-90 % ms", "fall 90-10 % ms", "25 % drive", "50 %", "75 %", "fastest unit passing ms",
           "per unit (peak, trough, worst trough)"]
    rows_out, tex = [], []
    for name in ("M1", "M2", "M2_v064"):
        if name not in res:
            continue
        mt = res[name][3]
        passing = [u for u in sorted(mt["units"]) if mt["units"][u][0] >= PEAK_MIN and mt["units"][u][1] <= TROUGH_MAX]
        # the fastest unit from which every slower unit also passes
        ok = None
        for u in sorted(mt["units"], reverse=True):
            if u in passing:
                ok = u
            else:
                break
        lv = mt["levels"]
        rows_out.append([name, round(median(mt["rise"])), round(median(mt["fall"])), round(lv.get(25, 0), 2),
                         round(lv.get(50, 0), 2), round(lv.get(75, 0), 2), ok,
                         {u: tuple(round(x, 2) for x in v) for u, v in mt["units"].items()}])
        label = {"M1": "M1 (wrist)", "M2": "M2 (forearm)", "M2_v064": "M2, replacement"}[name]
        tex.append(f"{label} & {median(mt['rise']):.0f} & "
                   f"{median(mt['fall']):.0f} & {100 * lv.get(25, 0):.0f}\\,\\% & {100 * lv.get(50, 0):.0f}\\,\\% & "
                   f"{100 * lv.get(75, 0):.0f}\\,\\% & {ok}\\,ms ({READ_SPEED.get(ok, 'faster than Q9')}) \\\\")
        note(f"{name}: rise ms, fall ms, levels 25/50/75 (rel. 100), fastest unit passing",
             (round(median(mt["rise"])), round(median(mt["fall"])),
              [round(lv.get(k, 0), 2) for k in (25, 50, 75)], ok), "phyphox")
        note(f"{name}: gap trough by unit", {u: round(v[1], 2) for u, v in sorted(mt["units"].items())}, "phyphox")
    write_table(out, "motor", hdr, rows_out, tex)
    return res

    # the Strength recording: its buzzes' levels, relative to the first
    if "Strength" in imu:
        t, mag = read_phyphox(imu["Strength"])
        env = envelope(mag)
        on = env > 0.3 * float(np.percentile(env, 99))
        bursts, i = [], 0
        while i < len(t):
            if on[i]:
                j = i
                while j < len(t) and (on[j] or (j + 40 < len(t) and on[j:j + 40].any())):
                    j += 1
                if t[j - 1] - t[i] > 2.0:
                    m = (t >= t[i] + 0.5) & (t < t[j - 1] - 0.5)
                    # plain floats, so numbers.txt shows 19.5 and not np.float64(19.5)
                    bursts.append((round(float(t[i]), 1), round(float(t[j - 1] - t[i]), 1), float(np.median(env[m]))))
                i = j
            i += 1
        if bursts:
            note("Strength recording: (start s, length s, level rel. first)",
                 [(b[0], b[1], round(b[2] / bursts[0][2], 2)) for b in bursts], "phyphox")



# ---------------------------------------------------------------- 6b. the strength sweep (firmware v0.6.6)

# `bench motor m1 all` / `m2 all`: nine blocks, I1 to I9, each 178.38 s from the
# one before: a 1 s start buzz at full, 2 s off, 3 s steady at the level (a
# 30 ms kick at full first, below full drive), 2 s off, then the dot and dash
# trains at the level. The replacement M2 was also run once on the first
# pattern (v0.6.4, `bench motor m2`): folder M2_v064.
SWEEP_BLOCK_S = 178.38
LEVEL_DRIVE = [45, 52, 59, 66, 74, 82, 91, 100, 100]  # % PWM; I9 is full drive at 3.4 V instead of 3.3 V


def sweep_block(level):
    """The pattern of one block of the sweep, as bench_pattern's segments: [(start s, duration s, level 0..1,
    kind, unit ms)]. Pure."""
    segs, t = [], 0.0

    def add(dur, lvl, kind, u=0):
        """Append one segment at the running time t (changes t)."""
        nonlocal t
        segs.append((t, dur, lvl, kind, u))
        t += dur
    add(1.0, 1.0, "start")
    add(2.0, 0.0, "gap")
    add(3.0, level, "steady")
    add(2.0, 0.0, "gap")
    for u in BENCH_UNITS:
        us = u / 1000
        for kind, on in (("dot", us), ("dash", 3 * us)):
            for _ in range(10):
                add(on, level, kind, u)
                add(us, 0.0, kind + "_gap", u)
            add(2.0 - us, 0.0, "gap")
    return segs


def read_xyz(folder):
    """(times s, [[x, y, z]] m/s^2) of a phyphox export folder, as numpy arrays."""
    with open(os.path.join(folder, "Raw Data.csv"), encoding="utf-8", errors="replace") as f:
        rows = list(csv.reader(f))[1:]
    a = np.array([[float(x) for x in r[:4]] for r in rows if len(r) >= 4])
    return a[:, 0], a[:, 1:4]


def sweep_metrics(folder):
    """What one sweep recording says, per level I1..I9: the steady buzz's RMS acceleration (over its middle 2 s,
    mean removed), the dominant frequency the phone saw in it (aliased: the phone samples at about 209 Hz, below
    the motor's speed), and per unit the gaps' mean trough relative to that level's steady envelope. Each block is
    lined up on its own start buzz. Returns [dict per level]."""
    t, xyz = read_xyz(folder)
    mag = np.sqrt((xyz ** 2).sum(axis=1))
    env = envelope(mag)
    fs = 1 / float(np.median(np.diff(t)))
    above = env > 0.3 * float(np.percentile(env, 99))
    k = int(0.5 * fs)
    first = t[next(i for i in range(len(t) - k) if above[i:i + k].all())]  # the first start buzz
    out = []
    for n in range(9):
        segs = sweep_block(1.0)
        # line this block up on its start buzz: the best match of on/off over its first 8 s
        best = (-2.0, first + n * SWEEP_BLOCK_S)
        for off in np.arange(first + n * SWEEP_BLOCK_S - 0.5, first + n * SWEEP_BLOCK_S + 0.3, 0.004):
            m = (t >= off - 0.5) & (t < off + 8)
            c = np.zeros(int(m.sum()))
            tt = t[m]
            c[(tt >= off) & (tt < off + 1.0)] = 1
            c[(tt >= off + 3.0) & (tt < off + 6.0)] = 1
            r = float(np.corrcoef(c, env[m])[0, 1])
            if r > best[0]:
                best = (r, float(off))
        off = best[1]
        sel = (t >= off + 3.5) & (t < off + 5.5)
        seg = xyz[sel] - xyz[sel].mean(axis=0)
        rms = float(np.sqrt((seg ** 2).sum(axis=1).mean()))
        z = seg[:, 2] * np.hanning(len(seg))
        sp = np.abs(np.fft.rfft(z))
        fr = np.fft.rfftfreq(len(z), 1 / fs)
        alias = float(fr[int(np.argmax(sp[3:])) + 3])
        steady = float(np.median(env[(t >= off + 4.0) & (t < off + 5.0)]))
        units = {}
        for u in BENCH_UNITS:
            dots = [(t0, d) for t0, d, lv, kind, uu in segs if kind == "dot" and uu == u]
            gaps = [(t0, d) for t0, d, lv, kind, uu in segs if kind == "dot_gap" and uu == u][:-1]
            pk = [float(env[(t >= off + t0) & (t < off + t0 + d + 0.02)].max()) / steady for t0, d in dots]
            tr = [float(env[(t >= off + t0 + d * 0.5) & (t < off + t0 + d + 0.01)].min()) / steady for t0, d in gaps]
            units[u] = (statistics.mean(pk), statistics.mean(tr))
        # the fastest unit from which every slower unit keeps its gaps below TROUGH_MAX
        fastest = None
        for u in sorted(units, reverse=True):
            if units[u][1] <= TROUGH_MAX:
                fastest = u
            else:
                break
        out.append({"level": n + 1, "corr": best[0], "rms": rms, "alias": alias, "fs": fs, "units": units,
                    "fastest": fastest})
    return out


def fig_strength(root, out, motors=None):
    """Figure: the strength sweep on both motors, (a) the vibration the phone measured at each level relative to
    I8, (b) the fastest unit at which the gaps still fall below 25 %. Notes the numbers, including the replacement
    M2 on the first pattern. Returns nothing."""
    imu = find_imu(root)
    res = {}
    for name in ("M1_all", "M2_all"):
        if name in imu:
            res[name[:2]] = sweep_metrics(imu[name])
    if "M2_v064" in imu:  # the replacement forearm motor on the first test's pattern
        segs, _ = bench_pattern()
        t, mag = read_phyphox(imu["M2_v064"])
        env = envelope(mag)
        off, r = align(t, env, segs)
        mt = motor_metrics(t, env, segs, off)
        ok = None
        for u in sorted(mt["units"], reverse=True):
            if mt["units"][u][0] >= PEAK_MIN and mt["units"][u][1] <= TROUGH_MAX:
                ok = u
            else:
                break
        note("M2 replacement (v0.6.4 pattern): rise ms, fall ms, levels 25/50/75, fastest unit passing",
             (round(median(mt["rise"])), round(median(mt["fall"])), [round(mt["levels"].get(k, 0), 2)
                                                                     for k in (25, 50, 75)], ok), "phyphox")
    if not res:
        print("  strength sweep: no phyphox folders (M1_all, M2_all); skipped")
        return
    fig, (ax0, ax1, ax2) = plt.subplots(1, 3, figsize=(FIG_W, 2.45), gridspec_kw={"width_ratios": [1.05, 1, 1]})
    # (a) the depth of the gaps at full drive, from the first pattern (fig_motor's results)
    shown = {"M1": "M1", "M2": "M2", "M2_v064": "M2, repl."}
    for name, col, mk, ls in (("M1", C[0], "o", "-"), ("M2", C[1], "s", "-"), ("M2_v064", C[1], "s", ":")):
        if not motors or name not in motors:
            continue
        mt = motors[name][3]
        us = sorted(mt["units"])
        ax0.plot(us, [mt["units"][u][1] for u in us], color=col, marker=mk, ms=3.5, ls=ls,
                 mec="white" if ls == "-" else col, mew=0.5 if ls == "-" else 0.9,
                 mfc=col if ls == "-" else "white", label=shown[name])
    ax0.axhline(TROUGH_MAX, color=INK2, lw=0.8)
    ax0.text(330, TROUGH_MAX + 0.02, "25 %", fontsize=7, color=INK2, ha="right", va="bottom")
    ax0.set_xlabel("unit (ms)")
    ax0.set_ylabel("lowest level in the gap\n(fraction of steady)")
    ax0.set_ylim(-0.02, 1.05)
    ax0.set_xlim(50, 335)
    ax0.set_title("(a) Gaps at full drive")
    ax0.legend(loc="upper right", fontsize=6.5)
    levels = list(range(1, 10))
    for name, col, mk in (("M1", C[0], "o"), ("M2", C[1], "s")):
        if name not in res:
            continue
        r = res[name]
        ref = r[7]["rms"]
        ax1.plot(levels, [x["rms"] / ref for x in r], color=col, marker=mk, ms=4.5, mec="white", mew=0.6,
                 label=f"{name}{'' if name == 'M1' else ', repl.'}")
        # M1's I2 vibrated within a few Hz of the phone's sampling rate: its envelope beats, so its gaps are not used
        pts = [(x["level"], x["fastest"]) for x in r if x["fastest"] and not (name == "M1" and x["alias"] < 5)]
        ax2.plot([p[0] for p in pts], [p[1] for p in pts], color=col, marker=mk, ms=4.5, mec="white", mew=0.6,
                 label=f"{name}{'' if name == 'M1' else ', repl.'}")
        note(f"sweep {name}: level rel. I8 (RMS) I1..I9", [round(x["rms"] / ref, 2) for x in r], "phyphox")
        note(f"sweep {name}: aliased frequency Hz I1..I9", [round(x["alias"], 1) for x in r], "phyphox")
        note(f"sweep {name}: fastest unit with gaps < 25 % I1..I9", [x["fastest"] for x in r], "phyphox")
        note(f"sweep {name}: dot peaks rel. steady at 220 ms I1..I9", [round(x["units"][220][0], 2) for x in r],
             "phyphox")
    ax1.plot(levels, [(d / 100) ** 2 for d in LEVEL_DRIVE], color=MUTED, lw=1.0, ls="--", label="drive$^2$")
    ax1.set_xticks(levels)
    ax1.set_xticklabels([f"I{i}" for i in levels])
    ax1.set_ylim(0, 1.6)
    ax1.set_ylabel("vibration (relative to I8)")
    ax1.set_title("(b) Level, I1–I9")
    ax1.legend(loc="lower right", fontsize=6.3)
    ax2.set_xticks(levels)
    ax2.set_xticklabels([f"I{i}" for i in levels])
    ax2.set_yticks([100, 120, 140, 160, 180, 200])
    ax2.set_ylim(90, 210)
    ax2.invert_yaxis()
    ax2.set_ylabel("fastest unit, gaps < 25 % (ms)")
    ax2.set_title("(c) Fastest unit, I1–I9")
    ax2.legend(loc="lower left", fontsize=6.5)
    for ax in (ax1, ax2):
        ax.tick_params(axis="x", labelsize=6.5)
    fig.tight_layout(w_pad=0.8)
    save(fig, out, "motor")

# ---------------------------------------------------------------- 7. the questionnaire (JS-04)

# The questionnaire's items, in its order: (column in participants.csv, short
# wording for the report's table, kind). Likert items are 1-5 (strongly
# disagree .. strongly agree); "scale" items have their own five options.
QUESTIONS = [
    ("q2_1", "I could easily feel the vibrations on my wrist", "likert"),
    ("q2_2", "I could tell short and long vibrations apart", "likert"),
    ("q2_3", "I could tell where one character ended and the next began", "likert"),
    ("q2_4", "The patterns were easy to follow at the speed sent", "likert"),
    ("q2_5", "Strength (1 too weak, 3 about right, 5 too strong)", "scale"),
    ("q3_1", "It was easy to tap out dots and dashes", "likert"),
    ("q3_2", "I felt confident my taps were registered correctly", "likert"),
    ("q3_3", "I could tap at a speed that felt natural", "likert"),
    ("q4_1", "The device was comfortable to wear", "likert"),
    ("q4_2", "I would wear it for a longer period", "likert"),
    ("q5_1", "The screen helped me understand what I felt", "likert"),
    ("q5_2", "The screen helped me link patterns to characters", "likert"),
    ("q5_3", "By the end I recognised some characters without the screen", "likert"),
    ("q5_4", "This set-up would help me learn Morse over time", "likert"),
    ("q5_5", "I would prefer learning this way to a book or app alone", "likert"),
    ("q5_6", "I relied on the screen rather than the vibration", "likert"),
    ("q5_7", "Display usefulness (1 not useful, 5 essential)", "scale"),
    ("q6_1", "Overall, the device was easy to use", "likert"),
    ("q6_2", "I would be interested in using a device like this", "likert"),
]
PART_OF = {"q2": "Feeling the vibrations", "q3": "Tapping", "q4": "Comfort", "q5": "Learning with the display",
           "q6": "Overall"}


def read_participants(root):
    """participants.csv anywhere under root: one row per participant, columns
    as QUESTIONS (and q1_1, q1_2, q5_8, q6_3..q6_5 as text). Returns the rows
    as dicts ([] if there is no file)."""
    for dp, _, names in os.walk(root):
        if "participants.csv" in names:
            with open(os.path.join(dp, "participants.csv"), encoding="utf-8-sig", errors="replace") as f:
                return [r for r in csv.DictReader(f) if any(v.strip() for v in r.values() if v)]
    return []


def table_questionnaire(root, out):
    """Table: each participant's answer to every rated item, with the median
    (blank answers left out). Writes tables/questionnaire.tex and .csv, and
    notes the background answers. Returns the number of participants."""
    rows = read_participants(root)
    if not rows:
        print("  questionnaire: no participants.csv yet; skipped")
        return 0
    n = len(rows)
    hdr = ["item"] + [f"P{i + 1}" for i in range(n)] + ["median"]
    rows_out, tex = [], []
    last_part = None
    for col, text, kind in QUESTIONS:
        part = PART_OF.get(col[:2], "")
        if part != last_part:
            if last_part is not None:
                tex.append("\\addlinespace[2pt]")
            tex.append(f"\\multicolumn{{{n + 2}}}{{@{{}}l}}{{\\textit{{{part}}}}} \\\\")
            last_part = part
        vals = []
        for r in rows:
            v = (r.get(col) or "").strip()
            vals.append(int(v) if v.isdigit() else None)
        got = [v for v in vals if v is not None]
        med = statistics.median(got) if got else None
        rows_out.append([text] + ["" if v is None else v for v in vals] + ["" if med is None else med])
        tex.append(f"{text} & " + " & ".join("--" if v is None else str(v) for v in vals) +
                   f" & {'--' if med is None else (str(int(med)) if med == int(med) else f'{med:.1f}')} \\\\")
    write_table(out, "questionnaire", hdr, rows_out, tex)
    for col in ("q1_1", "q1_2", "q5_8", "speed_q", "strength_i", "dash_pref"):
        note(f"questionnaire {col}", [r.get(col, "") for r in rows], "participants.csv")
    for col in ("q6_3", "q6_4", "q6_5"):
        note(f"questionnaire {col} (open)", [r.get(col, "") for r in rows], "participants.csv")
    return n



# ---------------------------------------------------------------- main

def main():
    """Read every test folder, write every figure and table, and numbers.txt. Returns the exit code."""
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", nargs="?", default=HERE, help="the Results folder (default: this file's folder)")
    ap.add_argument("-o", "--out", default=None, help="output folder (default: the Results folder)")
    a = ap.parse_args()
    out = a.out or a.root
    if plt is None:
        return at.finish("matplotlib is missing: run  pip install matplotlib  then this again.", 1)
    style()
    print("reading the session files ...")
    sessions = load_sessions(a.root)
    print(f"  {len(sessions)} sessions")
    for name, fn in (("latency", fig_latency), ("point tests", fig_points), ("dynamic", fig_dynamic)):
        print(f"{name} ...")
        fn(sessions, out)
    print("meter ...")
    meter = table_meter(a.root, out)
    print("run-downs ...")
    cap = fig_rundowns(a.root, sessions, out, meter)
    table_battery_life(meter, cap, out)
    print("motors ...")
    motors = fig_motor(a.root, out)
    print("strength sweep ...")
    fig_strength(a.root, out, motors)
    print("questionnaire ...")
    table_questionnaire(a.root, out)
    with open(os.path.join(out, "numbers.txt"), "w", encoding="utf-8") as f:
        for k, v, src in NUMBERS:
            f.write(f"{k}: {v}    [{src}]\n")
    return at.finish(f"Figures in {os.path.join(out, 'figures')}, tables in {os.path.join(out, 'tables')}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        traceback.print_exc()
        sys.exit(at.finish("\nSomething went wrong (see the lines above).", 2))
