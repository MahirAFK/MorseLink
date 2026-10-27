#!/usr/bin/env python3
r"""
MorseLink - analyse field-test logs (docs/TEST_PLAN.md section 4.6).

Reads the phones' session files (ML_<session>_<rover|base>_<ID>.jsonl),
pairs the rover's and the base's files of each session, and writes:

  results/summary.txt     per session: delivery rate, first-try rate and
                          round trip per path and direction, by message size,
                          by area and situation, signal strength, battery,
                          skips; the LoRa link's lost periods and its
                          availability (rebuilt for files from boards before
                          v0.6.0, which didn't log losses); then all sessions
                          by range setting (SF, power); then the run-downs
  results/messages.csv    one row per test message copy (open in Excel):
                          when, who sent it, path, size, delivered or not,
                          tries, round trip, distance, RSSI/SNR, area,
                          situation, SF and power
  results/sessions.csv    one row per session and direction (for tables
                          in the report)
  results/lost.csv        one row per lost period of the LoRa link
  results/rundowns.csv    the battery run-downs (`test dump`), one row per
                          board-minute (and, once a run-down A has run to
                          empty, where each reading sits on its curve)
  results/points.csv      (v0.6.1) the point tests: one row per session, step
                          and direction: the spot's label, the setting, sent /
                          delivered / failed / first try, RSSI and SNR (median
                          and min) as the receiver heard the sender, the
                          margin above the SF's decoding limit, the distance;
                          summary.txt adds the attenuation of each obstacle
                          (against its clear reference) and the path-loss
                          exponent of the distance ladder

Point tests, how the labels are read (TEST_PLAN section 17):
  - pairs: labels that start with the same words before the first comma
    belong together, e.g. "pillar A, clear, 12 m" and "pillar A, behind,
    12 m". The one with "clear" (or "ref") in it is the reference; each other
    one's loss = the reference's RSSI - its RSSI, per setting and direction.
    The loss shouldn't depend on the power: the 17 and 22 dBm steps of one
    sweep should give the same loss (a built-in check, printed too).
  - the distance ladder: labels with "open" (or "ladder") and a distance,
    e.g. "open, 200 m" (the distance from the label; without one, from the
    two phones' GPS). RSSI against log10(distance) per setting gives the
    path-loss exponent n (RSSI = A - 10 n log10(d)).

Run-down dumps: save what `test dump` printed (the serial monitor's text, as
it is) as a .txt or .csv file in data\ too; the script finds the
"# morselink-rundown" line anywhere in a file.

Set-up (Python 3.8+, nothing to install): keep this file in a folder of
its own, with a folder "data" next to it.

  MorseLink tests\            (any name, anywhere)
    analyse_tests.py
    data\                     both phones' ML_... files (subfolders are fine)
    results\                  made by the script

Run it from any terminal, or double-click it:
  python "C:\path\to\MorseLink tests\analyse_tests.py"
Each run reads everything in data\ again and replaces the files in
results\ (close them in Excel first). The first run makes data\ if it is
missing.

Other folders can still be given instead:
  python analyse_tests.py <folders or files> [more ...] [-o <results folder>]

Daily logs (ML_log_*.jsonl) are skipped. Files of the same session from
both phones are matched by the session ID in the name.
"""

import argparse
import bisect
import csv
import json
import math
import os
import re
import statistics
import sys
import traceback
from collections import defaultdict

NAME = re.compile(r"ML_(\d{8}-\d{6}|[A-Za-z0-9-]+)_(rover|base)_([A-Z0-9]+)\.jsonl$")

# The default folders: next to this file, wherever it is run from.
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
RESULTS = os.path.join(HERE, "results")


# ---------------------------------------------------------------- reading


def find_files(paths):
    """All session files under the given folders/files (daily logs skipped)."""
    out = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                out += [os.path.join(root, n) for n in names]
        else:
            out.append(p)
    return [f for f in out if NAME.search(os.path.basename(f))]


def read_jsonl(path):
    """Records of one file; a broken line (e.g. cut off by a crash) is skipped."""
    recs = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                recs.append(json.loads(line))
            except ValueError:
                pass
    return recs


class Side:
    """One phone's file of a session: its board's events, GPS and labels."""

    def __init__(self, path):
        m = NAME.search(os.path.basename(path))
        self.session, self.role, self.own = m.group(1), m.group(2), m.group(3)
        self.path = path
        self.recs = read_jsonl(path)
        self.meta = next((r for r in self.recs if r.get("kind") == "session"), {})
        self.peer = self.meta.get("peer", "")
        self.board = [r for r in self.recs if r.get("kind") == "board"]
        self.board.sort(key=lambda r: (r.get("t", 0), r.get("n", 0)))
        # The board's own START line says what it ran with (sf= since v0.6.0;
        # before, every board was on SF9).
        start = next((r for r in self.board if r.get("ev") == "START"), {})
        self.start = start
        self.sf = start.get("sf", self.meta.get("sf", 9))
        self.tx_dbm = start.get("tx_dbm", self.meta.get("tx_dbm", ""))
        self.fw = start.get("fw", self.meta.get("fw", ""))
        self.gps = [(r["t"], r["lat"], r["lon"], r.get("acc", 99)) for r in self.recs
                    if r.get("kind") == "gps" and "lat" in r]
        self.gps.sort()
        self.labels = sorted((r["t"], r.get("area", ""), r.get("sit", ""), r.get("note", ""))
                             for r in self.recs if r.get("kind") == "label")
        self.labels = corrected_labels(self, self.labels)  # (corrections.txt)
        # the base's position as the rover heard it (cmd pos over MQTT)
        self.base_pos = [(r["t"], r["msg"]["lat"], r["msg"]["lon"]) for r in self.recs
                         if r.get("kind") == "ctl" and r.get("dir") == "in"
                         and isinstance(r.get("msg"), dict) and r["msg"].get("cmd") == "pos"]

    def ev(self, name):
        return [r for r in self.board if r.get("ev") == name]


# ---------------------------------------------------------------- helpers


def metres(lat1, lon1, lat2, lon2):
    """Great-circle distance in metres."""
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def nearest(track, t, max_gap_ms=10000):
    """The GPS fix closest in time to t (None if none within max_gap_ms)."""
    if not track:
        return None
    times = [x[0] for x in track]
    i = bisect.bisect_left(times, t)
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(track) and (best is None or abs(track[j][0] - t) < abs(best[0] - t)):
            best = track[j]
    return best if best and abs(best[0] - t) <= max_gap_ms else None


def fixed_position(side):
    """A board that doesn't move: the median of its good fixes (acc <= 30 m)."""
    good = [g for g in side.gps if g[3] <= 30] or side.gps
    if not good:
        return None
    return statistics.median(g[1] for g in good), statistics.median(g[2] for g in good)


# Label corrections (corrections.txt in the data folder; added 21 Oct). One
# rule a line, '#' starts a comment:
#     <session> <situation> <area>
# e.g. "20261018-184526 indoors dense_urban": in that session, every stretch
# labelled with that situation counts as that area. "<situation>+lost"
# (e.g. "indoors+lost") limits it to the stretches during which this board's
# LoRa link was lost at some point: Dynamic 2's indoor stretches with an
# outage were the mall (left on urban); indoor stretches without one may
# have been at home and keep their label. The phones' files are not changed;
# summary.txt lists every rule it applied.
CORRECTIONS = {}  # session -> [(situation, area)]


def read_corrections(folders):
    """Load corrections.txt from each input folder into CORRECTIONS; returns
    the rules as text lines for summary.txt."""
    used = []
    for d in folders:
        path = os.path.join(d, "corrections.txt") if os.path.isdir(d) else ""
        if not path or not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.split("#", 1)[0].split()
                if len(parts) == 3:
                    CORRECTIONS.setdefault(parts[0], []).append((parts[1], parts[2]))
                    used.append(f"  session {parts[0]}: '{parts[1]}' counts as area '{parts[2]}'")
    return used


def corrected_labels(side, labels):
    """The labels with the session's corrections applied: each label gets the
    area and situation in force from it on, and a corrected situation also
    sets its area (a label record only changes what it names). A rule
    "<situation>+lost" applies only to the stretches (label to next label)
    that overlap one of this board's lost periods."""
    rules = CORRECTIONS.get(side.session, [])
    if not rules:
        return labels
    periods = lost_periods(side) if any(r[0].endswith("+lost") for r in rules) else []
    end_all = side.board[-1]["t"] if side.board else (labels[-1][0] if labels else 0)
    out, area, sit = [], "", ""
    for i, (lt, a, s_, note) in enumerate(labels):
        area, sit = a or area, s_ or sit
        until = labels[i + 1][0] if i + 1 < len(labels) else end_all
        lost_here = any(p["start"] < until and (p["end"] or end_all) > lt for p in periods)
        new_area = area
        for situation, to_area in rules:
            if situation == sit or (situation == sit + "+lost" and lost_here):
                new_area = to_area
        out.append((lt, new_area, sit, note))
        # (the next label inherits the real area, not the corrected one)
    return out


def label_at(labels, t):
    """Area and situation in force at time t (the last label before it)."""
    area = sit = ""
    for lt, a, s, _ in labels:
        if lt > t:
            break
        area, sit = a or area, s or sit
    return area, sit


def pct(a, b):
    return "-" if b == 0 else f"{100.0 * a / b:.1f} %"


def fw_at_least(fw, want):
    """Is firmware version fw ("0.5.1") at least want ((0, 6, 0))?"""
    try:
        return tuple(int(x) for x in str(fw).split(".")[:3]) >= want
    except ValueError:
        return False


def lost_ms_for(sf):
    """The time without proof after which a board calls the link lost, at
    this SF (firmware link_timing(): 90 s at SF9 and SF10, 135 s at SF11,
    180 s at SF12)."""
    return {11: 135000, 12: 180000}.get(sf, 90000)


def size_band(size):
    """The four size bands of the dynamic test (5-100 characters at random)."""
    for hi in (25, 50, 75, 100):
        if size <= hi:
            return f"{hi - 24 if hi > 25 else 5:>3}-{hi:<3}"
    return f"{size}"


def q(values, p):
    """The p-quantile (0..1) of a list, '-' if empty."""
    if not values:
        return "-"
    v = sorted(values)
    return round(v[min(len(v) - 1, int(p * len(v)))])


# ---------------------------------------------------------------- analysis


def messages(sender, receiver, rover, base):
    """One row per test message copy that `sender`'s board made."""
    acks = {(r["path"], r["seq"]): r for r in sender.ev("ACK")}
    fails = {(r["path"], r["seq"]): r for r in sender.ev("FAIL")}
    heard = {}  # (path, seq) -> the receiver's RX (LoRa: RSSI/SNR) or RIN (relay)
    if receiver:
        for r in receiver.ev("RX") + receiver.ev("RIN"):
            if r.get("type") == "MSG" and r.get("src") == sender.own and "seq" in r and not r.get("dup"):
                heard.setdefault((r["path"], r["seq"]), r)
    base_fix = fixed_position(base) if base else None
    rows = []
    for g in sender.ev("GEN"):
        t = g["t"]
        # distance: the rover's fix at the time vs the (fixed) base
        dist = ""
        rf = nearest(rover.gps, t) if rover else None
        bp = base_fix
        if bp is None and rover and rover.base_pos:
            bp = rover.base_pos[-1][1:]
        if rf and bp:
            dist = round(metres(rf[1], rf[2], bp[0], bp[1]))
        area, sit = label_at(rover.labels if rover else [], t)
        for path, on_key, skip_key in (("L", "lora", "lora_skip"), ("R", "relay_id", "relay_skip")):
            if skip_key in g:
                outcome, why = "skipped", g[skip_key]
            elif on_key not in g or g.get(on_key) == "off":
                continue  # this path wasn't in the test
            elif (path, g["seq"]) in acks:
                outcome, why = "delivered", ""
            elif (path, g["seq"]) in fails:
                outcome, why = "failed", fails[(path, g["seq"])].get("why", "")
            else:
                outcome, why = "unknown", "no result (session ended)"
            a = acks.get((path, g["seq"]), {})
            h = heard.get((path, g["seq"]), {})
            rows.append({
                "session": sender.session, "from": sender.own, "from_role": sender.role,
                "to": sender.peer, "seq": g["seq"], "size": g["size"], "step": g.get("step", ""),
                "path": "LoRa" if path == "L" else "relay", "outcome": outcome, "why": why,
                "rtt_ms": a.get("rtt_ms", ""), "tries": a.get("tries", ""),
                "distance_m": dist, "rssi": h.get("rssi", ""), "snr": h.get("snr", ""),
                "area": area, "situation": sit, "phone_time_ms": t,
                "sf": sender.sf, "tx_dbm": sender.tx_dbm, "test": sender.meta.get("test", ""),
            })
    return rows


def lost_periods(side):
    """
    The LoRa link's lost periods as this board saw them, as dicts {start,
    end (phone ms), why, rebuilt}. During a test the link is LoRa alone (the
    test sends its own relay copies), so these are LoRa's outages.

    Since v0.6.0 the board logs every loss (LINK state=lost, with why) and
    every return (restored). Boards before that logged only the returns, so
    their losses are rebuilt (HANDOVER 5.1f): the link is down from a failed
    L copy (no-receipt), or from the lost time after the last packet heard
    over LoRa (silence), until the next restored.
    """
    evs = side.board
    if not evs:
        return []
    exact = fw_at_least(side.fw, (0, 6, 0)) or any(
        r.get("ev") == "LINK" and r.get("state") == "lost" for r in evs)
    lost_ms = lost_ms_for(side.sf)
    start_t = side.start.get("t", evs[0]["t"])
    stop = next((r for r in evs if r.get("ev") == "STOP"), None)
    end_t = stop["t"] if stop else evs[-1]["t"]
    out = []
    down = None  # (since, why, rebuilt)
    last_rx = start_t
    for r in evs:
        t, ev = r["t"], r.get("ev")
        if t > end_t:
            break
        if not exact and down is None and t - last_rx > lost_ms:
            down = (last_rx + lost_ms, "silence", True)
        if ev == "RX":
            last_rx = t
        elif ev == "FAIL" and r.get("path") == "L" and not exact and down is None:
            down = (t, r.get("why", "no-receipt"), True)
        elif ev == "LINK":
            st = r.get("state")
            if st in ("lost", "ended") and down is None:
                down = (t, r.get("why", st), False)
            elif st == "restored" and down is not None:
                out.append({"start": down[0], "end": t, "why": down[1], "rebuilt": down[2]})
                down = None
                last_rx = t
    if not exact and down is None and end_t - last_rx > lost_ms:
        down = (last_rx + lost_ms, "silence", True)
    if down is not None:
        out.append({"start": down[0], "end": end_t, "why": down[1], "rebuilt": down[2], "open": True})
    return out


def path_shares(side):
    """Since v0.6.0: how long LoRa spent in each state (PATH path=L lines),
    as {state: ms}, from the START to the STOP; {} for older boards."""
    evs = [r for r in side.board if r.get("ev") == "PATH" and r.get("path") == "L"]
    if not evs:
        return {}
    start_t = side.start.get("t", side.board[0]["t"])
    stop = next((r for r in side.board if r.get("ev") == "STOP"), None)
    end_t = stop["t"] if stop else side.board[-1]["t"]
    shares = defaultdict(int)
    state, since = "connected", start_t  # (a test only starts with LoRa up or checking)
    for r in evs:
        shares[state] += max(0, r["t"] - since)
        state, since = r.get("state", "?"), r["t"]
    shares[state] += max(0, end_t - since)
    return dict(shares)


def time_in_labels(labels, start_t, end_t, periods):
    """{(area, situation): (ms in it, ms of that lost)}: the rover's labels
    against a board's lost periods."""
    marks = sorted([(start_t, None)] + [(lt, (a, s)) for lt, a, s, _ in labels if start_t <= lt <= end_t]
                   + [(end_t, None)])
    area, sit = label_at(labels, start_t)
    out = defaultdict(lambda: [0, 0])
    for (t0, lab), (t1, _) in zip(marks, marks[1:]):
        if lab:
            area, sit = lab[0] or area, lab[1] or sit
        if t1 <= t0:
            continue
        k = (area or "?", sit or "?")
        out[k][0] += t1 - t0
        for p in periods:
            out[k][1] += max(0, min(t1, p["end"]) - max(t0, p["start"]))
    return out


def summarise(sid, sides, rows, out):
    rover, base = sides.get("rover"), sides.get("base")
    any_side = rover or base
    m = any_side.meta
    w = out.append
    w("=" * 72)
    w(f"Session {sid}: {m.get('test')} test, paths {m.get('paths')}, SF{any_side.sf} "
      f"(range level {int(any_side.sf) - 8}), tx {any_side.tx_dbm} dBm, fw {any_side.fw}"
      + (" (the boards' own setting: the plan's steps below)" if is_point(sides) else ""))
    for s in sides.values():
        if s.sf != any_side.sf:
            w(f"  WARNING: the {s.role} board ran at SF{s.sf}: the two boards were on different range levels")
    w("  files: " + ", ".join(f"{k} {os.path.basename(s.path)}" for k, s in sides.items()))
    if not (rover and base):
        w("  NOTE: only one phone's file - receive-side numbers (RSSI) are missing.")
    for s in sides.values():
        st = s.ev("STOP")
        if st:
            w(f"  {s.role} board {s.own}: stopped ({st[-1].get('reason')}) after "
              f"{st[-1].get('gen')} messages")
        bad = len(s.ev("RXBAD"))
        rx = len(s.ev("RX"))
        w(f"  {s.role} board {s.own}: {rx} packets heard over LoRa, {bad} damaged (RXBAD)")
        b = [r for r in s.ev("STAT") if r.get("bat_mv")]
        if b:
            mins = (b[-1]["t"] - b[0]["t"]) / 60000
            noise = [r["noise_dbm"] for r in b if "noise_dbm" in r]
            w(f"  {s.role} battery: {b[0]['bat_mv']} -> {b[-1]['bat_mv']} mV "
              f"({b[0].get('bat_pct')} -> {b[-1].get('bat_pct')} %) over {mins:.0f} min"
              + (f"; noise floor {statistics.median(noise):.0f} dBm" if noise else ""))
    dists = [r["distance_m"] for r in rows if r["distance_m"] != ""]
    if dists:
        w(f"  distance rover-base: median {statistics.median(dists):.0f} m "
          f"(min {min(dists)}, max {max(dists)}) - GPS based, +/- the fixes' accuracy")

    def block(title, sel):
        w("")
        w(f"  {title}")
        w(f"    {'':28}{'made':>6}{'deliv.':>8}{'failed':>8}{'skip':>6}{'rate':>9}{'1st try':>9}"
          f"{'rtt med':>9}{'rtt p95':>9}")
        for key, grp in sel:
            n = len(grp)
            d = [r for r in grp if r["outcome"] == "delivered"]
            f = sum(r["outcome"] == "failed" for r in grp)
            sk = sum(r["outcome"] == "skipped" for r in grp)
            first = sum(1 for r in d if r["tries"] == 1)
            rtt = [r["rtt_ms"] for r in d if r["rtt_ms"] != ""]
            w(f"    {key:28}{n:>6}{len(d):>8}{f:>8}{sk:>6}{pct(len(d), len(d) + f):>9}"
              f"{pct(first, len(d) + f):>9}{q(rtt, .5):>9}{q(rtt, .95):>9}")

    def groups(keyf, rs):
        g = defaultdict(list)
        for r in rs:
            g[keyf(r)].append(r)
        return sorted(g.items(), key=lambda kv: str(kv[0]))

    if not rows:  # run-down A, or stopped before the first message
        w("")
        w("  No test messages in this session.")
        w("")
        return
    block("By direction and path (rate = delivered / (delivered + failed); 1st try = delivered on its first "
          "copy, of the same; skips excluded)",
          groups(lambda r: f"{r['from_role']}->{'base' if r['from_role'] == 'rover' else 'rover'} {r['path']}",
                 rows))
    if any(r["step"] != "" for r in rows):  # a point test: its steps
        plan = plan_of(any_side)
        w("")
        w(f"  Point test at \"{label_of(sides)}\": plan " + ", ".join(f"SF{sf}/{d} dBm" for sf, d in plan))

        def step_key(r):
            sf, d = plan[r["step"] - 1] if 0 < r["step"] <= len(plan) else ("?", "?")
            return f"{r['step']} SF{sf}/{d} {r['from_role']}->{other(r['from_role'])}"
        block("By step and direction", groups(step_key, [r for r in rows if r["step"] != ""]))
    elif len({r["size"] for r in rows}) <= 4:  # baseline: 5 / 25 / 50 / 100
        block("By size (both directions)", groups(lambda r: f"{r['path']} {r['size']:>3} chars", rows))
    else:  # dynamic: 5-100 at random, in four bands
        block("By size (both directions), in bands",
              groups(lambda r: f"{r['path']} {size_band(r['size'])} chars", rows))
    if any(r["area"] or r["situation"] for r in rows):
        block("By area / situation (rover's labels)",
              groups(lambda r: f"{r['path']} {r['area'] or '?'}/{r['situation'] or '?'}"[:28], rows))
    if dists:
        def band(r):
            if r["distance_m"] == "":
                return "LoRa ? m"
            lo = int(r["distance_m"]) // 250 * 250
            return f"{r['path']} {lo}-{lo + 250} m"
        block("By distance (250 m bands)", groups(band, rows))
    lora = [r for r in rows if r["path"] == "LoRa" and r["rssi"] != ""]
    if lora:
        w("")
        w("  LoRa signal at the receiver:")
        for d, grp in groups(lambda r: f"{r['from_role']}->{'base' if r['from_role'] == 'rover' else 'rover'}", lora):
            rs, sn = [r["rssi"] for r in grp], [r["snr"] for r in grp]
            w(f"    {d:22} RSSI median {statistics.median(rs):.0f} dBm (min {min(rs)}), "
              f"SNR median {statistics.median(sn):.1f} dB (min {min(sn)})")
    whys = defaultdict(int)
    for r in rows:
        if r["outcome"] in ("skipped", "failed", "unknown"):
            whys[f"{r['path']} {r['outcome']}: {r['why']}"] += 1
    if whys:
        w("")
        w("  Not delivered, why:")
        for k, v in sorted(whys.items()):
            w(f"    {k}: {v}")
    lost_report(sides, w)
    notes = [l for s in sides.values() for l in s.labels if l[3]]
    if notes:
        w("")
        w("  Notes:")
        for t, _, _, n in notes:
            w(f"    {(t - any_side.recs[0]['t']) / 60000:6.1f} min  {n}")
    w("")


def lost_report(sides, w):
    """The LoRa link's lost periods and availability, per board."""
    rover = sides.get("rover")
    first_t = min(s.recs[0]["t"] for s in sides.values() if s.recs)
    w("")
    w("  LoRa link (each board's view: lost = the board found it unusable; during a test the link is LoRa alone)")
    for s in sides.values():
        if not s.board:
            continue
        periods = lost_periods(s)
        start_t = s.start.get("t", s.board[0]["t"])
        stop = next((r for r in s.board if r.get("ev") == "STOP"), None)
        end_t = stop["t"] if stop else s.board[-1]["t"]
        span = max(1, end_t - start_t)
        down = sum(p["end"] - p["start"] for p in periods)
        rebuilt = any(p["rebuilt"] for p in periods)
        w(f"    {s.role} {s.own}: up {100.0 * (span - down) / span:.1f} % of {span / 60000:.0f} min; "
          f"lost {len(periods)} time(s), {down / 60000:.1f} min in all"
          + (f" (longest {max(p['end'] - p['start'] for p in periods) / 60000:.1f} min)" if periods else "")
          + (" - rebuilt: this board's firmware didn't log losses" if rebuilt else ""))
        for p in periods:
            w(f"      {(p['start'] - first_t) / 60000:6.1f} - {(p['end'] - first_t) / 60000:6.1f} min "
              f"{(p['end'] - p['start']) / 60000:6.1f} min  {p['why']}"
              + ("  (still lost at the end)" if p.get("open") else ""))
        shares = path_shares(s)
        if shares:
            tot = max(1, sum(shares.values()))
            w("      LoRa path states: " + ", ".join(
                f"{k} {100.0 * v / tot:.1f} %" for k, v in sorted(shares.items(), key=lambda kv: -kv[1])))
        if rover and rover.labels:
            tl = time_in_labels(rover.labels, start_t, end_t, periods)
            parts = [f"{a}/{si} up {100.0 * (ms - lo) / ms:.0f} % of {ms / 60000:.0f} min"
                     for (a, si), (ms, lo) in sorted(tl.items()) if ms >= 30000]
            if parts:
                w("      by the rover's labels: " + "; ".join(parts))


def lost_rows(sid, sides):
    """lost.csv: one row per lost period."""
    out = []
    first_t = min(s.recs[0]["t"] for s in sides.values() if s.recs)
    for s in sides.values():
        for p in lost_periods(s):
            out.append({"session": sid, "board": s.own, "role": s.role, "sf": s.sf, "tx_dbm": s.tx_dbm,
                        "start_min": round((p["start"] - first_t) / 60000, 2),
                        "end_min": round((p["end"] - first_t) / 60000, 2),
                        "minutes": round((p["end"] - p["start"]) / 60000, 2), "why": p["why"],
                        "rebuilt": int(p["rebuilt"]), "open_at_end": int(bool(p.get("open")))})
    return out


def setting_block(all_rows, w):
    """All sessions by range setting: test, SF, power, path."""
    if not all_rows:
        return
    w("=" * 72)
    w("All sessions by range setting (rate = delivered / (delivered + failed); skips excluded)")
    w(f"    {'test / SF / dBm / path':30}{'made':>6}{'deliv.':>8}{'failed':>8}{'skip':>6}{'rate':>9}"
      f"{'1st try':>9}{'rtt med':>9}")
    for (test, sf, tx, path), grp in sorted(_group(all_rows, lambda r: (r["test"], r["sf"], r["tx_dbm"],
                                                                        r["path"])).items(), key=str):
        d = [r for r in grp if r["outcome"] == "delivered"]
        f = sum(r["outcome"] == "failed" for r in grp)
        sk = sum(r["outcome"] == "skipped" for r in grp)
        first = sum(1 for r in d if r["tries"] == 1)
        rtt = [r["rtt_ms"] for r in d if r["rtt_ms"] != ""]
        key = f"{test} SF{sf} {tx} dBm {path}"
        w(f"    {key:30}{len(grp):>6}{len(d):>8}{f:>8}{sk:>6}{pct(len(d), len(d) + f):>9}"
          f"{pct(first, len(d) + f):>9}{q(rtt, .5):>9}")
    w("")


def session_rows(sid, rows):
    out = []
    for (frm, path), grp in sorted(_group(rows, lambda r: (r["from_role"], r["path"])).items()):
        d = [r for r in grp if r["outcome"] == "delivered"]
        f = sum(r["outcome"] == "failed" for r in grp)
        rtt = [r["rtt_ms"] for r in d if r["rtt_ms"] != ""]
        first = sum(1 for r in d if r["tries"] == 1)
        out.append({"session": sid, "test": "", "sf": grp[0]["sf"], "tx_dbm": grp[0]["tx_dbm"],
                    "from_role": frm, "path": path, "made": len(grp),
                    "delivered": len(d), "failed": f,
                    "skipped": sum(r["outcome"] == "skipped" for r in grp),
                    "rate_pct": "" if not (len(d) + f) else round(100 * len(d) / (len(d) + f), 1),
                    "first_try_pct": "" if not (len(d) + f) else round(100 * first / (len(d) + f), 1),
                    "rtt_median_ms": q(rtt, .5), "rtt_p95_ms": q(rtt, .95)})
    return out


def _group(rows, keyf):
    g = defaultdict(list)
    for r in rows:
        g[keyf(r)].append(r)
    return g


# ---------------------------------------------------------------- point tests (v0.6.1)

# LoRa's decoding limit (SNR, dB) per SF: below it nothing decodes. The
# margin = the median SNR - this.
DECODE_LIMIT_DB = {9: -12.5, 10: -15.0, 11: -17.5, 12: -20.0}


def is_point(sides):
    """Is this session a point test (either board's START, or the phones' record)?"""
    return any(s.meta.get("test") == "point" or s.start.get("test") == "point" for s in sides.values())


def plan_of(side):
    """The plan the board ran, [(sf, dBm), ...]: its START line's
    plan=9:17/9:22/12:22 (or the phone's session record, 9:17,9:22,12:22).
    The STEP lines say the power each board really used (a chip clamps it);
    step_windows() has that."""
    txt = str(side.start.get("plan") or side.meta.get("plan") or "")
    out = []
    for part in re.split(r"[,/]", txt):
        m = re.fullmatch(r"\s*(\d+):(\d+)\s*", part)
        if m:
            out.append((int(m.group(1)), int(m.group(2))))
    return out


def label_of(sides):
    """The spot's label: the rover's session record, or a later label record
    (typed during the test); the base's copy if the rover's file is missing."""
    for role in ("rover", "base"):
        s = sides.get(role)
        if not s:
            continue
        lab = s.meta.get("label", "")
        for r in s.recs:
            if r.get("kind") == "label" and r.get("label"):
                lab = r["label"]
        if lab:
            return lab
    return ""


def step_windows(side):
    """This board's steps, from its STEP lines (phone time): {i: (start_t,
    end_t, sf, dBm used)}. A step ends at the next STEP line (the last one,
    i=0 restored=1, ends the plan) or the STOP."""
    steps = side.ev("STEP")
    stop = side.ev("STOP")
    last_t = stop[-1]["t"] if stop else (side.board[-1]["t"] if side.board else 0)
    out = {}
    for k, r in enumerate(steps):
        i = r.get("i", 0)
        if not i:
            continue
        end = steps[k + 1]["t"] if k + 1 < len(steps) else last_t
        out[i] = (r["t"], end, r.get("sf"), r.get("dbm"))
    return out


DIST_IN_LABEL = re.compile(r"(\d+(?:\.\d+)?)\s*m\b", re.I)


def point_distance(sides, label):
    """The rover-base distance: the number in the label ("200 m"), else the
    two phones' GPS (both still: each one's median fix). ('', '') if neither."""
    m = DIST_IN_LABEL.search(label or "")
    if m:
        return float(m.group(1)), "label"
    rover, base = sides.get("rover"), sides.get("base")
    a = fixed_position(rover) if rover else None
    b = fixed_position(base) if base else None
    if a and b:
        return round(metres(a[0], a[1], b[0], b[1]), 1), "gps"
    return "", ""


def med(v):
    """The median, one decimal ('' if empty)."""
    return round(statistics.median(v), 1) if v else ""


def other(role):
    return "base" if role == "rover" else "rover"


def point_rows(sid, sides):
    """points.csv: one row per step and direction of a point test. The
    sender's GEN / ACK / FAIL lines give its messages' fate (by the step each
    was made in); the receiver's RX lines of anything the sender transmitted
    during the receiver's own step i (messages, their retries, receipts,
    keepalives) give the signal: the settle time at each step's start keeps
    the two boards' switching moments apart."""
    rows = []
    label = label_of(sides)
    dist, dist_src = point_distance(sides, label)
    rover, base = sides.get("rover"), sides.get("base")
    for snd, rcv in ((rover, base), (base, rover)):
        if not snd:
            if rcv:  # the sender's phone lost its board (21 Oct): what the receiver heard
                rows += heard_only_rows(sid, rcv, "base" if rcv.role == "rover" else "rover", label, dist, dist_src)
            continue
        plan = plan_of(snd)
        win_s = step_windows(snd)
        win_r = step_windows(rcv) if rcv else {}
        acks = {r["seq"]: r for r in snd.ev("ACK") if r.get("path") == "L"}
        fails = {r["seq"]: r for r in snd.ev("FAIL") if r.get("path") == "L"}
        gens = snd.ev("GEN")
        for i in range(1, len(plan) + 1):
            mine = [g for g in gens if g.get("step") == i]
            sent = [g for g in mine if g.get("lora") == "queued"]
            skipped = [g for g in mine if "lora_skip" in g]
            dl = [acks[g["seq"]] for g in sent if g["seq"] in acks]
            fl = sum(1 for g in sent if g["seq"] in fails)
            first = sum(1 for a in dl if a.get("tries") == 1)
            rtt = [a["rtt_ms"] for a in dl if "rtt_ms" in a]
            rssi, snr = [], []
            if rcv and i in win_r:
                t0, t1 = win_r[i][0], win_r[i][1]
                for r in rcv.ev("RX"):
                    if r.get("src") == snd.own and t0 <= r["t"] < t1 and "rssi" in r:
                        rssi.append(r["rssi"])
                        snr.append(r.get("snr", 0))
            sf, dbm = plan[i - 1]
            used = win_s.get(i, (0, 0, sf, dbm))[3]
            limit = DECODE_LIMIT_DB.get(sf, -12.5)
            rows.append({
                "session": sid, "label": label, "step": i, "sf": sf, "dbm": used if used is not None else dbm,
                "from_role": snd.role, "from": snd.own, "to": snd.peer,
                "sent": len(sent), "delivered": len(dl), "failed": fl, "skipped": len(skipped),
                "rate_pct": "" if not (len(dl) + fl) else round(100 * len(dl) / (len(dl) + fl), 1),
                "first_try_pct": "" if not (len(dl) + fl) else round(100 * first / (len(dl) + fl), 1),
                "rtt_median_ms": q(rtt, .5), "packets_heard": len(rssi),
                "rssi_median": med(rssi), "rssi_min": min(rssi) if rssi else "",
                "snr_median": med(snr), "snr_min": min(snr) if snr else "",
                "margin_db": round(statistics.median(snr) - limit, 1) if snr else "",
                "distance_m": dist, "distance_from": dist_src,
                "skew_ms": (win_r[i][0] - win_s[i][0]) if (i in win_r and i in win_s) else "",
                "source": "both phones" if rcv else "sender's phone only",
            })
    return rows


def heard_only_rows(sid, rcv, snd_role, label, dist, dist_src):
    """points.csv rows for a direction whose sender's phone has no file (its
    Bluetooth dropped): from the receiver's RX lines alone, per the
    receiver's own steps: how many different messages arrived (sent, failed
    and first try aren't known), and the RSSI / SNR of everything heard from
    the sender."""
    rows = []
    plan = plan_of(rcv)
    win = step_windows(rcv)
    for i in range(1, len(plan) + 1):
        rssi, snr, msgs = [], [], set()
        if i in win:
            t0, t1 = win[i][0], win[i][1]
            for r in rcv.ev("RX"):
                if r.get("src") == rcv.peer and t0 <= r["t"] < t1 and "rssi" in r:
                    rssi.append(r["rssi"])
                    snr.append(r.get("snr", 0))
                    if r.get("type") == "MSG" and not r.get("dup"):
                        msgs.add(r.get("id"))
        sf, dbm = plan[i - 1]
        limit = DECODE_LIMIT_DB.get(sf, -12.5)
        rows.append({
            "session": sid, "label": label, "step": i, "sf": sf, "dbm": dbm,
            "from_role": snd_role, "from": rcv.peer, "to": rcv.own,
            "sent": "", "delivered": len(msgs), "failed": "", "skipped": "",
            "rate_pct": "", "first_try_pct": "", "rtt_median_ms": "", "packets_heard": len(rssi),
            "rssi_median": med(rssi), "rssi_min": min(rssi) if rssi else "",
            "snr_median": med(snr), "snr_min": min(snr) if snr else "",
            "margin_db": round(statistics.median(snr) - limit, 1) if snr else "",
            "distance_m": dist, "distance_from": dist_src, "skew_ms": "",
            "source": "receiver's phone only",
        })
    return rows


def pair_key(label):
    """A label's group: its words before the first comma ("pillar a")."""
    return label.split(",")[0].strip().lower()


def is_reference(label):
    """Is this the clear reference of its group ("clear" or "ref" in it)?"""
    return re.search(r"\b(clear|ref|reference)\b", label, re.I) is not None


def is_ladder(label):
    """Is this a spot of the distance ladder ("open" or "ladder" in it)?"""
    return re.search(r"\b(open|ladder)\b", label, re.I) is not None


def path_loss_fit(pts):
    """Least squares of RSSI on log10(distance): (n, A, R^2) with
    RSSI = A - 10 n log10(d); pts = [(log10 d, rssi), ...]."""
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    a = my - b * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    r2 = 1 - sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys)) / ss_tot if ss_tot else 1.0
    return -b / 10, a, r2


def point_report(prows, w):
    """summary.txt: every point test step by step; the obstacles' losses
    against their references (and the check that the power doesn't change
    them); the distance ladder's path-loss exponent."""
    if not prows:
        return
    w("=" * 72)
    w("Point tests (v0.6.1), per step and direction. rate = delivered / (delivered + failed); RSSI / SNR as")
    w("the receiver heard the sender in that step (all its packets); margin = median SNR - the SF's limit")
    w(f"    {'label':26}{'step':>5} {'setting':12}{'dir':12}{'sent':>5}{'deliv':>6}{'rate':>8}{'1st':>8}"
      f"{'RSSI med/min':>14}{'SNR med/min':>13}{'margin':>8}{'n':>5}")
    for r in sorted(prows, key=lambda r: (r["session"], r["step"], r["from_role"])):
        d = f"{r['from_role']}->{other(r['from_role'])}"
        rs = f"{r['rssi_median']}/{r['rssi_min']}" if r["rssi_median"] != "" else "-"
        sn = f"{r['snr_median']}/{r['snr_min']}" if r["snr_median"] != "" else "-"
        rate = "-" if r["rate_pct"] == "" else f"{r['rate_pct']} %"
        first = "-" if r["first_try_pct"] == "" else f"{r['first_try_pct']} %"
        setting = f"SF{r['sf']}/{r['dbm']}"
        w(f"    {r['label'][:25]:26}{r['step']:>5} {setting:12}{d:12}{r['sent']:>5}{r['delivered']:>6}{rate:>8}"
          f"{first:>8}{rs:>14}{sn:>13}{str(r['margin_db']):>8}{r['packets_heard']:>5}")
    skews = [abs(r["skew_ms"]) for r in prows if r["skew_ms"] != ""]
    if skews:
        w(f"    (the two boards switched within {max(skews) / 1000:.1f} s of each other, by the phones' clocks; "
          f"each step's settle time is 10-20 s)")

    # Obstacles against their references: same group, setting and direction.
    groups = defaultdict(lambda: {"ref": [], "obs": []})
    for r in prows:
        if r["rssi_median"] == "":
            continue
        g = groups[pair_key(r["label"])]
        (g["ref"] if is_reference(r["label"]) else g["obs"]).append(r)
    lines = []
    for key, g in sorted(groups.items()):
        for o in sorted(g["obs"], key=lambda r: (r["label"], r["step"], r["from_role"])):
            ref = [x for x in g["ref"] if (x["sf"], x["dbm"], x["from_role"]) == (o["sf"], o["dbm"], o["from_role"])]
            if ref:
                rr = ref[-1]
                lines.append((o["label"], o["sf"], o["dbm"], o["from_role"], rr["rssi_median"], o["rssi_median"],
                              round(rr["rssi_median"] - o["rssi_median"], 1)))
    if lines:
        w("")
        w("  Attenuation (loss = the clear reference's RSSI - the obstacle's; same group, setting and direction):")
        w(f"    {'obstacle':30}{'setting':12}{'dir':12}{'ref':>8}{'obst.':>8}{'loss dB':>9}")
        for lab, sf, dbm, frm, ref_rssi, obs_rssi, loss in lines:
            setting = f"SF{sf}/{dbm}"
            w(f"    {lab[:29]:30}{setting:12}{frm + '->' + other(frm):12}{ref_rssi:>8}{obs_rssi:>8}{loss:>9}")
        by = defaultdict(dict)  # the check: at one SF, the loss shouldn't depend on the power
        for lab, sf, dbm, frm, _, _, loss in lines:
            by[(lab, sf, frm)][dbm] = loss
        checks = sorted((k, v) for k, v in by.items() if len(v) >= 2)
        if checks:
            w("    check (the loss should come out the same at every power):")
            for (lab, sf, frm), v in checks:
                vals = ", ".join(f"{d} dBm {l}" for d, l in sorted(v.items()))
                w(f"      {lab[:29]:30} SF{sf} {frm}->{other(frm)}: {vals}  "
                  f"(spread {max(v.values()) - min(v.values()):.1f} dB)")

    # The distance ladder: RSSI against log10(distance), per setting and direction.
    fits = defaultdict(list)
    for r in prows:
        if is_ladder(r["label"]) and r["distance_m"] != "" and r["rssi_median"] != "" and float(r["distance_m"]) > 0:
            fits[(r["sf"], r["dbm"], r["from_role"])].append((math.log10(float(r["distance_m"])), r["rssi_median"],
                                                             float(r["distance_m"])))
    shown = False
    for (sf, dbm, frm), pts in sorted(fits.items()):
        if len({p[0] for p in pts}) < 2:
            continue  # one distance: no slope
        if not shown:
            w("")
            w("  Distance ladder (RSSI = A - 10 n log10(d); n, the path-loss exponent: 2 in free space, about 2.7-3.5 "
              "in town):")
            shown = True
        n, a, r2 = path_loss_fit([(p[0], p[1]) for p in pts])
        at = ", ".join(f"{p[2]:g} m {p[1]}" for p in sorted(pts, key=lambda p: p[2]))
        w(f"    SF{sf}/{dbm} dBm {frm}->{other(frm)}: n = {n:.2f}, A (1 m) = {a:.0f} dBm, R^2 = {r2:.2f}  ({at})")
    w("")


# The columns of the CSV files (fixed, so a file is written, with its
# header, even when a run has no rows for it: no stale results are left).
MESSAGE_FIELDS = ["session", "test", "sf", "tx_dbm", "from", "from_role", "to", "seq", "size", "step", "path",
                  "outcome", "why", "rtt_ms", "tries", "distance_m", "rssi", "snr", "area", "situation",
                  "phone_time_ms"]
POINT_FIELDS = ["session", "label", "step", "sf", "dbm", "from_role", "from", "to", "sent", "delivered", "failed",
                "skipped", "rate_pct", "first_try_pct", "rtt_median_ms", "packets_heard", "rssi_median", "rssi_min",
                "snr_median", "snr_min", "margin_db", "distance_m", "distance_from", "skew_ms", "source"]
SESSION_FIELDS = ["session", "test", "sf", "tx_dbm", "from_role", "path", "made", "delivered", "failed", "skipped",
                  "rate_pct", "first_try_pct", "rtt_median_ms", "rtt_p95_ms"]
LOST_FIELDS = ["session", "board", "role", "sf", "tx_dbm", "start_min", "end_min", "minutes", "why", "rebuilt",
               "open_at_end"]
RUNDOWN_FIELDS = ["board", "session", "test", "paths", "role", "fw", "sf", "tx_dbm", "minute", "mv", "charging",
                  "m1_ms", "m2_ms", "f_on_a"]


# ---------------------------------------------------------------- run-downs


# Terminal escape codes and other control characters the serial monitor
# leaves in a saved log (tabs kept).
ESC = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|[\x00-\x08\x0b-\x1f]")
ROW = re.compile(r"\s*(\d+),(\d+),([01]),(\d+),(\d+)\s*")


def find_dumps(paths):
    """Files (not session files) that hold a `test dump`: anything with a
    "# morselink-rundown" line in its first 4 MB."""
    out = []
    for p in paths:
        names = []
        if os.path.isdir(p):
            for root, _, ns in os.walk(p):
                names += [os.path.join(root, n) for n in ns]
        else:
            names.append(p)
        for f in names:
            if f.endswith(".jsonl"):
                continue
            try:
                with open(f, "rb") as fh:
                    if b"# morselink-rundown" in fh.read(4 << 20):
                        out.append(f)
            except OSError:
                pass
    return out


def read_dumps(path):
    """Every run-down in one saved monitor log: [{meta, rows, file}], rows
    as (minute, mv, charging, m1_ms, m2_ms). The header line can sit after
    the monitor's own text and escape codes; a file may hold several dumps."""
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = [ESC.sub("", l.replace("\r", "")) for l in f.read().split("\n")]
    out = []
    i = 0
    while i < len(lines):
        k = lines[i].find("# morselink-rundown")
        if k < 0:
            i += 1
            continue
        meta = dict(kv.split("=", 1) for kv in lines[i][k:].split()[2:] if "=" in kv)
        i += 1
        while i < len(lines) and not lines[i].strip().startswith("minute,") and "# morselink-rundown" not in lines[i]:
            i += 1  # (the comment line saying how it ended)
        rows = []
        if i < len(lines) and lines[i].strip().startswith("minute,"):
            i += 1
            while i < len(lines):
                m = ROW.fullmatch(lines[i])
                if not m:
                    break
                rows.append(tuple(int(x) for x in m.groups()))
                i += 1
        if "board" in meta and rows:  # (not a mere mention of the header, e.g. in a README)
            out.append({"meta": meta, "rows": rows, "file": path})
    return out


def dedupe_dumps(dumps):
    """The same run dumped twice (or saved twice): keep the longest copy."""
    best = {}
    for d in dumps:
        m = d["meta"]
        key = (m.get("board"), m.get("session"), m.get("test"), d["rows"][0][1] if d["rows"] else None)
        if key not in best or len(d["rows"]) > len(best[key]["rows"]):
            best[key] = d
    return sorted(best.values(), key=lambda d: (d["meta"].get("board", ""), d["meta"].get("session", "")))


def linear_slope(points):
    """Least-squares slope of (x, y) points; None for fewer than 2 x values."""
    if len({x for x, _ in points}) < 2:
        return None
    n = len(points)
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    return sum((x - mx) * (y - my) for x, y in points) / sxx


def running_median(values, half=7):
    """A centred running median (15 samples: 15 minutes) of a list."""
    return [statistics.median(values[max(0, i - half):i + half + 1]) for i in range(len(values))]


def a_curve(d):
    """A run-down A that ran to empty (TEST_PLAN 14.2) as its board's
    reference: (T_A minutes, [(minute, smoothed mV)], falling only); None if
    this dump isn't one (another test, stopped by hand, or not near empty)."""
    m = d["meta"]
    rows = [r for r in d["rows"] if r[2] == 0]
    if m.get("test") != "rundown-a" or m.get("end") != "none" or len(rows) < 120 or rows[-1][1] > 3650:
        return None
    sm = running_median([r[1] for r in rows])
    low = []
    for v in sm:  # (falling only: a small rise must not map back to an earlier point)
        low.append(min(v, low[-1]) if low else v)
    return rows[-1][0], [(r[0], v) for r, v in zip(rows, low)]


def fraction_used(curve, mv):
    """Where `mv` sits on an A curve: the fraction of the battery used (0-1)."""
    t_a, pts = curve
    for minute, v in pts:
        if v <= mv:
            return minute / t_a
    return 1.0


def rundown_report(dumps, w):
    """The run-down section of summary.txt; returns rundowns.csv's rows."""
    out_rows = []
    if not dumps:
        return out_rows
    curves = {}
    for d in dumps:
        c = a_curve(d)
        b = d["meta"].get("board", "?")
        if c and (b not in curves or c[0] > curves[b][0]):
            curves[b] = c
    w("=" * 72)
    w("Battery run-downs (test dump; samples with charging = 0)")
    for d in dumps:
        m = d["meta"]
        b = m.get("board", "?")
        rows = [r for r in d["rows"] if r[2] == 0]
        curve = curves.get(b)
        f_of = {}
        if curve and rows:
            sm = running_median([r[1] for r in rows])
            f_of = {r[0]: fraction_used(curve, v) for r, v in zip(rows, sm)}
        for r in d["rows"]:
            out_rows.append({"board": b, "session": m.get("session", ""), "test": m.get("test", ""),
                             "paths": m.get("paths", ""), "role": m.get("role", ""), "fw": m.get("fw", ""),
                             "sf": m.get("sf", "9"), "tx_dbm": m.get("tx_dbm", ""), "minute": r[0], "mv": r[1],
                             "charging": r[2], "m1_ms": r[3], "m2_ms": r[4],
                             "f_on_a": round(f_of[r[0]], 4) if r[0] in f_of else ""})
        w("")
        w(f"  {b} {m.get('test', '?')} (paths {m.get('paths', '?')}, {m.get('role', '?')}), session "
          f"{m.get('session', '?')}, fw {m.get('fw', '?')}, SF{m.get('sf', '9')}, {m.get('tx_dbm', '?')} dBm: "
          f"{len(d['rows'])} min, ended: {m.get('end', '?')}   [{os.path.basename(d['file'])}]")
        if not rows:
            w("    no samples without charging")
            continue
        slope = linear_slope([(r[0] / 60.0, r[1]) for r in rows if r[0] >= 30])
        w(f"    {rows[0][1]} -> {rows[-1][1]} mV over {(rows[-1][0] - rows[0][0]) / 60:.1f} h"
          + (f"; after the first 30 min {slope:+.1f} mV/h" if slope is not None else ""))
        hours = _group(rows, lambda r: r[0] // 60)
        step = max(1, math.ceil(len(hours) / 24))  # (at most about 24 values on the line)
        w(f"    {'hourly medians' if step == 1 else f'medians every {step} h'}: " + " | ".join(
            f"{h}h {statistics.median(x[1] for x in hours[h]):.0f}" for h in sorted(hours) if h % step == 0))
        on1, on2 = sum(r[3] for r in d["rows"]), sum(r[4] for r in d["rows"])
        if on1 or on2:
            span = max(1, len(d["rows"])) * 60000
            w(f"    motor on-time: M1 {on1 / 3.6e6:.2f} h ({100.0 * on1 / span:.0f} %), "
              f"M2 {on2 / 3.6e6:.2f} h ({100.0 * on2 / span:.0f} %)")
        if curve and f_of and d is not None:
            t_a = curve[0]
            pts = [(r[0] / 60.0, f_of[r[0]]) for r in rows if r[0] >= 30]
            fs = linear_slope(pts)
            first, last = f_of[rows[0][0]], f_of[rows[-1][0]]
            if a_curve(d) is not None:
                w(f"    this is {b}'s reference curve (run-down A to empty): {t_a / 60:.1f} h")
            elif fs is not None and fs > 0:
                r_ = fs * t_a / 60.0
                w(f"    on {b}'s run-down A curve ({t_a / 60:.1f} h to empty): {100 * first:.1f} -> {100 * last:.1f} % "
                  f"used, {100 * fs:.2f} %/h = {r_:.2f}x listening only; a full battery would last ~{t_a / 60 / r_:.0f} h")
    if not curves:
        w("")
        w("  (No run-down A to empty yet: once there is one per board, each run is placed on it, TEST_PLAN 14.2.)")
    w("")
    return out_rows


# ---------------------------------------------------------------- running


def double_clicked():
    """
    Windows: was this started by double-clicking it (Explorer is the parent,
    or the parent of the py launcher)? Its window then closes as soon as the
    script ends, so finish() waits for Enter first. Anything unexpected
    gives False (no waiting).
    """
    if os.name != "nt":
        return False
    try:
        import ctypes
        from ctypes import wintypes

        class Entry(ctypes.Structure):  # PROCESSENTRY32W
            _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                        ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                        ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                        ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                        ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
        k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
        k32.CloseHandle.argtypes = [wintypes.HANDLE]
        snap = k32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS: all processes
        if not snap or snap == wintypes.HANDLE(-1).value:
            return False
        procs = {}  # pid -> (parent pid, exe name)
        try:
            e = Entry()
            e.dwSize = ctypes.sizeof(Entry)
            ok = k32.Process32FirstW(snap, ctypes.byref(e))
            while ok:
                procs[e.th32ProcessID] = (e.th32ParentProcessID, e.szExeFile.lower())
                ok = k32.Process32NextW(snap, ctypes.byref(e))
        finally:
            k32.CloseHandle(snap)
        pid = os.getppid()
        for _ in range(2):  # our parent, then (for py.exe) its parent
            if pid not in procs:
                return False
            pid, exe = procs[pid]
            if exe == "explorer.exe":
                return True
        return False
    except Exception:
        return False


def finish(message, code=0):
    """Print the last words; after a double-click wait for Enter so they can be read."""
    print(message)
    if double_clicked():
        try:
            input("\nPress Enter to close.")
        except EOFError:
            pass
    return code


def write_csv(path, fields, rows):
    """One CSV file (header always)."""
    with open(path, "w", newline="", encoding="utf-8") as f:
        wr = csv.DictWriter(f, fieldnames=fields)
        wr.writeheader()
        wr.writerows(rows)


def main():
    try:
        sys.stdout.reconfigure(errors="replace")  # (an odd character in a note must not stop the printout)
    except Exception:
        pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="*", help=f"folders or session files (default: {DATA})")
    ap.add_argument("-o", "--out", default=RESULTS, help=f"output folder (default: {RESULTS})")
    a = ap.parse_args()
    inputs = a.inputs or [DATA]

    # First run: make data\ and say what goes in it.
    if not a.inputs and not os.path.isdir(DATA):
        os.makedirs(DATA)
        return finish(f"Made the folder {DATA}\n"
                      "Copy both phones' ML_... files into it, then run this again.")
    fixes = read_corrections(inputs)
    files = find_files(inputs)
    dumps = dedupe_dumps([d for f in find_dumps(inputs) for d in read_dumps(f)])
    if not files and not dumps:
        return finish(f"No session files (ML_<session>_<rover|base>_<ID>.jsonl) or run-down dumps in "
                      f"{', '.join(inputs)}\n"
                      "Copy both phones' files there (keep their names), then run this again.", 1)

    sessions = defaultdict(dict)  # session ID -> {"rover": Side, "base": Side}
    for f in files:
        s = Side(f)
        if s.role in sessions[s.session]:
            print(f"warning: two {s.role} files for {s.session}; using {f}")
        sessions[s.session][s.role] = s

    all_rows, srows, lrows, prows, text = [], [], [], [], []
    if fixes:
        text.append("Label corrections applied (corrections.txt):")
        text += fixes
        text.append("")
    for sid in sorted(sessions):
        sides = sessions[sid]
        rover, base = sides.get("rover"), sides.get("base")
        rows = []
        for snd, rcv in ((rover, base), (base, rover)):  # both directions
            if snd:
                rows += messages(snd, rcv, rover, base)
        summarise(sid, sides, rows, text)
        for r in session_rows(sid, rows):
            r["test"] = (rover or base).meta.get("test", "")
            srows.append(r)
        lrows += lost_rows(sid, sides)
        if is_point(sides):
            prows += point_rows(sid, sides)
        all_rows += rows
    setting_block([r for r in all_rows if r["test"] != "point"], text.append)  # (a sweep's steps: point_report)
    point_report(prows, text.append)
    drows = rundown_report(dumps, text.append)

    print("\n".join(text))
    os.makedirs(a.out, exist_ok=True)
    try:
        with open(os.path.join(a.out, "summary.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(text) + "\n")
        write_csv(os.path.join(a.out, "messages.csv"), MESSAGE_FIELDS, all_rows)
        write_csv(os.path.join(a.out, "sessions.csv"), SESSION_FIELDS, srows)
        write_csv(os.path.join(a.out, "lost.csv"), LOST_FIELDS, lrows)
        write_csv(os.path.join(a.out, "rundowns.csv"), RUNDOWN_FIELDS, drows)
        write_csv(os.path.join(a.out, "points.csv"), POINT_FIELDS, prows)
    except OSError as e:  # on Windows: a results file is open in Excel
        return finish(f"Can't write {e.filename}: is it open in Excel or another program?\n"
                      "Close it and run this again.", 1)
    return finish(f"{len(sessions)} session(s), {len(all_rows)} message copies, {len(prows)} point-test rows, "
                  f"{len(dumps)} run-down dump(s).\n"
                  f"Results in {os.path.abspath(a.out)}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:  # show what went wrong (and keep a double-clicked window open)
        traceback.print_exc()
        sys.exit(finish("\nSomething went wrong (see the lines above).", 2))
