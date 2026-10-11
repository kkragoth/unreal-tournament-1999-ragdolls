"""Wound atlas for the dismemberment caps -> RagdollPkg/Src/RagdollWoundTexture.h

A 64x64 palettised (P8) texture with mips, laid out in three horizontal
bands that tile in U (the caps map the angle around the rim to U):

    V   0..127  meat: muscle red, lighter at the top (V=0, near the bone) and
                darker toward V=127, with marbled streaks along V
    V 128..191  clot: near-black congealed blood with a few glossy flecks
    V 192..255  bone: ivory, slightly yellow, faint cracks, pinkish marrow

Procedural by default (value noise, seeded), or quantised from a hand-painted
PNG with --from (any size, resampled to 64x64; needs Pillow). Always writes a
PNG preview next to the header (tools/rig_verify/wound_atlas.png) so the
result can be looked at / edited.

    python tools/build_wound_texture.py
    python tools/build_wound_texture.py --from my_wound.png
"""

import argparse
import math
import os
import random
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
OUT_H = os.path.join(REPO, "RagdollPkg", "Src", "RagdollWoundTexture.h")
OUT_PNG = os.path.join(HERE, "rig_verify", "wound_atlas.png")
SIZE = 64


# --------------------------------------------------------------------------- noise
class Noise:
    def __init__(self, seed, period):
        rng = random.Random(seed)
        self.period = period
        self.grid = [[rng.random() for _ in range(period)] for _ in range(period)]

    def value(self, x, y):
        """Smooth value noise, tileable with 'period' cells."""
        p = self.period
        x0, y0 = int(math.floor(x)) % p, int(math.floor(y)) % p
        fx, fy = x - math.floor(x), y - math.floor(y)
        sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        g = self.grid
        a = g[y0][x0] + (g[y0][(x0 + 1) % p] - g[y0][x0]) * sx
        b = g[(y0 + 1) % p][x0] + (g[(y0 + 1) % p][(x0 + 1) % p] - g[(y0 + 1) % p][x0]) * sx
        return a + (b - a) * sy


def fbm(noises, x, y):
    v, amp, tot = 0.0, 1.0, 0.0
    for n in noises:
        v += n.value(x * n.period / 8.0, y * n.period / 8.0) * amp
        tot += amp
        amp *= 0.5
    return v / tot


def clamp(v, lo=0, hi=255):
    return int(max(lo, min(hi, round(v))))


def lerp3(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


# --------------------------------------------------------------------------- painting
def paint_procedural(seed=7):
    """Returns a SIZE x SIZE list of (r, g, b)."""
    rng = random.Random(seed)
    meat_n = [Noise(seed + 1, 4), Noise(seed + 2, 8), Noise(seed + 3, 16)]
    streak_n = [Noise(seed + 11, 16), Noise(seed + 12, 32)]
    clot_n = [Noise(seed + 21, 8), Noise(seed + 22, 16)]
    bone_n = [Noise(seed + 31, 4), Noise(seed + 32, 8)]
    crack_n = Noise(seed + 41, 16)
    img = [[(0, 0, 0)] * SIZE for _ in range(SIZE)]
    for y in range(SIZE):
        for x in range(SIZE):
            u, v = x / SIZE, y / SIZE
            if y < 32:
                # Meat: fibres run along V (radially on the cap). Light near V=0.
                t = y / 31.0
                base = lerp3((150, 32, 28), (78, 10, 10), t)
                fib = fbm(streak_n, u * 8.0, v * 1.5)          # elongated along V
                marb = fbm(meat_n, u * 8.0, v * 8.0)
                k = 0.75 + 0.5 * (fib - 0.5) + 0.3 * (marb - 0.5)
                col = tuple(c * k for c in base)
                if marb > 0.78:                                  # fat / sinew fleck
                    col = lerp3(col, (190, 150, 120), (marb - 0.78) * 3.0)
                if fib < 0.22:                                   # dark vessel
                    col = lerp3(col, (40, 4, 6), (0.22 - fib) * 3.0)
            elif y < 48:
                # Clot: congealed blood, almost black, wet flecks.
                n = fbm(clot_n, u * 8.0, v * 8.0)
                col = lerp3((30, 3, 4), (70, 8, 10), n)
                if n > 0.8:
                    col = lerp3(col, (120, 20, 24), (n - 0.8) * 4.0)
                # Edge rows blend toward the meat band so the strip has no hard line.
                if y < 34:
                    col = lerp3(col, (78, 10, 10), (34 - y) / 2.5)
            else:
                # Bone: ivory with a yellow tint, faint cracks, marrow pink at V~222.
                n = fbm(bone_n, u * 8.0, v * 8.0)
                col = lerp3((205, 192, 160), (236, 226, 200), n)
                cr = crack_n.value(u * 16.0, v * 16.0)
                if cr < 0.14:
                    col = lerp3(col, (120, 100, 70), (0.14 - cr) * 5.0)
                # Marrow stripe around the tip row (V 212..232 -> y 53..58).
                dm = abs(y - 55.5)
                if dm < 3.0:
                    col = lerp3(col, (170, 80, 70), (1.0 - dm / 3.0) * 0.6 * (0.6 + 0.4 * n))
            img[y][x] = (clamp(col[0]), clamp(col[1]), clamp(col[2]))
    return img


def load_png(path):
    from PIL import Image  # optional dependency, only for --from
    im = Image.open(path).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS)
    px = im.load()
    return [[px[x, y] for x in range(SIZE)] for y in range(SIZE)]


# --------------------------------------------------------------------------- quantise + mips
def quantise(img):
    """Median-cut-ish quantisation to 255 colours (index 0 reserved). Returns (palette, indices)."""
    colors = {}
    for row in img:
        for c in row:
            colors[c] = colors.get(c, 0) + 1
    boxes = [list(colors.items())]
    while len(boxes) < 255:
        # split the box with the largest channel range
        best, bi, bch = -1, -1, 0
        for i, b in enumerate(boxes):
            if len(b) < 2:
                continue
            for ch in range(3):
                r = max(c[ch] for c, _ in b) - min(c[ch] for c, _ in b)
                if r > best:
                    best, bi, bch = r, i, ch
        if bi < 0:
            break
        b = sorted(boxes[bi], key=lambda cw: cw[0][bch])
        tot = sum(w for _, w in b)
        acc, cut = 0, 0
        for k, (_, w) in enumerate(b):
            acc += w
            if acc >= tot / 2 and k < len(b) - 1:
                cut = k + 1
                break
        if cut == 0:
            cut = len(b) // 2
        boxes[bi:bi + 1] = [b[:cut], b[cut:]]
    palette = [(0, 0, 0)]
    for b in boxes:
        tot = sum(w for _, w in b)
        palette.append(tuple(int(round(sum(c[ch] * w for c, w in b) / tot)) for ch in range(3)))
    while len(palette) < 256:
        palette.append((0, 0, 0))
    lut = {}
    def best_index(c):
        if c in lut:
            return lut[c]
        bi, bd = 1, 1 << 30
        for i in range(1, 256):
            p = palette[i]
            d = (p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 + (p[2] - c[2]) ** 2
            if d < bd:
                bd, bi = d, i
        lut[c] = bi
        return bi
    idx = [[best_index(c) for c in row] for row in img]
    return palette, idx, best_index


def mips(img, palette, best_index):
    """Box-filtered RGB mips, each quantised to the shared palette."""
    out = []
    cur = img
    size = SIZE
    while size >= 1:
        out.append([[best_index(c) for c in row] for row in cur])
        if size == 1:
            break
        nxt = []
        for y in range(size // 2):
            row = []
            for x in range(size // 2):
                cs = [cur[2 * y][2 * x], cur[2 * y][2 * x + 1], cur[2 * y + 1][2 * x], cur[2 * y + 1][2 * x + 1]]
                row.append(tuple(int(round(sum(c[ch] for c in cs) / 4.0)) for ch in range(3)))
            nxt.append(row)
        cur = nxt
        size //= 2
    return out


# --------------------------------------------------------------------------- output
def write_png(path, img):
    h = len(img)
    w = len(img[0])
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in img)
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fp:
        fp.write(png)


def write_header(path, palette, levels):
    lines = ["// Generated by tools/build_wound_texture.py. DO NOT EDIT.",
             "// 64x64 P8 wound atlas for the dismemberment caps (bands in V: meat",
             "// 0..127, clot 128..191, bone 192..255; tileable in U) with %d mips." % len(levels),
             "#ifndef __RAGDOLLWOUNDTEXTURE_H__", "#define __RAGDOLLWOUNDTEXTURE_H__", "",
             "#define RAGDOLL_WOUND_SIZE %d" % SIZE,
             "#define RAGDOLL_WOUND_BITS %d" % int(math.log2(SIZE)),
             "#define RAGDOLL_WOUND_MIPS %d" % len(levels), "",
             "static const BYTE GRagdollWoundPalette[256][3] =", "{"]
    for i in range(0, 256, 8):
        lines.append("\t" + " ".join("{%3d,%3d,%3d}," % palette[j] for j in range(i, i + 8)))
    lines.append("};")
    lines.append("")
    lines.append("// Mip levels, largest first, row-major; offsets into GRagdollWoundTexels.")
    offs, texels = [], []
    for lvl in levels:
        offs.append(len(texels))
        for row in lvl:
            texels.extend(row)
    lines.append("static const INT GRagdollWoundMipOffset[RAGDOLL_WOUND_MIPS] = { %s };" % ", ".join(str(o) for o in offs))
    lines.append("static const BYTE GRagdollWoundTexels[%d] =" % len(texels))
    lines.append("{")
    for i in range(0, len(texels), 32):
        lines.append("\t" + ",".join("%3d" % t for t in texels[i:i + 32]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    with open(path, "w") as fp:
        fp.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="src", help="hand-painted PNG to quantise instead of the procedural atlas")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--header", default=OUT_H)
    ap.add_argument("--png", default=OUT_PNG)
    args = ap.parse_args()
    img = load_png(args.src) if args.src else paint_procedural(args.seed)
    palette, idx, best_index = quantise(img)
    levels = mips(img, palette, best_index)
    write_header(args.header, palette, levels)
    # Preview from the quantised level 0 so it shows what the game shows.
    write_png(args.png, [[palette[i] for i in row] for row in levels[0]])
    print("wrote %s (%d colours, %d mips) and %s" % (args.header, len(set(i for row in idx for i in row)), len(levels), args.png))


if __name__ == "__main__":
    main()
