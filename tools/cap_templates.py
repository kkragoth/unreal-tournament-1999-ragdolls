"""Stump cap templates -> RagdollPkg/Src/RagdollCapTemplates.h

A cap template is a small mesh in UNIT space: the rim is a circle of radius
1 in the plane z = 0 with exactly RIM_M vertices at angles k * 2pi / RIM_M
(k = 0 .. RIM_M-1, counter-clockwise seen from +z); +z points OUT of the
severed part (into the gap), -z into the flesh. Everything else is free-form:
rings of torn meat, a bone shaft, whatever. UVs index the wound atlas
(tools/build_wound_texture.py: V 0..127 meat, 128..191 clot, 192..255
bone). Vertices used only by faces in the "bone" group scale along z with
the BoneStubLength knob at runtime.

At runtime (RagdollGore.cpp, BuildTemplateCap) the real rim loop is sorted by
angle, the template's rim ring is placed ON the loop polygon (polar radius of
the loop at each ring angle), interior vertices follow that radial correction
weighted by their own radius, everything is scaled by the loop's mean radius,
and the loop is stitched to the ring with one thin strip.

Workflow (user: "hand polished", "do it in Blender"):
    python tools/cap_templates.py --regen      # (re)write the procedural starting OBJs
    <edit tools/cap_templates/<variant>.obj in Blender: keep the 16 rim verts
     on the unit circle at z=0, keep object groups "meat" / "bone", keep UVs>
    python tools/cap_templates.py              # export the header from the OBJs

Variants: shaft (broken bone sticking out), flush (bone cross-section level
with the meat), head (rounded bone end, joint-level cut on the limb side),
socket (no bone, joint-level stump).
"""

import argparse
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
TPL_DIR = os.path.join(HERE, "cap_templates")
OUT_H = os.path.join(REPO, "RagdollPkg", "Src", "RagdollCapTemplates.h")
RIM_M = 16
BONE_SEG = 8
VARIANTS = ["shaft", "flush", "head", "socket"]
# Atlas bands (V 0..255).
V_MEAT_LIGHT, V_MEAT_DARK, V_CLOT, V_BONE0, V_BONE1, V_BONE_TIP = 20, 100, 170, 198, 250, 222


class Tpl:
    def __init__(self):
        self.v = []      # (x, y, z)
        self.vt = []     # (u, v) in 0..1
        self.faces = {"meat": [], "bone": []}   # (vi, ti) triples

    def add(self, p, uv):
        self.v.append(p)
        self.vt.append(uv)
        return len(self.v) - 1

    def ring(self, r, z, uvv, n=RIM_M, lumps=0, amp=0.0, phase=0.0, ramp=0.0):
        """Ring of n verts; z and r modulated by 'lumps' smooth bumps around it."""
        out = []
        for k in range(n):
            a = 2 * math.pi * k / n
            zz = z + amp * math.sin(lumps * a + phase) if lumps else z
            rr = r + ramp * math.cos(lumps * a + phase) if lumps else r
            out.append(self.add((rr * math.cos(a), rr * math.sin(a), zz), (k / n, uvv / 255.0)))
        return out

    def bridge(self, ra, rb, group):
        """Quad strip between two rings of equal size, wound so normals face +z/outward."""
        n = len(ra)
        for k in range(n):
            k2 = (k + 1) % n
            self.faces[group].append(((ra[k], ra[k]), (rb[k], rb[k]), (ra[k2], ra[k2])))
            self.faces[group].append(((ra[k2], ra[k2]), (rb[k], rb[k]), (rb[k2], rb[k2])))

    def fan(self, ring, centre, group):
        n = len(ring)
        for k in range(n):
            self.faces[group].append(((ring[k], ring[k]), (ring[(k + 1) % n], ring[(k + 1) % n]), (centre, centre)))


def build_variant(name):
    T = Tpl()
    # Rim ring (exact unit circle, z = 0) - MUST stay first and unchanged.
    rim = T.ring(1.0, 0.0, V_CLOT)
    # Torn flesh: a clotted lip that everts slightly, then meat bulging out
    # in two smooth lumpy rings, settling toward the bone. Low-frequency
    # bumps (3 and 2 around) instead of per-vertex jitter.
    lip = T.ring(0.93, 0.16, V_CLOT - 15, lumps=3, amp=0.05, phase=0.4, ramp=0.02)
    meat1 = T.ring(0.72, 0.24, V_MEAT_DARK, lumps=3, amp=0.07, phase=1.1, ramp=0.03)
    meat2 = T.ring(0.47, 0.14, V_MEAT_LIGHT + 35, lumps=2, amp=0.05, phase=2.0, ramp=0.02)
    T.bridge(rim, lip, "meat")
    T.bridge(lip, meat1, "meat")
    T.bridge(meat1, meat2, "meat")
    if name == "socket":
        # Empty joint socket: the meat dips into a cup.
        cup = T.ring(0.24, -0.10, V_MEAT_LIGHT + 10)
        T.bridge(meat2, cup, "meat")
        c = T.add((0.0, 0.0, -0.22), (0.5, (V_MEAT_DARK + 10) / 255.0))
        T.fan(cup, c, "meat")
        return T
    # Bone base ring at the meat floor (8 segments: a second ring of 16 would
    # waste verts; the 16 -> 8 transition is a fan of meat triangles).
    base16 = T.ring(0.27, 0.02, V_MEAT_LIGHT)
    T.bridge(meat2, base16, "meat")
    rb = 0.25
    base = T.ring(rb, 0.02, V_BONE0, n=BONE_SEG)
    # meat floor between base16 (16) and base (8): pair each 8-vert with two 16-verts
    for k in range(BONE_SEG):
        a0, a1, a2 = base16[(2 * k) % RIM_M], base16[(2 * k + 1) % RIM_M], base16[(2 * k + 2) % RIM_M]
        b0, b1 = base[k], base[(k + 1) % BONE_SEG]
        T.faces["meat"].append(((a0, a0), (b0, b0), (a1, a1)))
        T.faces["meat"].append(((a1, a1), (b0, b0), (b1, b1)))
        T.faces["meat"].append(((a1, a1), (b1, b1), (a2, a2)))
    if name == "shaft":
        knob = T.ring(rb * 1.3, 0.45, V_BONE0 + 22, n=BONE_SEG, lumps=2, amp=0.03, ramp=0.03)
        neck = T.ring(rb * 1.05, 0.75, V_BONE0 + 40, n=BONE_SEG)
        tip = T.ring(rb * 0.9, 0.98, V_BONE1, n=BONE_SEG, lumps=3, amp=0.07, phase=0.7, ramp=0.04)
        T.bridge(base, knob, "bone")
        T.bridge(knob, neck, "bone")
        T.bridge(neck, tip, "bone")
        c = T.add((0.05, -0.03, 1.08), (0.5, V_BONE_TIP / 255.0))
        T.fan(tip, c, "bone")
    elif name == "flush":
        tip = T.ring(rb * 0.95, 0.16, V_BONE0 + 30, n=BONE_SEG, lumps=2, amp=0.02, phase=0.3)
        T.bridge(base, tip, "bone")
        c = T.add((0.0, 0.0, 0.20), (0.5, V_BONE_TIP / 255.0))
        T.fan(tip, c, "bone")
    elif name == "head":
        r1 = T.ring(rb * 1.15, 0.22, V_BONE0 + 15, n=BONE_SEG)
        r2 = T.ring(rb * 1.40, 0.46, V_BONE0 + 30, n=BONE_SEG)
        r3 = T.ring(rb * 1.15, 0.68, V_BONE0 + 45, n=BONE_SEG)
        T.bridge(base, r1, "bone")
        T.bridge(r1, r2, "bone")
        T.bridge(r2, r3, "bone")
        c = T.add((0.0, 0.0, 0.80), (0.5, (V_BONE0 + 50) / 255.0))
        T.fan(r3, c, "bone")
    return T


# --------------------------------------------------------------------------- OBJ I/O
def write_obj(T, path):
    with open(path, "w") as fp:
        fp.write("# cap template (tools/cap_templates.py). Unit rim: first %d verts on the unit circle at z=0.\n" % RIM_M)
        for p in T.v:
            fp.write("v %.5f %.5f %.5f\n" % p)
        for uv in T.vt:
            fp.write("vt %.5f %.5f\n" % (uv[0], 1.0 - uv[1]))
        for g in ("meat", "bone"):
            fp.write("o %s\n" % g)
            for f in T.faces[g]:
                fp.write("f %s\n" % " ".join("%d/%d" % (vi + 1, ti + 1) for vi, ti in f))


def read_obj(path):
    T = Tpl()
    group = "meat"
    with open(path) as fp:
        for line in fp:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "v":
                T.v.append(tuple(float(x) for x in parts[1:4]))
            elif parts[0] == "vt":
                T.vt.append((float(parts[1]), 1.0 - float(parts[2])))
            elif parts[0] == "o":
                group = "bone" if parts[1].startswith("bone") else "meat"
            elif parts[0] == "f":
                corners = []
                for c in parts[1:]:
                    idx = c.split("/")
                    vi = int(idx[0]) - 1
                    ti = int(idx[1]) - 1 if len(idx) > 1 and idx[1] else vi
                    corners.append((vi, ti))
                for i in range(1, len(corners) - 1):     # fan-triangulate n-gons
                    T.faces[group].append((corners[0], corners[i], corners[i + 1]))
    return T


def validate(T, name):
    assert len(T.v) >= RIM_M, "%s: fewer than %d verts" % (name, RIM_M)
    for k in range(RIM_M):
        x, y, z = T.v[k]
        a = 2 * math.pi * k / RIM_M
        ex, ey = math.cos(a), math.sin(a)
        if abs(x - ex) > 0.02 or abs(y - ey) > 0.02 or abs(z) > 0.01:
            raise SystemExit("%s: rim vertex %d is at (%.3f %.3f %.3f), expected (%.3f %.3f 0)" % (name, k, x, y, z, ex, ey))
    for g in T.faces:
        for f in T.faces[g]:
            for vi, ti in f:
                assert 0 <= vi < len(T.v) and 0 <= ti < len(T.vt), "%s: bad index" % name


def bone_verts(T):
    """Vertices that only bone faces use (scaled by the stub-length knob)."""
    in_meat = set(vi for f in T.faces["meat"] for vi, _ in f)
    in_bone = set(vi for f in T.faces["bone"] for vi, _ in f)
    return in_bone - in_meat


# --------------------------------------------------------------------------- header
def emit_header(tpls, path):
    L = ["// Generated by tools/cap_templates.py from tools/cap_templates/*.obj. DO NOT EDIT.",
         "// Stump cap templates in unit space (rim = %d verts on the unit circle at z=0," % RIM_M,
         "// +z out of the severed part). See the tool's docstring and RagdollGore.cpp.",
         "#ifndef __RAGDOLLCAPTEMPLATES_H__", "#define __RAGDOLLCAPTEMPLATES_H__", "",
         "#define RAGDOLL_CAP_RIM %d" % RIM_M,
         "#define RAGDOLL_CAP_VARIANTS %d" % len(tpls),
         "enum ERagdollCapVariant { %s };" % ", ".join("CAP_%s" % n.upper() for n, _ in tpls), "",
         "struct FRagdollCapVert { FLOAT X, Y, Z; BYTE U, V; BYTE bBone; };",
         "struct FRagdollCapFace { _WORD V[3]; BYTE bBone; };",
         "struct FRagdollCapTemplate { const TCHAR* Name; INT NumVerts; INT NumFaces; const FRagdollCapVert* Verts; const FRagdollCapFace* Faces; };", ""]
    for name, T in tpls:
        bv = bone_verts(T)
        # One output vertex per (vertex, uv) pair actually used, so UV seams
        # work. The RIM_M rim vertices come first (output 0..RIM_M-1 = ring
        # vertex k at angle k*2pi/RIM_M), which the runtime relies on.
        pairs, index = [], {}
        faces = []
        first_t = {}
        for g in ("meat", "bone"):
            for f in T.faces[g]:
                for vi, ti in f:
                    first_t.setdefault(vi, ti)
        for vi in range(RIM_M):
            key = (vi, first_t.get(vi, vi))
            index[key] = len(pairs)
            pairs.append(key)
        for g in ("meat", "bone"):
            for f in T.faces[g]:
                tri = []
                for vi, ti in f:
                    key = (vi, ti)
                    if key not in index:
                        index[key] = len(pairs)
                        pairs.append(key)
                    tri.append(index[key])
                faces.append((tri, g == "bone"))
        L.append("static const FRagdollCapVert GCap_%s_Verts[%d] =" % (name, len(pairs)))
        L.append("{")
        for vi, ti in pairs:
            x, y, z = T.v[vi]
            u, v = T.vt[ti]
            L.append("\t{ %.4ff, %.4ff, %.4ff, %d, %d, %d }," % (x, y, z, int(round(u * 255)) & 255, int(round(v * 255)) & 255, 1 if vi in bv else 0))
        L.append("};")
        L.append("static const FRagdollCapFace GCap_%s_Faces[%d] =" % (name, len(faces)))
        L.append("{")
        for tri, bone in faces:
            L.append("\t{ { %d, %d, %d }, %d }," % (tri[0], tri[1], tri[2], 1 if bone else 0))
        L.append("};")
        L.append("")
    L.append("static const FRagdollCapTemplate GRagdollCapTemplates[RAGDOLL_CAP_VARIANTS] =")
    L.append("{")
    for name, T in tpls:
        L.append("\t{ TEXT(\"%s\"), sizeof(GCap_%s_Verts)/sizeof(GCap_%s_Verts[0]), sizeof(GCap_%s_Faces)/sizeof(GCap_%s_Faces[0]), GCap_%s_Verts, GCap_%s_Faces }," % ((name,) * 7))
    L.append("};")
    L.append("")
    L.append("#endif")
    with open(path, "w") as fp:
        fp.write("\n".join(L) + "\n")
    print("wrote", path, ", ".join("%s(%dv/%df)" % (n, len(T.v), sum(len(f) for f in T.faces.values())) for n, T in tpls))


def load_templates(regen=False):
    os.makedirs(TPL_DIR, exist_ok=True)
    tpls = []
    for name in VARIANTS:
        path = os.path.join(TPL_DIR, name + ".obj")
        if regen or not os.path.exists(path):
            write_obj(build_variant(name), path)
            print("wrote", path)
        T = read_obj(path)
        validate(T, name)
        tpls.append((name, T))
    return tpls


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--regen", action="store_true", help="overwrite the OBJs with the procedural templates")
    ap.add_argument("--header", default=OUT_H)
    args = ap.parse_args()
    emit_header(load_templates(args.regen), args.header)


if __name__ == "__main__":
    main()
