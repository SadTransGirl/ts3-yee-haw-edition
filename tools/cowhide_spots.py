"""Generate the launcher's cowhide pattern (launcher/Cowhide.xaml).

Classic packed cowhide: seed points are scattered over the tile, each seed gets
the area nearer to it than to any other seed (a Voronoi cell), and every cell
is pulled in from its neighbours to leave a white gap, shrunk toward its middle
by a random amount, nudged out of shape a little, and rounded off. The tile wraps (seeds near one edge see the seeds near
the opposite edge), so the pattern repeats without seams.

    python tools/cowhide_spots.py

writes launcher/Cowhide.xaml next to this repo's launcher. The seed is fixed,
so the same pattern comes out every time; change SEED for a different one.
"""
import math
import os
import random

W, H = 360, 180          # the tile, in the brush's Viewbox units
SEED = 7
SPACING = (17.0, 30.0)   # each seed's personal space (radius): sets the spot sizes and how much they vary
GAP = (5.0, 10.0)        # white gap between two neighbouring cells, picked per pair
SHRINK = (0.74, 0.94)    # each spot then shrinks toward its middle by this much: rounder, uneven white
WOBBLE = 0.22            # how far each edge's middle is pushed in or out (fraction of the edge)
ROUNDING = 4             # corner-cutting passes
COLOUR = "#2E211A"       # dark brown, like her boots
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "launcher", "Cowhide.xaml")
OFFSETS = [(ox, oy) for ox in (-W, 0, W) for oy in (-H, 0, H)]


def wrapped_distance(p, q):
    dx = abs(p[0] - q[0]); dy = abs(p[1] - q[1])
    return math.hypot(min(dx, W - dx), min(dy, H - dy))


def scatter(rng):
    """Seeds on the wrapping tile, each with its own personal space (dart throwing)."""
    seeds, space = [], []
    for _ in range(20000):
        p, r = (rng.uniform(0, W), rng.uniform(0, H)), rng.uniform(*SPACING)
        if all(wrapped_distance(p, q) >= r + s for q, s in zip(seeds, space)):
            seeds.append(p)
            space.append(r)
    return seeds


def clip(poly, a, b, c):
    """The part of a convex polygon where a*x + b*y <= c."""
    out = []
    for i, p in enumerate(poly):
        q = poly[(i + 1) % len(poly)]
        fp, fq = a * p[0] + b * p[1] - c, a * q[0] + b * q[1] - c
        if fp <= 0:
            out.append(p)
        if (fp < 0) != (fq < 0) and fp != fq:
            t = fp / (fp - fq)
            out.append((p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1])))
    return out


def cell(seeds, i):
    """Seed i's Voronoi cell, pulled in by half the gap to each neighbour."""
    px, py = seeds[i]
    poly = [(px - W, py - H), (px + W, py - H), (px + W, py + H), (px - W, py + H)]
    for j, (qx, qy) in enumerate(seeds):
        for ox, oy in OFFSETS:
            if j == i and ox == 0 and oy == 0:
                continue
            dx, dy = qx + ox - px, qy + oy - py
            dist = math.hypot(dx, dy)
            if dist > 8 * SPACING[1]:
                continue
            gap = random.Random(min(i, j) * 7919 + max(i, j)).uniform(*GAP)   # same gap seen from both sides
            mx, my = px + dx / 2, py + dy / 2
            poly = clip(poly, dx, dy, dx * mx + dy * my - gap / 2 * dist)
            if not poly:
                return []
    return poly


def shrink(rng, poly):
    """Pull the spot toward its middle by a random amount."""
    s = rng.uniform(*SHRINK)
    cx, cy = sum(x for x, _ in poly) / len(poly), sum(y for _, y in poly) / len(poly)
    return [(cx + (x - cx) * s, cy + (y - cy) * s) for x, y in poly]


def wobble(rng, poly):
    """Push each edge's middle in or out a little, so spots aren't all neat polygons."""
    out = []
    for i, a in enumerate(poly):
        b = poly[(i + 1) % len(poly)]
        ex, ey = b[0] - a[0], b[1] - a[1]
        length = math.hypot(ex, ey)
        out.append(a)
        if length > 6:
            push = rng.uniform(-WOBBLE, WOBBLE) * length
            out.append(((a[0] + b[0]) / 2 + ey / length * push, (a[1] + b[1]) / 2 - ex / length * push))
    return out


def round_off(poly):
    """Chaikin corner cutting: each pass replaces every corner by two points on its edges."""
    for _ in range(ROUNDING):
        poly = [pt for i, a in enumerate(poly) for b in [poly[(i + 1) % len(poly)]]
                for pt in ((0.75 * a[0] + 0.25 * b[0], 0.75 * a[1] + 0.25 * b[1]),
                           (0.25 * a[0] + 0.75 * b[0], 0.25 * a[1] + 0.75 * b[1]))]
    return poly


def figure(poly):
    return "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in poly) + "Z"


def main():
    rng = random.Random(SEED)
    seeds = scatter(rng)
    figures = []
    for i in range(len(seeds)):
        poly = cell(seeds, i)
        if len(poly) < 3:
            continue
        poly = round_off(wobble(rng, shrink(rng, poly)))
        for ox, oy in OFFSETS:                     # the copies that show across the tile's edges
            moved = [(x + ox, y + oy) for x, y in poly]
            xs, ys = [x for x, _ in moved], [y for _, y in moved]
            if max(xs) > 0 and min(xs) < W and max(ys) > 0 and min(ys) < H:
                figures.append(figure(moved))
    with open(OUT, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(
            '<ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"\n'
            '                    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">\n'
            f'    <!-- Made by tools/cowhide_spots.py (seed {SEED}, {len(seeds)} spots per tile); regenerate rather than edit.\n'
            '         Dark spots only: lay the brush over white (a tile background would show seams). -->\n'
            '    <DrawingBrush x:Key="CowhideBrush" TileMode="Tile" Viewport="0,0,420,210" ViewportUnits="Absolute"\n'
            f'                  Viewbox="0,0,{W},{H}" ViewboxUnits="Absolute" Stretch="Fill">\n'
            '        <DrawingBrush.Drawing>\n'
            f'            <GeometryDrawing Brush="{COLOUR}"\n'
            f'                Geometry="{" ".join(figures)}" />\n'
            '        </DrawingBrush.Drawing>\n'
            '    </DrawingBrush>\n'
            '</ResourceDictionary>\n')
    print(f"{len(seeds)} spots per tile, {len(figures)} outlines -> {os.path.normpath(OUT)}")


if __name__ == "__main__":
    main()
