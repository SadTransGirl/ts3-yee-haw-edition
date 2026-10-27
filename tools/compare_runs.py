#!/usr/bin/env python3
"""Compare two harness runs, or inspect one.

    python compare_runs.py <runA> <runB> [--threshold PCT]
    python compare_runs.py inspect <run>

Run names are folders under harness\\runs\\. A comparison walks the frames both
runs captured, reports whether the pixel hash matched, and for mismatches how
many pixels differ. It writes a diff image for the first frame that exceeds
the threshold (default 1% of pixels) into the second run's folder.
"""
import csv
import pathlib
import re
import struct
import sys

import numpy as np
from PIL import Image, ImageChops

RUNS = pathlib.Path(__file__).resolve().parent.parent / "runs"   # harness\runs (this file is in harness\tools)

HEADER = struct.Struct("<4sIIId8s")
REC = struct.Struct("<IIBBHIIHBBhhhh")  # seq frame kind user flags result | pkt buttons lt rt lx ly rx ry
KIND_LOAD = 0xF0
REC_LOADING = 1
BUTTONS = {
    0x0001: "Up", 0x0002: "Down", 0x0004: "Left", 0x0008: "Right", 0x0010: "Start", 0x0020: "Back",
    0x0040: "LS", 0x0080: "RS", 0x0100: "LB", 0x0200: "RB", 0x0400: "Guide", 0x1000: "A", 0x2000: "B",
    0x4000: "X", 0x8000: "Y",
}


def load_frames(run):
    path = RUNS / run / "frames.csv"
    if not path.exists():
        sys.exit(f"{path} not found (did the run take screenshots?)")
    out = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            out[int(row["frame"])] = (row["hash"], int(row["width"]), int(row["height"]))
    return out


def load_log(run):
    path = RUNS / run / "harness.log"
    return path.read_text(errors="replace").splitlines() if path.exists() else []


def pixel_diff(a_path, b_path):
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        # Window client size can differ by a pixel or two between runs
        # (border/DPI rounding). Crop to the common area; refuse anything bigger.
        if abs(a.width - b.width) > 4 or abs(a.height - b.height) > 4:
            return None, None, None
        w, h = min(a.width, b.width), min(a.height, b.height)
        a, b = a.crop((0, 0, w, h)), b.crop((0, 0, w, h))
    na = np.asarray(a, dtype=np.int16)
    nb = np.asarray(b, dtype=np.int16)
    d = np.abs(na - nb)
    changed = (d.max(axis=2) > 16).mean() * 100.0
    return changed, float(d.mean()), ImageChops.difference(a, b)


def compare(run_a, run_b, threshold):
    fa, fb = load_frames(run_a), load_frames(run_b)
    common = sorted(set(fa) & set(fb))
    only_a = sorted(set(fa) - set(fb))
    only_b = sorted(set(fb) - set(fa))

    print(f"\n{run_a}  vs  {run_b}")
    print(f"frames captured: {len(fa)} / {len(fb)}, common: {len(common)}")
    for run, lines in ((run_a, load_log(run_a)), (run_b, load_log(run_b))):
        for line in lines:
            if re.search(r"WARNING|MISMATCH|drifted|summary|NOTE", line):
                print(f"  [{run}] {line.replace('[xinput-harness] ', '')}")

    if not common:
        print("no common frames to compare")
        return 2

    print(f"\n{'frame':>8} {'hash':>6} {'pixels>16':>10} {'mean':>7}")
    first_div = None
    worst = 0.0
    for fr in common:
        ha, hb = fa[fr][0], fb[fr][0]
        if ha == hb:
            print(f"{fr:>8} {'same':>6} {0.0:>9.2f}% {0.0:>7.2f}")
            continue
        pa = RUNS / run_a / f"shot_{fr:06d}.bmp"
        pb = RUNS / run_b / f"shot_{fr:06d}.bmp"
        if not (pa.exists() and pb.exists()):
            print(f"{fr:>8} {'diff':>6} {'(bmp missing)':>10}")
            continue
        changed, mean, diff_img = pixel_diff(pa, pb)
        if changed is None:
            print(f"{fr:>8} {'diff':>6} {'(size differs)':>10}")
            continue
        flag = " <-- diverged" if changed > threshold else ""
        print(f"{fr:>8} {'diff':>6} {changed:>9.2f}% {mean:>7.2f}{flag}")
        worst = max(worst, changed)
        if changed > threshold and first_div is None:
            first_div = fr
            out = RUNS / run_b / f"diff_{fr:06d}.png"
            diff_img.point(lambda v: min(255, v * 4)).save(out)
            print(f"{'':>8} diff image written to {out}")

    if only_a:
        print(f"\nonly in {run_a}: frames {only_a[0]}..{only_a[-1]} ({len(only_a)})")
    if only_b:
        print(f"only in {run_b}: frames {only_b[0]}..{only_b[-1]} ({len(only_b)})")

    print()
    if first_div is None:
        print(f"VERDICT: no frame differed by more than {threshold}% of pixels (worst {worst:.2f}%). "
              "The runs stayed in sync over the captured range.")
        return 0
    print(f"VERDICT: runs diverged at frame {first_div}. Look at shot_{first_div:06d}.bmp in both run "
          "folders. A one-off blip that returns to ~0% is DWM/render jitter; a difference that persists "
          "or grows from here on is a real simulation desync.")
    return 1


def inspect(run):
    path = RUNS / run / "input.bin"
    data = path.read_bytes()
    magic, version, rec_size, flags, fps, _ = HEADER.unpack_from(data, 0)
    if magic != b"XIH1" or rec_size != REC.size:
        sys.exit(f"{path}: not a harness recording")
    n = (len(data) - HEADER.size) // REC.size
    print(path)
    print(f"version {version}, fixed_timestep={flags & 1}, freeze_wallclock={(flags >> 1) & 1}, "
          f"load_detect={(flags >> 2) & 1}, fps={fps}")
    users, kinds, held = {}, {}, {}
    last_frame = 0
    first_input = None
    calls = loading_calls = loading_input = 0
    loads = []  # logical frame at which each load finished
    for i in range(n):
        seq, frame, kind, user, rflags, result, pkt, buttons, lt, rt, lx, ly, rx, ry = REC.unpack_from(
            data, HEADER.size + i * REC.size)
        if kind == KIND_LOAD:
            loads.append(frame)
            continue
        calls += 1
        last_frame = frame
        users[user] = users.get(user, 0) + 1
        kinds[kind] = kinds.get(kind, 0) + 1
        active = result == 0 and (buttons or lt > 30 or rt > 30 or abs(lx) > 8000 or abs(ly) > 8000)
        if rflags & REC_LOADING:
            loading_calls += 1
            loading_input += bool(active)
            continue
        if active:
            if first_input is None:
                first_input = frame
            for bit, name in BUTTONS.items():
                if buttons & bit:
                    held[name] = held.get(name, 0) + 1
    print(f"{calls} calls, {last_frame} logical frames ({last_frame / fps:.1f} s of non-loading time at {fps:.0f} fps)")
    print("calls per pad:", ", ".join(f"pad{u}={c}" for u, c in sorted(users.items())))
    print("call kinds:   ", ", ".join(f"{'GetState' if k == 0 else 'GetStateEx'}={c}" for k, c in sorted(kinds.items())))
    print(f"loads: {len(loads)}" + (f", ending at logical frames {loads}" if loads else ""))
    print(f"polls during loads: {loading_calls} (not replayed), {loading_input} of them with input held")
    print("first non-neutral input at logical frame:", first_input)
    print("button hold counts:", ", ".join(f"{k}={v}" for k, v in sorted(held.items(), key=lambda kv: -kv[1])) or "none")
    for line in load_log(run):
        if re.search(r"first poll|window|WARNING|summary|LOAD|io hooks", line):
            print(" ", line.replace("[xinput-harness] ", ""))


def main(argv):
    global RUNS
    threshold = 1.0
    if "--runs" in argv:
        i = argv.index("--runs")
        RUNS = pathlib.Path(argv[i + 1])
        del argv[i:i + 2]
    if "--threshold" in argv:
        i = argv.index("--threshold")
        threshold = float(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) == 3 and argv[1] == "inspect":
        inspect(argv[2])
        return 0
    if len(argv) != 3:
        print(__doc__)
        return 2
    return compare(argv[1], argv[2], threshold)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
