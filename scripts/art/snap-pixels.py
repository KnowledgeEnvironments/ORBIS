#!/usr/bin/env python3
"""Snap generated "pixel art" to real pixels.

An image model paints the look of pixel art: soft edges, hundreds of near-duplicate colours, and a
lattice that is not square and bends slowly across the frame. So one comb over the image fails, and
tracking each line alone warps the picture. This measures the lattice instead:

  1. the local phase of the grid, per axis, from where the colour edges fall;
  2. one smooth displacement field, from that phase, unwrapped from the surest cells outward;
  3. a rigid grid sampled through the field: the median of each cell's middle, which skips the blur;
  4. hard alpha, a small palette, no stray specks.

Every output is an integer multiple of one master, so each pixel is exactly square.

  snap-pixels.py SOURCE.png OUT_MASTER.png --pitch 13.0 12.25 [--colours 48] [--cells 96] [--smooth 70]
  snap-pixels.py SOURCE.png --measure     prints the likeliest pitches, with their coherence

Coherence decides the mode. Above about .25 the art has a lattice: follow it, with --smooth as low as
keeps straight lines straight. Far below that it has none: use --smooth 0 and a grid fine enough for
its thinnest lines. The shipped icons, each from a 1254px source, all to 1152px by an integer scale:

  appicon-classic  --pitch 13.0  12.25 --smooth 50  --cells 96  --colours 32   then x12
  server-classic   --pitch 14.15 13.05 --smooth 320 --cells 96  --colours 32   then x12
  appicon, server  --pitch 6.53  6.53  --smooth 0   --cells 192 --colours 96   then x6
"""
import argparse, heapq
import numpy as np
from PIL import Image

SMOOTH = 70    # px: how slowly the lattice may bend. Lower lets a line slip a cell.
STEP = 8       # px: the coarse grid the phase is unwrapped on.
WINDOW = .28   # of a cell, each side of its centre: the part that is not blur.


def gauss(a, s):
    if np.iscomplexobj(a):
        return gauss(a.real, s) + 1j * gauss(a.imag, s)
    h, w = a.shape
    fy, fx = np.fft.fftfreq(h)[:, None], np.fft.rfftfreq(w)[None, :]
    return np.fft.irfft2(np.fft.rfft2(a) * np.exp(-2 * (np.pi * s) ** 2 * (fx ** 2 + fy ** 2)), a.shape)


def load(path):
    a = np.asarray(Image.open(path).convert("RGBA")).astype(np.float32)
    w = a[..., 3:4] / 255.
    return a, np.concatenate([a[..., :3] * w + 128 * (1 - w), a[..., 3:4]], 2)


def edges(c):
    ex, ey = np.zeros(c.shape[:2], np.float32), np.zeros(c.shape[:2], np.float32)
    ex[:, 1:] = np.minimum(np.abs(np.diff(c, axis=1)).sum(2), 150)
    ey[1:, :] = np.minimum(np.abs(np.diff(c, axis=0)).sum(2), 150)
    return ex, ey


def wave(e, pitch, axis):
    w = np.exp(2j * np.pi * np.arange(e.shape[axis]) / pitch)
    return e * (w[None, :] if axis == 1 else w[:, None])


def measure(path):
    _, c = load(path)
    for name, e, axis in (("x", edges(c)[0], 1), ("y", edges(c)[1], 0)):
        ps = np.arange(6, 20, .05)
        total = gauss(e, 45).sum()
        v = np.array([np.abs(gauss(wave(e, p, axis), 45)).sum() / total for p in ps])
        peaks = [i for i in range(1, len(v) - 1) if v[i - 1] < v[i] > v[i + 1]]
        print(name, [(round(float(ps[i]), 2), round(float(v[i]), 3)) for i in sorted(peaks, key=lambda i: -v[i])[:4]])


def unwrap(z):
    """Quality-guided: the surest phase first, so a blank region cannot tear the field."""
    z = z[::STEP, ::STEP]
    q, ph = np.abs(z), np.angle(z)
    h, w = z.shape
    out = np.full((h, w), np.nan)
    start = np.unravel_index(np.argmax(q), q.shape)
    out[start] = ph[start]
    todo = [(-q[start], start)]
    while todo:
        _, (y, x) = heapq.heappop(todo)
        for Y, X in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
            if 0 <= Y < h and 0 <= X < w and np.isnan(out[Y, X]):
                out[Y, X] = out[y, x] + np.angle(np.exp(1j * (ph[Y, X] - out[y, x])))
                heapq.heappush(todo, (-q[Y, X], (Y, X)))
    return out


def displacement(e, pitch, axis):
    z = wave(e, pitch, axis)
    if SMOOTH <= 0:     # rigid: one offset for the whole frame, since a field here would only warp noise
        return np.full((e.shape[0] // STEP + 1, e.shape[1] // STEP + 1), np.angle(z.sum()) * pitch / (2 * np.pi))
    u = unwrap(gauss(z, SMOOTH) + .12 * gauss(z, SMOOTH * 3)) * pitch / (2 * np.pi)
    return u - np.round(np.nanmean(u) / pitch) * pitch


def bilinear(f, y, x):
    y = np.clip(y / STEP, 0, f.shape[0] - 1.001)
    x = np.clip(x / STEP, 0, f.shape[1] - 1.001)
    y0, x0 = y.astype(int), x.astype(int)
    fy, fx = y - y0, x - x0
    return (f[y0, x0] * (1 - fy) * (1 - fx) + f[y0, x0 + 1] * (1 - fy) * fx
            + f[y0 + 1, x0] * fy * (1 - fx) + f[y0 + 1, x0 + 1] * fy * fx)


def sample(path, px, py, span):
    a, c = load(path)
    ex, ey = edges(c)
    ux, uy = displacement(ex, px, 1), displacement(ey, py, 0)
    opaque = np.where(a[..., 3] > 128)
    i0, j0 = round(opaque[1].mean() / px) - span // 2, round(opaque[0].mean() / py) - span // 2
    I, J = np.meshgrid(np.arange(span) + i0, np.arange(span) + j0)
    x0, y0 = (I + .5) * px, (J + .5) * py
    x, y = x0.copy(), y0.copy()
    for _ in range(10):
        x, y = x0 + bilinear(ux, y, x), y0 + bilinear(uy, y, x)
    rx, ry = max(1, int(px * WINDOW)), max(1, int(py * WINDOW))
    h, w = a.shape[:2]
    out = np.zeros((span, span, 4), np.uint8)
    for j in range(span):
        for i in range(span):
            xx, yy = int(round(x[j, i])), int(round(y[j, i]))
            if not (rx <= xx < w - rx and ry <= yy < h - ry):
                continue
            cell = a[yy - ry:yy + ry + 1, xx - rx:xx + rx + 1].reshape(-1, 4)
            if np.median(cell[:, 3]) >= 128:
                out[j, i, :3] = np.median(cell[cell[:, 3] >= 128][:, :3], 0)
                out[j, i, 3] = 255
    return out


def despeckle(o):
    """A lone opaque pixel goes; a one-pixel hole fills. Dither is colour, so it is untouched."""
    solid = o[..., 3] > 0
    n = np.zeros(solid.shape, int)
    n[1:] += solid[:-1]; n[:-1] += solid[1:]; n[:, 1:] += solid[:, :-1]; n[:, :-1] += solid[:, 1:]
    o = o.copy()
    o[solid & (n <= 1)] = 0
    for y, x in zip(*np.where(~solid & (n == 4))):
        around = np.array([o[y - 1, x], o[y + 1, x], o[y, x - 1], o[y, x + 1]])
        o[y, x] = np.median(around, 0).astype(np.uint8)
    return o


def palette(o, colours):
    solid = o[..., 3] > 0
    strip = Image.fromarray(o[solid][:, :3][None, ...], "RGB")
    q = strip.quantize(colours, method=Image.Quantize.MEDIANCUT, kmeans=4, dither=Image.Dither.NONE).convert("RGB")
    o = o.copy()
    o[solid, :3] = np.asarray(q)[0]
    return o


def frame(o, cells):
    ys, xs = np.where(o[..., 3] > 0)
    t = o[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    h, w = t.shape[:2]
    if max(h, w) > cells:
        raise SystemExit(f"{w}x{h} does not fit in {cells} cells")
    c = np.zeros((cells, cells, 4), np.uint8)
    y, x = (cells - h) // 2, (cells - w) // 2
    c[y:y + h, x:x + w] = t
    return c, (w, h)


def main():
    global SMOOTH
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source")
    ap.add_argument("master", nargs="?")
    ap.add_argument("--measure", action="store_true")
    ap.add_argument("--pitch", nargs=2, type=float, metavar=("X", "Y"))
    ap.add_argument("--colours", type=int, default=48)
    ap.add_argument("--cells", type=int, default=96)
    ap.add_argument("--smooth", type=float, default=SMOOTH, help="px; 0 for a rigid grid, when coherence is low")
    args = ap.parse_args()
    SMOOTH = args.smooth
    if args.measure:
        return measure(args.source)
    o, size = frame(palette(despeckle(sample(args.source, *args.pitch, args.cells + 16)), args.colours), args.cells)
    Image.fromarray(o).save(args.master)
    print(args.master, f"{size[0]}x{size[1]} in {args.cells}", len(np.unique(o[o[..., 3] > 0], axis=0)), "colours")


if __name__ == "__main__":
    main()
