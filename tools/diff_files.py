#!/usr/bin/env python3
"""Find where two runs' file activity during play first differs.

    python harness\\tools\\diff_files.py <runA> <runB> [--runs DIR]

Uses files.csv (every open, plus reads made outside loads, with the frame and
thread). Threads are compared by name (or main/other), since thread ids change
every launch. For each stretch between loads it prints the first file event
that differs, then lists files first touched during play on a different frame.
"""
import argparse
import csv
import pathlib
import re
import sys

RUNS = pathlib.Path(__file__).resolve().parent.parent / "runs"   # harness\runs (this file is in harness\tools)


def load(run):
    p = RUNS / run / "files.csv"
    if not p.exists():
        sys.exit(f"{p} not found: was the run made with io_log=1 (the default)?")
    names, events = {}, []
    with open(p, newline="", encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            if r["op"] == "thread":
                if r["file"] != "(unnamed)":
                    names[r["thread"]] = r["file"]
                continue
            if r["op"] not in ("open", "read") or r["state"] != "0":
                continue
            who = names.get(r["thread"]) or ("main" if r["main"] == "1" else "other thread")
            events.append((int(r["logical"]), r["op"], int(r["bytes"]), r["file"], who))
    return events


def load_ends(run):
    text = (RUNS / run / "harness.log").read_text(errors="replace")
    return [int(m) for m in re.findall(r"LOAD #\d+ end at poll \d+ \(logical (\d+)\)", text)]


def show(e):
    f, op, n, path, who = e
    size = f" {n // 1024} KB" if op == "read" else ""
    return f"{op}{size} {pathlib.PureWindowsPath(path).name} [{who}]"


def compare(name, a, b):
    n = min(len(a), len(b))
    i = next((k for k in range(n) if a[k] != b[k]), None)
    if i is None:
        print(f"  {name}: identical ({len(a)} events)" if len(a) == len(b)
              else f"  {name}: identical for {n} events, then one run has {abs(len(a) - len(b))} more")
        return
    print(f"  {name}: FIRST DIFFERENCE at event {i} (frame {a[i][0]} vs {b[i][0]})")
    for k in range(max(0, i - 3), min(i + 4, n)):
        mark = ">>" if k == i else "  "
        print(f"     {mark} A frame {a[k][0]:6d} {show(a[k]):52s}  B frame {b[k][0]:6d} {show(b[k])}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--runs", type=pathlib.Path)
    args = ap.parse_args()
    global RUNS
    if args.runs:
        RUNS = args.runs

    ea_, eb_ = load(args.a), load(args.b)
    la, lb = load_ends(args.a), load_ends(args.b)
    print(f"{args.a}: {len(ea_)} file events during play; {args.b}: {len(eb_)}")
    bounds = [0] + sorted(set(la) & set(lb)) + [2**32 - 1]
    print("\nStretches between loads:")
    for lo, hi in zip(bounds, bounds[1:]):
        sa = [e for e in ea_ if lo < e[0] <= hi or (lo == 0 and e[0] == 0)]
        sb = [e for e in eb_ if lo < e[0] <= hi or (lo == 0 and e[0] == 0)]
        compare(f"logical {lo + 1 if lo else 0}-{hi if hi < 2**32 - 1 else 'end'}", sa, sb)

    def first_touch(events):
        out = {}
        for f, op, n, path, who in events:
            out.setdefault(path, (f, who))
        return out
    fa, fb = first_touch(ea_), first_touch(eb_)
    moved = sorted((fa[p][0], p) for p in set(fa) & set(fb) if fa[p][0] != fb[p][0])
    print(f"\nFiles first touched during play on a different frame: {len(moved)}")
    for f, p in moved[:20]:
        print(f"  {pathlib.PureWindowsPath(p).name}: frame {fa[p][0]} [{fa[p][1]}] vs {fb[p][0]} [{fb[p][1]}]   ({p})")
    only = sorted(set(fa) ^ set(fb))
    if only:
        print(f"Files touched during play in only one run: {len(only)}")
        for p in only[:10]:
            print(f"  {pathlib.PureWindowsPath(p).name} ({'A' if p in fa else 'B'} only)")


if __name__ == "__main__":
    main()
