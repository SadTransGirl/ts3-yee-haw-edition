#!/usr/bin/env python3
"""Find where two runs' engine-RNG draws first differ.

    python harness\\tools\\diff_mt.py <runA> <runB> [--runs DIR]

Uses mt_draws.bin (every main-thread draw: logical frame, caller, wrapper's
caller, generator) and mt_callers.csv from each run. Draws made during loads
are skipped, since load lengths legitimately differ. The draws are compared in
stretches between loads, because each load resets the scene: for every
stretch it reports the first draw that differs, with the callers on both
sides. Addresses are as in Ghidra (base 0x140000000).
"""
import argparse
import csv
import pathlib
import re
import sys

import numpy as np

RUNS = pathlib.Path(__file__).resolve().parent.parent / "runs"   # harness\runs (this file is in harness\tools)
DRAW = np.dtype([("logical", "<u4"), ("r0", "<u4"), ("r1", "<u4"), ("flags", "<u4")])
BASE = 0x140000000


def addr(rva):
    return f"{BASE + int(rva):x}" if rva else "-"


def load_draws(run):
    p = RUNS / run / "mt_draws.bin"
    if not p.exists():
        sys.exit(f"{p} not found: was the run made with mt_trace=1?")
    d = np.fromfile(p, dtype=DRAW)
    return d[(d["flags"] & 2) == 0]          # drop draws made while loading or holding


def load_ends(run):
    text = (RUNS / run / "harness.log").read_text(errors="replace")
    return [int(m) for m in re.findall(r"LOAD #\d+ end at poll \d+ \(logical (\d+)\)", text)]


def load_callers(run):
    p = RUNS / run / "mt_callers.csv"
    out = {}
    if p.exists():
        with open(p, newline="") as f:
            for row in csv.DictReader(f):
                out[(row["caller"], row["via"], row["generator"])] = int(row["main"])
    return out


def describe(d, i):
    r = d[i]
    gen = "global" if r["flags"] & 1 else "private"
    via = f" via {addr(r['r1'])}" if r["r1"] else ""
    return f"mt@{addr(r['r0'])}{via} ({gen})"


def compare_segment(name, a, b):
    n = min(len(a), len(b))
    key = ["logical", "r0", "r1", "flags"]
    same = np.ones(n, dtype=bool)
    for k in key:
        same &= a[k][:n] == b[k][:n]
    bad = np.nonzero(~same)[0]
    if len(bad) == 0:
        if len(a) == len(b):
            print(f"  {name}: identical ({len(a)} draws)")
            return None
        i = n
        print(f"  {name}: identical for {n} draws, then one run has {abs(len(a) - len(b))} more")
        return None
    i = int(bad[0])
    fa, fb = int(a["logical"][i]), int(b["logical"][i])
    print(f"  {name}: FIRST DIFFERENCE at draw {i} of the stretch (logical frame {fa} vs {fb})")
    lo = max(0, i - 3)
    for j in range(lo, min(i + 4, n)):
        mark = ">>" if j == i else "  "
        print(f"     {mark} frame {int(a['logical'][j]):6d}  A: {describe(a, j):48s}"
              f"  frame {int(b['logical'][j]):6d}  B: {describe(b, j)}")
    for side, d, f in (("A", a, fa), ("B", b, fb)):
        print(f"     draws in frame {f}, run {side}: {int((d['logical'] == f).sum())}")
    return i


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--runs", type=pathlib.Path)
    args = ap.parse_args()
    global RUNS
    if args.runs:
        RUNS = args.runs

    da, db = load_draws(args.a), load_draws(args.b)
    ea, eb = load_ends(args.a), load_ends(args.b)
    print(f"{args.a}: {len(da)} draws outside loads, loads ending at logical {ea}")
    print(f"{args.b}: {len(db)} draws outside loads, loads ending at logical {eb}")
    if ea != eb:
        print("NOTE: the runs' loads ended at different logical frames; stretches may not line up")

    bounds = [0] + sorted(set(ea) & set(eb)) + [2**32 - 1]
    print("\nStretches between loads (each load resets the scene):")
    for lo, hi in zip(bounds, bounds[1:]):
        sa = da[(da["logical"] > lo) & (da["logical"] <= hi)] if lo else da[da["logical"] <= hi]
        sb = db[(db["logical"] > lo) & (db["logical"] <= hi)] if lo else db[db["logical"] <= hi]
        name = f"logical {lo + 1 if lo else 0}-{hi if hi < 2**32 - 1 else 'end'}"
        compare_segment(name, sa, sb)

    ca, cb = load_callers(args.a), load_callers(args.b)
    diffs = sorted(((abs(ca.get(k, 0) - cb.get(k, 0)), k) for k in set(ca) | set(cb)
                    if ca.get(k, 0) != cb.get(k, 0)), reverse=True)
    print(f"\nCallers whose main-thread draw totals differ: {len(diffs)} of {len(set(ca) | set(cb))}")
    for d, (c, via, gen) in diffs[:20]:
        v = f" via {via}" if via != "0" else ""
        print(f"  mt@{c}{v} ({gen}): {ca.get((c, via, gen), 0)} vs {cb.get((c, via, gen), 0)}")


if __name__ == "__main__":
    main()
