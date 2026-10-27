#!/usr/bin/env python3
r"""
MorseLink - analyse field-test logs (docs/TEST_PLAN.md section 4.6).

Reads the phones' session files (ML_<session>_<rover|base>_<ID>.jsonl),
pairs the rover's and the base's files of each session, and writes:

  results/summary.txt     per session: delivery rate and round trip per
                          path and direction, by message size, by area and
                          situation, signal strength, battery, skips
  results/messages.csv    one row per test message copy (open in Excel):
                          when, who sent it, path, size, delivered or not,
                          round trip, distance, RSSI/SNR, area, situation
  results/sessions.csv    one row per session and direction (for tables
                          in the report)

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
        self.gps = [(r["t"], r["lat"], r["lon"], r.get("acc", 99)) for r in self.recs
                    if r.get("kind") == "gps" and "lat" in r]
        self.gps.sort()
        self.labels = sorted((r["t"], r.get("area", ""), r.get("sit", ""), r.get("note", ""))
                             for r in self.recs if r.get("kind") == "label")
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
                "to": sender.peer, "seq": g["seq"], "size": g["size"],
                "path": "LoRa" if path == "L" else "relay", "outcome": outcome, "why": why,
                "rtt_ms": a.get("rtt_ms", ""), "tries": a.get("tries", ""),
                "distance_m": dist, "rssi": h.get("rssi", ""), "snr": h.get("snr", ""),
                "area": area, "situation": sit, "phone_time_ms": t,
            })
    return rows


def summarise(sid, sides, rows, out):
    rover, base = sides.get("rover"), sides.get("base")
    any_side = rover or base
    m = any_side.meta
    w = out.append
    w("=" * 72)
    w(f"Session {sid}: {m.get('test')} test, paths {m.get('paths')}, tx {m.get('tx_dbm')} dBm, "
      f"fw {m.get('fw')}")
    w("  files: " + ", ".join(f"{k} {os.path.basename(s.path)}" for k, s in sides.items()))
    if not (rover and base):
        w("  NOTE: only one phone's file - receive-side numbers (RSSI) are missing.")
    for s in sides.values():
        st = s.ev("STOP")
        if st:
            w(f"  {s.role} board {s.own}: stopped ({st[-1].get('reason')}) after "
              f"{st[-1].get('gen')} messages")
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
        w(f"    {'':22}{'made':>6}{'deliv.':>8}{'failed':>8}{'skip':>6}{'rate':>9}"
          f"{'rtt med':>9}{'rtt p95':>9}")
        for key, grp in sel:
            n = len(grp)
            d = [r for r in grp if r["outcome"] == "delivered"]
            f = sum(r["outcome"] == "failed" for r in grp)
            sk = sum(r["outcome"] == "skipped" for r in grp)
            rtt = [r["rtt_ms"] for r in d if r["rtt_ms"] != ""]
            w(f"    {key:22}{n:>6}{len(d):>8}{f:>8}{sk:>6}{pct(len(d), len(d) + f):>9}"
              f"{q(rtt, .5):>9}{q(rtt, .95):>9}")

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
    block("By direction and path (rate = delivered / (delivered + failed); skips excluded)",
          groups(lambda r: f"{r['from_role']}->{'base' if r['from_role'] == 'rover' else 'rover'} {r['path']}",
                 rows))
    block("By size (both directions)", groups(lambda r: f"{r['path']} {r['size']:>3} chars", rows))
    if any(r["area"] or r["situation"] for r in rows):
        block("By area / situation (rover's labels)",
              groups(lambda r: f"{r['path']} {r['area'] or '?'}/{r['situation'] or '?'}"[:22], rows))
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
    notes = [l for s in sides.values() for l in s.labels if l[3]]
    if notes:
        w("")
        w("  Notes:")
        for t, _, _, n in notes:
            w(f"    {(t - any_side.recs[0]['t']) / 60000:6.1f} min  {n}")
    w("")


def session_rows(sid, rows):
    out = []
    for (frm, path), grp in sorted(_group(rows, lambda r: (r["from_role"], r["path"])).items()):
        d = [r for r in grp if r["outcome"] == "delivered"]
        f = sum(r["outcome"] == "failed" for r in grp)
        rtt = [r["rtt_ms"] for r in d if r["rtt_ms"] != ""]
        out.append({"session": sid, "test": "", "from_role": frm, "path": path, "made": len(grp),
                    "delivered": len(d), "failed": f,
                    "skipped": sum(r["outcome"] == "skipped" for r in grp),
                    "rate_pct": "" if not (len(d) + f) else round(100 * len(d) / (len(d) + f), 1),
                    "rtt_median_ms": q(rtt, .5), "rtt_p95_ms": q(rtt, .95)})
    return out


def _group(rows, keyf):
    g = defaultdict(list)
    for r in rows:
        g[keyf(r)].append(r)
    return g


# The columns of the two CSV files (fixed, so a file is written, with its
# header, even when a run has no rows for it: no stale results are left).
MESSAGE_FIELDS = ["session", "from", "from_role", "to", "seq", "size", "path", "outcome", "why",
                  "rtt_ms", "tries", "distance_m", "rssi", "snr", "area", "situation", "phone_time_ms"]
SESSION_FIELDS = ["session", "test", "from_role", "path", "made", "delivered", "failed", "skipped",
                  "rate_pct", "rtt_median_ms", "rtt_p95_ms"]


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
    files = find_files(inputs)
    if not files:
        return finish(f"No session files (ML_<session>_<rover|base>_<ID>.jsonl) in {', '.join(inputs)}\n"
                      "Copy both phones' files there (keep their names), then run this again.", 1)

    sessions = defaultdict(dict)  # session ID -> {"rover": Side, "base": Side}
    for f in files:
        s = Side(f)
        if s.role in sessions[s.session]:
            print(f"warning: two {s.role} files for {s.session}; using {f}")
        sessions[s.session][s.role] = s

    all_rows, srows, text = [], [], []
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
        all_rows += rows

    print("\n".join(text))
    os.makedirs(a.out, exist_ok=True)
    try:
        with open(os.path.join(a.out, "summary.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(text) + "\n")
        write_csv(os.path.join(a.out, "messages.csv"), MESSAGE_FIELDS, all_rows)
        write_csv(os.path.join(a.out, "sessions.csv"), SESSION_FIELDS, srows)
    except OSError as e:  # on Windows: a results file is open in Excel
        return finish(f"Can't write {e.filename}: is it open in Excel or another program?\n"
                      "Close it and run this again.", 1)
    return finish(f"{len(sessions)} session(s), {len(all_rows)} message copies.\n"
                  f"Results in {os.path.abspath(a.out)}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:  # show what went wrong (and keep a double-clicked window open)
        traceback.print_exc()
        sys.exit(finish("\nSomething went wrong (see the lines above).", 2))
