"""Offline prototype of the planar dismemberment cut (RagdollGore.cpp).

Reads a dataset straight out of RagdollPkg/Src/RagdollDatasets.h (the exact
arrays the native code sees), applies joint cuts / fragment planes with the
same algorithm the DLL uses, and writes one OBJ object per rigid island plus
"cap_*" objects for the planar stump caps, so the result can be rendered with
tools/render_groups.py:

    python tools/plane_cut.py SelectionMale1 --cut 6 9 --out tools/rig_verify/cut_m1.obj
    python tools/plane_cut.py SelectionMale1 --gib --frag 1:3 3:3 --seed 3 --explode 6 --out ...
    "<blender 3.6>" -b --python-exit-code 1 -P tools/render_groups.py -- out.png 60 3q tools/rig_verify/cut_m1.obj

Algorithm (kept in lock-step with RagdollSplitIslands):
  1. Each body belongs to a rigid island (walk up the bone tree until a cut
     joint). A vertex starts in the island of its dominant bind bone and is
     then re-classified by every cut joint's plane (anchor, child axis):
     child-dominated verts above the plane go to the parent island,
     parent-dominated verts below it and within the limb radius go to the
     child island. Weights on other islands are dropped.
  2. A face whose vertices span two islands is clipped against the plane that
     separates them (joint plane or fragment plane): new vertices on the
     plane, 1+2 triangles, each side keeps its part. Faces no plane separates
     fall back to the old per-island copy with duplicated foreign vertices.
  3. The on-plane edges of each island form a planar rim that is fan-capped
     from its centroid with the cap material.
"""

import argparse
import math
import os
import random
import re
import sys

BONE_COUNT = 14
PARENT = [-1, 0, 1, 2, 1, 1, 4, 5, 0, 0, 8, 9, 10, 11]
EPS = 0.02
BAND_MARGIN = 2.0      # parent verts this far beyond the ring's wander still move to the child
RADIUS_SCALE = 1.35    # ... when within this multiple of the ring radius of the axis
RADIUS_MARGIN = 1.0
FALLBACK_CAP_SCALE = 0.8   # non-planar rims: only fan edges this close to the rim centroid
FALLBACK_CAP_MARGIN = 1.5
NAMES = ["Pelvis", "Spine", "Neck", "Head", "UpperArmL", "UpperArmR", "ForearmL", "ForearmR",
         "ThighL", "ThighR", "ShinL", "ShinR", "FootL", "FootR"]


# --------------------------------------------------------------------------- header parsing
def _block(text, name):
    m = re.search(r"static const \w+ %s\[\d+\] = \{(.*?)\n\};" % re.escape(name), text, re.S)
    if not m:
        raise KeyError(name)
    return m.group(1)


def load_dataset(header, sel):
    text = open(header, encoding="utf-8", errors="replace").read()
    fv = r"FVector\(\s*([-\d.e+]+),\s*([-\d.e+]+),\s*([-\d.e+]+)\s*\)"
    verts = [tuple(map(float, t)) for t in re.findall(fv, _block(text, "G%s_Verts" % sel))]
    faces = [(int(a), int(b), int(c), int(m)) for a, b, c, m in
             re.findall(r"\{\s*\{\s*(\d+),\s*(\d+),\s*(\d+)\s*\},\s*(\d+)\s*\}", _block(text, "G%s_Faces" % sel))]
    wedges = [(int(v), int(u), int(w)) for v, u, w in
              re.findall(r"\{\s*(\d+),\s*\{\s*(\d+),\s*(\d+)\s*\}\s*\}", _block(text, "G%s_Wedges" % sel))]
    bones = [tuple(map(float, t)) for t in
             re.findall(r"\{\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+)\s*\}",
                        _block(text, "G%s_Bones" % sel))]
    anchors = [tuple(map(float, t)) for t in re.findall(fv, _block(text, "G%s_Anchors" % sel))]
    bb = [int(x) for x in re.findall(r"-?\d+", _block(text, "G%s_BindBones" % sel))]
    bw = [float(x) for x in re.findall(r"[-\d.]+", _block(text, "G%s_BindWeights" % sel))]
    n = len(verts)
    assert len(bb) == 3 * n and len(bw) == 3 * n, (len(bb), len(bw), n)
    binds = [([bb[3 * i], bb[3 * i + 1], bb[3 * i + 2]], [bw[3 * i], bw[3 * i + 1], bw[3 * i + 2]]) for i in range(n)]
    return dict(verts=verts, faces=faces, wedges=wedges, bones=bones, anchors=anchors, binds=binds)


# --------------------------------------------------------------------------- vector helpers
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def length(a): return math.sqrt(dot(a, a))
def norm(a):
    l = length(a)
    return mul(a, 1.0 / l) if l > 1e-9 else (0.0, 0.0, 1.0)
def lerp(a, b, t): return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def in_subtree(b, anc):
    """True when bone b is anc or hangs below it in the bone tree (fragments never are)."""
    if b >= BONE_COUNT:
        return False
    while b >= 0:
        if b == anc:
            return True
        b = PARENT[b]
    return False


# --------------------------------------------------------------------------- state
class State:
    """Mirror of the native FRagdollState pieces the cut touches."""

    def __init__(self, ds):
        self.bones = list(ds["bones"])            # (hx,hy,hz,cx,cy,cz,mass) per bone
        self.anchors = ds["anchors"]
        self.pos = list(ds["verts"])              # BindPos
        self.binds = [(list(b), list(w)) for b, w in ds["binds"]]
        self.faces = list(ds["faces"])            # (w0,w1,w2,mat)
        self.wedges = list(ds["wedges"])          # (vert,u,v)
        self.cut = [False] * BONE_COUNT
        self.alive = [True] * BONE_COUNT
        self.num_bodies = 12                      # real-mesh: no foot bodies
        self.alive[12] = self.alive[13] = False
        self.extra_center = []                    # fragment bodies
        self.extra_radius = []
        self.frag_planes = []                     # (A, n) in creation order
        self.joint_planes = {}                    # c -> (A, n, W, R), fixed when the cut first appears
        self.cap_mat = 7
        self.splits = 0

    # bone helpers -------------------------------------------------------
    def num_bones(self): return BONE_COUNT + len(self.extra_center)

    def bone_alive(self, b):
        if b >= BONE_COUNT:
            return b - BONE_COUNT < len(self.extra_center)
        return 0 <= b < self.num_bodies and self.alive[b]

    def center(self, b):
        if b >= BONE_COUNT:
            return self.extra_center[b - BONE_COUNT]
        d = self.bones[b]
        return (d[3], d[4], d[5])

    def radius_perp(self, b):
        if b >= BONE_COUNT:
            return self.extra_radius[b - BONE_COUNT]
        return max(self.bones[b][0], self.bones[b][1])

    def island_root(self, b):
        if b >= BONE_COUNT:
            return b
        while 0 <= b < BONE_COUNT and not self.cut[b] and PARENT[b] >= 0:
            b = PARENT[b]
        return b

    def fit_joint_plane(self, c, island):
        """Cut plane of joint c, fitted to the current heat-weight seam ring.

        Point = centroid of the vertices on faces straddling c's island and its
        parent's island; normal = from that point toward the centroid of the
        vertices bound to c's subtree (down the limb). W = how far the ring
        wanders off the plane, R = the ring's radius about the axis. Falls
        back to the joint anchor + child-centre axis when there is no ring.
        """
        p = PARENT[c]
        Ip = island[p]
        vi = [island[b[0][0]] for b in self.binds]
        ring = set()
        for f in self.faces:
            vs = [self.wedges[f[k]][0] for k in range(3)]
            isl = [vi[v] for v in vs]
            if c in isl and Ip in isl:
                ring.update(v for v, i in zip(vs, isl) if i in (c, Ip))
        sub_vs = [v for v in range(len(self.pos)) if in_subtree(self.binds[v][0][0], c)]
        if len(ring) < 3 or not sub_vs:
            a = self.anchors[c - 1]
            n = norm(sub(self.center(c), a))
            return a, n, 2.0, 1.6 * self.radius_perp(c) + 1.5
        C = mul((sum(self.pos[v][0] for v in ring), sum(self.pos[v][1] for v in ring), sum(self.pos[v][2] for v in ring)), 1.0 / len(ring))
        L = mul((sum(self.pos[v][0] for v in sub_vs), sum(self.pos[v][1] for v in sub_vs), sum(self.pos[v][2] for v in sub_vs)), 1.0 / len(sub_vs))
        n = norm(sub(L, C))
        W = R = 0.0
        for v in ring:
            rel = sub(self.pos[v], C)
            d = dot(rel, n)
            W = max(W, abs(d))
            R = max(R, length(sub(rel, mul(n, d))))
        return C, n, W, R

    # mesh helpers -------------------------------------------------------
    def add_vertex(self, p, bones, weights):
        self.pos.append(p)
        self.binds.append((list(bones), list(weights)))
        return len(self.pos) - 1

    def add_rigid(self, p, bone):
        return self.add_vertex(p, [bone, bone, bone], [1.0, 0.0, 0.0])

    def add_wedge(self, v, u, w):
        self.wedges.append((v, int(round(u)) & 255, int(round(w)) & 255))
        return len(self.wedges) - 1

    def nearest_in_island(self, island, I, p):
        best, bd = -1, 1e30
        for b in range(self.num_bones()):
            if not self.bone_alive(b) or island[b] != I:
                continue
            d = length(sub(p, self.center(b)))
            if d < bd:
                bd, best = d, b
        return best

    def restrict(self, v, island, I, pref):
        """Weights of vertex v restricted to island I (renormalised); rigid on pref/nearest if none."""
        bones, weights = self.binds[v]
        nb, nw = [], []
        for b, w in zip(bones, weights):
            if w > 0 and self.bone_alive(b) and island[b] == I:
                nb.append(b); nw.append(w)
        if not nw:
            b = pref if (pref is not None and island[pref] == I) else self.nearest_in_island(island, I, self.pos[v])
            return [b, b, b], [1.0, 0.0, 0.0]
        s = sum(nw)
        nw = [w / s for w in nw]
        while len(nb) < 3:
            nb.append(nb[0]); nw.append(0.0)
        return nb, nw


# --------------------------------------------------------------------------- the split
def split_islands(S, verbose=True):
    island = [S.island_root(b) if S.bone_alive(b) else -1 for b in range(BONE_COUNT + 24)]
    nv0, nf0 = len(S.pos), len(S.faces)

    # Joint planes of the cut joints (fitted once, when the cut first appears).
    for c in range(1, S.num_bodies):
        if S.alive[c] and S.cut[c] and c not in S.joint_planes:
            S.joint_planes[c] = S.fit_joint_plane(c, island)
    jp = {c: pl for c, pl in S.joint_planes.items() if S.alive[c] and S.cut[c]}

    # 1. Vertex islands: dominant bone, refined by the joint planes.
    visl = [0] * nv0
    pref = [None] * nv0
    moved = 0
    for v in range(nv0):
        b = S.binds[v][0][0]
        I = island[b]
        p = S.pos[v]
        for c, (a, n, W, R) in jp.items():
            par = PARENT[c]
            d = dot(sub(p, a), n)
            if b == c and d < -EPS:
                I, pref[v] = island[par], par
                moved += 1
            elif b == par and EPS < d < W + BAND_MARGIN:
                rel = sub(p, a)
                perp = length(sub(rel, mul(n, d)))
                if perp < min(R * RADIUS_SCALE + RADIUS_MARGIN, 1.3 * S.radius_perp(c) + 1.5):
                    I, pref[v] = c, c
                    moved += 1
        visl[v] = I
    orig = [(list(b), list(w)) for b, w in S.binds]
    for v in range(nv0):
        S.binds[v] = S.restrict(v, island, visl[v], pref[v])

    # Candidate separating planes for an island pair: joint planes of either
    # island first (if it is a cut child), then the fragment planes in order.
    def candidates(I, J):
        out = []
        for X in (I, J):
            if 0 < X < BONE_COUNT and X in jp:
                out.append(("j%d" % X, jp[X][0], jp[X][1]))
        for k, (a, n) in enumerate(S.frag_planes):
            out.append(("f%d" % k, a, n))
        return out

    dup_map = {}       # (v, I) -> copy of v in island I
    isect = {}         # (I, a, b) -> vertex on the plane between a and b, bound to I
    rim = {}           # group -> {edge(a<b): count}
    rim_pts = {}       # group -> set of rim vertex ids
    fallback_sum = {}  # group -> [sum, n] of duplicated positions (fallback apex)
    n_seam = n_planar = n_fallback = 0

    def dup(v, J, I):
        key = (v, I)
        if key in dup_map:
            return dup_map[key]
        bones, weights = orig[v]
        bone = next((b for b, w in zip(bones, weights) if w > 0 and island[b] == I), -1)
        if bone < 0:
            bone = S.nearest_in_island(island, I, S.pos[v])
        nv = S.add_rigid(S.pos[v], bone)
        dup_map[key] = nv
        g = ("dup", I, J)
        s = fallback_sum.setdefault(g, [(0.0, 0.0, 0.0), 0])
        s[0] = add(s[0], S.pos[v]); s[1] += 1
        return nv

    def count_edge(group, a, b):
        e = (min(a, b), max(a, b))
        rim.setdefault(group, {})
        rim[group][e] = rim[group].get(e, 0) + 1
        rim_pts.setdefault(group, set()).update(e)

    new_faces = []
    for f in range(nf0):
        w0, w1, w2, mat = S.faces[f]
        ws = (w0, w1, w2)
        vs = [S.wedges[w][0] for w in ws]
        isl = [visl[v] if v < nv0 else -1 for v in vs]
        if isl[0] == isl[1] == isl[2]:
            new_faces.append(S.faces[f])
            continue
        n_seam += 1
        present = []
        for i in isl:
            if i >= 0 and i not in present:
                present.append(i)

        plane = None
        if len(present) == 2:
            I, J = present
            P = [S.pos[v] for v in vs]
            for pid, a, n in candidates(I, J):
                d = [dot(sub(p, a), n) for p in P]
                # sign s such that island I lies at s*d <= eps and J at s*d >= -eps
                for s in (1.0, -1.0):
                    ok = all((s * d[k] <= EPS) if isl[k] == I else (s * d[k] >= -EPS) for k in range(3))
                    if ok and any(abs(x) > EPS for x in d):
                        plane = (pid, a, n, d, s)
                        break
                if plane:
                    break

        if plane is None:
            # Fallback: one copy per island present, foreign vertices duplicated.
            n_fallback += 1
            for I in present:
                nw = list(ws)
                for k in range(3):
                    if isl[k] != I:
                        _, u, vv = S.wedges[ws[k]]
                        nw[k] = S.add_wedge(dup(vs[k], isl[k], I), u, vv)
                new_faces.append((nw[0], nw[1], nw[2], mat))
                for e in range(3):
                    e2 = (e + 1) % 3
                    if isl[e] == I or isl[e2] == I:
                        continue
                    count_edge(("dup", I, isl[e]), vs[e], vs[e2])
            continue

        # Planar clip: each side keeps the part of the triangle in its half-space.
        n_planar += 1
        pid, a, n, d, s = plane
        for side, I in ((s, present[0]), (-s, present[1])):
            poly = []   # (wedge, on_plane)
            for k in range(3):
                k2 = (k + 1) % 3
                dk, dk2 = side * d[k], side * d[k2]
                if dk <= EPS:
                    v = vs[k]
                    _, u, vv = S.wedges[ws[k]]
                    if isl[k] == I:
                        wk = ws[k]
                    else:
                        wk = S.add_wedge(dup(v, isl[k], I), u, vv)
                    poly.append((wk, abs(d[k]) <= EPS))
                if (dk < -EPS and dk2 > EPS) or (dk > EPS and dk2 < -EPS):
                    key = (I, min(vs[k], vs[k2]), max(vs[k], vs[k2]))
                    if key in isect:
                        xv = isect[key]
                    else:
                        t = d[k] / (d[k] - d[k2])
                        xp = lerp(S.pos[vs[k]], S.pos[vs[k2]], t)
                        inside = k if dk < 0 else k2     # endpoint on I's side
                        bones, weights = S.binds[vs[inside]]
                        xv = S.add_vertex(xp, bones, weights)
                        isect[key] = xv
                    _, u1, v1 = S.wedges[ws[k]]
                    _, u2, v2 = S.wedges[ws[k2]]
                    t = d[k] / (d[k] - d[k2])
                    poly.append((S.add_wedge(xv, u1 + (u2 - u1) * t, v1 + (v2 - v1) * t), True))
            if len(poly) < 3:
                continue
            for i in range(1, len(poly) - 1):
                new_faces.append((poly[0][0], poly[i][0], poly[i + 1][0], mat))
            for i in range(len(poly)):
                p0, p1 = poly[i], poly[(i + 1) % len(poly)]
                if p0[1] and p1[1]:
                    count_edge((pid, I), S.wedges[p0[0]][0], S.wedges[p1[0]][0])
    S.faces = new_faces

    # Stats for callers (tools/gore_cuts.py): live rim edges per group.
    S.stats = dict(moved=moved, seam=n_seam, planar=n_planar, fallback=n_fallback,
                   rim={g: [e for e, c in edges.items() if c == 1] for g, edges in rim.items()})
    if getattr(S, "no_caps", False):
        S.splits += 1
        return island

    # 3. Caps.
    n_caps = 0
    for group, edges in rim.items():
        live = [e for e, c in edges.items() if c == 1]
        if not live:
            continue
        I = group[1]
        if group[0] == "dup":
            s = fallback_sum.get(group)
            if not s or s[1] == 0:
                continue
            apex = mul(s[0], 1.0 / s[1])
            R = FALLBACK_CAP_SCALE * S.radius_perp(I) + FALLBACK_CAP_MARGIN
            live = [e for e in live if length(sub(S.pos[e[0]], apex)) < R and length(sub(S.pos[e[1]], apex)) < R]
        else:
            pts = set()
            for e in live:
                pts.update(e)
            apex = mul((sum(S.pos[p][0] for p in pts), sum(S.pos[p][1] for p in pts), sum(S.pos[p][2] for p in pts)),
                       1.0 / len(pts))
        bone = S.nearest_in_island(island, I, apex)
        if group[0] != "dup" and group[0].startswith("j"):
            c = int(group[0][1:])
            bone = c if I == c else (PARENT[c] if island[PARENT[c]] == I else bone)
        av = S.add_rigid(apex, bone)
        for a, b in live:
            if group[0] == "dup":
                # Fallback rims are keyed by the ORIGINAL foreign vertices; fan their copies in I.
                if (a, I) not in dup_map or (b, I) not in dup_map:
                    continue
                a, b = dup_map[(a, I)], dup_map[(b, I)]
            S.faces.append((S.add_wedge(a, 8, 248), S.add_wedge(b, 248, 248), S.add_wedge(av, 128, 8), S.cap_mat))
            n_caps += 1
    S.splits += 1
    if verbose:
        print("split #%d: moved=%d seam=%d planar=%d fallback=%d caps=%d verts %d->%d faces %d->%d" % (
            S.splits, moved, n_seam, n_planar, n_fallback, n_caps, nv0, len(S.pos), nf0, len(S.faces)))
    return island


def fragment_body(S, body, K, rng, verbose=True):
    """Split the skin bound to 'body' into K chunks with K-1 random planes."""
    V = [v for v in range(len(S.pos)) if S.binds[v][0][0] == body and S.binds[v][1][0] > 0]
    if len(V) < K * 6:
        return 0
    label = {v: 0 for v in V}
    chunks = {0: list(V)}
    planes = []
    for k in range(1, K):
        big = max(chunks, key=lambda c: len(chunks[c]))
        cv = chunks[big]
        ctr = mul((sum(S.pos[v][0] for v in cv), sum(S.pos[v][1] for v in cv), sum(S.pos[v][2] for v in cv)), 1.0 / len(cv))
        ok = False
        for _ in range(12):
            n = norm((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))
            ext = max(abs(dot(sub(S.pos[v], ctr), n)) for v in cv)
            a = add(ctr, mul(n, rng.uniform(-0.3, 0.3) * ext))
            pos = [v for v in cv if dot(sub(S.pos[v], a), n) > 0]
            if 6 <= len(pos) <= len(cv) - 6:
                ok = True
                break
        if not ok:
            break
        for v in pos:
            label[v] = k
        chunks[big] = [v for v in cv if label[v] != k]
        chunks[k] = pos
        planes.append((a, n))
        S.frag_planes.append((a, n))
    made = 0
    for k in range(1, K):
        if k not in chunks:
            continue
        cv = chunks[k]
        mn = [min(S.pos[v][i] for v in cv) for i in range(3)]
        mx = [max(S.pos[v][i] for v in cv) for i in range(3)]
        c = tuple((mn[i] + mx[i]) * 0.5 for i in range(3))
        h = [max((mx[i] - mn[i]) * 0.5 * 0.8, 1.5) for i in range(3)]
        S.extra_center.append(c)
        S.extra_radius.append(max(h) * 1.5)
        bone = BONE_COUNT + len(S.extra_center) - 1
        for v in cv:
            S.binds[v] = ([bone, bone, bone], [1.0, 0.0, 0.0])
        made += 1
    for v in chunks.get(0, []):
        S.binds[v] = ([body, body, body], [1.0, 0.0, 0.0])
    if verbose:
        print("fragment body=%d K=%d made=%d planes=%d" % (body, K, made, len(planes)))
    return made


# --------------------------------------------------------------------------- export
def write_obj(S, path, explode=0.0):
    island = [S.island_root(b) if S.bone_alive(b) else -1 for b in range(BONE_COUNT + 24)]
    # Vertex -> island (by dominant bone after the split).
    visl = [island[S.binds[v][0][0]] for v in range(len(S.pos))]
    # Explode offset per island: cut children move along their own cut normal,
    # fragments away from the body they were split from, everything else away
    # from the pelvis, so every cap faces a gap.
    offs = {}
    if explode > 0:
        cen = {}
        for v, I in enumerate(visl):
            s = cen.setdefault(I, [(0.0, 0.0, 0.0), 0])
            s[0] = add(s[0], S.pos[v]); s[1] += 1
        for I, (s, n) in cen.items():
            if I == 0:
                offs[I] = (0.0, 0.0, 0.0)
            elif I in S.joint_planes:
                offs[I] = mul(S.joint_planes[I][1], explode)
            elif I >= BONE_COUNT:
                offs[I] = mul(norm(sub(S.extra_center[I - BONE_COUNT], (0.0, 0.0, 10.0))), explode)
            else:
                offs[I] = mul(norm(mul(s, 1.0 / n)), explode)
    groups = {}
    for f in S.faces:
        v0 = S.wedges[f[0]][0]
        I = visl[v0]
        key = ("cap_%d" % I) if f[3] == S.cap_mat else ("Bone_%d_%s" % (I, NAMES[I] if I < BONE_COUNT else "frag"))
        groups.setdefault(key, []).append(f)
    with open(path, "w") as fp:
        fp.write("# plane_cut.py export\n")
        for v, p in enumerate(S.pos):
            o = offs.get(visl[v], (0.0, 0.0, 0.0))
            fp.write("v %.4f %.4f %.4f\n" % (p[0] + o[0], p[1] + o[1], p[2] + o[2]))
        for key in sorted(groups):
            fp.write("o %s\n" % key)
            for f in groups[key]:
                fp.write("f %d %d %d\n" % tuple(S.wedges[f[k]][0] + 1 for k in range(3)))
    print("wrote", path, "objects:", ", ".join(sorted(groups)))


def check_mesh(S):
    """Sanity: every wedge/vertex index in range, no degenerate faces, caps planar."""
    bad = 0
    for f in S.faces:
        vs = [S.wedges[f[k]][0] for k in range(3)]
        if any(v < 0 or v >= len(S.pos) for v in vs):
            bad += 1
        elif len(set(vs)) < 3:
            bad += 1
    print("check: faces=%d bad=%d verts=%d wedges=%d" % (len(S.faces), bad, len(S.pos), len(S.wedges)))
    return bad == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sel")
    ap.add_argument("--header", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "RagdollPkg", "Src", "RagdollDatasets.h"))
    ap.add_argument("--cut", type=int, nargs="*", default=[], help="child bodies whose joint is cut (6=ForearmL, 9=ThighR ...)")
    ap.add_argument("--gib", action="store_true", help="cut every joint")
    ap.add_argument("--frag", nargs="*", default=[], help="body:K fragmentation after the first split")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--explode", type=float, default=0.0)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    ds = load_dataset(args.header, args.sel)
    print("%s: verts=%d faces=%d wedges=%d" % (args.sel, len(ds["verts"]), len(ds["faces"]), len(ds["wedges"])))
    S = State(ds)
    cuts = list(range(1, 12)) if args.gib else args.cut
    for c in cuts:
        S.cut[c] = True
    split_islands(S)
    if args.frag:
        rng = random.Random(args.seed)
        for spec in args.frag:
            body, K = (int(x) for x in spec.split(":"))
            fragment_body(S, body, K, rng)
        split_islands(S)
    check_mesh(S)
    write_obj(S, args.out, args.explode)


if __name__ == "__main__":
    main()
