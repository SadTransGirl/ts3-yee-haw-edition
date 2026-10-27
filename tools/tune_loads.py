#!/usr/bin/env python3
"""Check and tune the loading detector against a run's io.csv.

    python harness\\tune_loads.py <run> [--enter-kb N] [--window N] [--quiet-kb N]
                                        [--quiet-frames N] [--hitch-ms N]
                                        [--sources exe,crt,bink,fmod]

Prints the loads the harness actually detected during the run (the "state"
column), then re-runs the same detector offline with the given settings so you
can try thresholds without playing again. Defaults match xinput_harness.ini.

What good looks like: one detected load per loading screen you saw, and none
during normal play. If gameplay streaming triggers loads, raise --enter-kb or
drop a source. If a loading screen is missed, lower --enter-kb or add a source.
"""
import argparse
import csv
import pathlib
import sys

RUNS = pathlib.Path(__file__).resolve().parent.parent / "runs"   # harness\runs (this file is in harness\tools)
SOURCES = ["exe", "crt", "bink", "fmod"]


def load_rows(run):
    path = RUNS / run / "io.csv"
    if not path.exists():
        sys.exit(f"{path} not found. It is written by record/replay runs with io_log=1.")
    with open(path, newline="") as f:
        return [{k: float(v) for k, v in row.items()} for row in csv.DictReader(f)]


def intervals(flags):
    """[(start_index, end_index_exclusive)] of runs of True."""
    out, start = [], None
    for i, on in enumerate(flags):
        if on and start is None:
            start = i
        elif not on and start is not None:
            out.append((start, i))
            start = None
    if start is not None:
        out.append((start, len(flags)))
    return out


def simulate(rows, sources, window, enter_kb, quiet_kb, quiet_frames, hitch_ms):
    loading, quiet, ring, flags = False, 0, [], []
    for r in rows:
        kb = sum(r[f"{s}_kb"] for s in sources)
        ring.append(kb)
        hitch = hitch_ms > 0 and r["ms"] >= hitch_ms
        if not loading:
            if sum(ring[-window:]) >= enter_kb or hitch:
                loading, quiet = True, 0
        else:
            quiet = quiet + 1 if (kb < quiet_kb and not hitch) else 0
            if quiet >= quiet_frames:
                loading = False
        flags.append(loading)
    return flags


def report(title, rows, flags, sources):
    t = [0.0]
    for r in rows:
        t.append(t[-1] + r["ms"] / 1000.0)
    spans = intervals(flags)
    print(f"\n{title}: {len(spans)} load(s)")
    if spans:
        print(f"  {'#':>3} {'polls':>13} {'time (s)':>15} {'dur s':>6} {'MB':>7}")
    for i, (a, b) in enumerate(spans, 1):
        mb = sum(sum(rows[j][f"{s}_kb"] for s in sources) for j in range(a, b)) / 1024
        print(f"  {i:>3} {int(rows[a]['poll']):>6}-{int(rows[b - 1]['poll']):<6} "
              f"{t[a]:>7.1f}-{t[b]:<7.1f} {t[b] - t[a]:>6.1f} {mb:>7.1f}")


def percentiles(values):
    v = sorted(values)
    if not v:
        return "n/a"
    pick = lambda p: v[min(len(v) - 1, int(p / 100 * len(v)))]
    return " ".join(f"p{p}={pick(p):.0f}" for p in (50, 90, 99)) + f" max={v[-1]:.0f}"


def main():
    global RUNS
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--runs", type=pathlib.Path, default=RUNS)
    ap.add_argument("--enter-kb", type=float, default=2048)
    ap.add_argument("--window", type=int, default=8)
    ap.add_argument("--quiet-kb", type=float, default=16)
    ap.add_argument("--quiet-frames", type=int, default=30)
    ap.add_argument("--hitch-ms", type=float, default=0)
    ap.add_argument("--sources", default="exe,crt")
    a = ap.parse_args()
    RUNS = a.runs
    sources = [s for s in a.sources.split(",") if s]
    if any(s not in SOURCES for s in sources):
        sys.exit(f"sources must be from {SOURCES}")

    rows = load_rows(a.run)
    total_s = sum(r["ms"] for r in rows) / 1000
    print(f"{a.run}: {len(rows)} polls, {total_s:.1f} s")
    print("KB read per poll, by source (all polls):")
    for s in SOURCES:
        vals = [r[f"{s}_kb"] for r in rows]
        print(f"  {s:>5}: total {sum(vals) / 1024:8.1f} MB   {percentiles(vals)}")
    slow = [r for r in rows if r["ms"] >= 100]
    print(f"polls taking >= 100 ms: {len(slow)}" +
          (f" (longest {max(r['ms'] for r in slow):.0f} ms at poll {int(max(slow, key=lambda r: r['ms'])['poll'])})" if slow else ""))

    report("Detected during the run (state column)", rows, [r["state"] == 1 for r in rows], sources)
    if any(r["state"] == 2 for r in rows):
        report("Replay holding at a recorded load", rows, [r["state"] == 2 for r in rows], sources)
    flags = simulate(rows, sources, max(1, a.window), a.enter_kb, a.quiet_kb, max(1, a.quiet_frames), a.hitch_ms)
    report(f"Offline re-run (sources={','.join(sources)} enter={a.enter_kb:g}KB/{a.window}f "
           f"quiet<{a.quiet_kb:g}KB x{a.quiet_frames} hitch={a.hitch_ms:g}ms)", rows, flags, sources)


if __name__ == "__main__":
    main()
