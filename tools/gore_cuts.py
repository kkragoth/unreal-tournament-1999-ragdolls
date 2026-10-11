"""Cut sites for the dismemberment system (docs/plans/gore-variation.md).

For every severable bone of every dataset in RagdollDatasets.h this sweeps a
cut plane along the bone axis

    A(t) = Anchor + t * (DistalEnd - Anchor),  N = normalised (DistalEnd - Anchor)

and runs the SAME vertex classification + seam clip the DLL runs
(plane_cut.split_islands, kept in lock-step with RagdollSplitIslands). A t is
CLEAN when no seam face needs the duplicated-skirt fallback and each side's
rim is a single closed loop. The longest clean run per bone, with the
capture band / radius that made it clean, goes to
RagdollPkg/Src/RagdollCutSites.h (GRagdollCutSites[], looked up by dataset
name). Hand overrides: tools/rigs/<Sel>.cuts.json (see OVERRIDE_KEYS).

    python tools/gore_cuts.py --all --header RagdollPkg/Src/RagdollCutSites.h
    python tools/gore_cuts.py SelectionMale1 --bone 6 --t 0.5 --obj tools/rig_verify/cut_m1_fore.obj --explode 6
    python tools/gore_cuts.py SelectionMale1 --sweep            (print the table only)

The --obj export also builds the MODELLED stump caps (meat dish + bone
stub, same construction as RagdollBuildModelledCap in RagdollGore.cpp) as
"cap_*" / "bone_*" objects so they can be rendered with
tools/render_groups.py or imported into Blender.
"""

import argparse
import json
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import plane_cut as pc  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
DATASETS_H = os.path.join(REPO, "RagdollPkg", "Src", "RagdollDatasets.h")
RIGS_DIR = os.path.join(HERE, "rigs")

# Live-mesh -> dataset names, as GRagdollDatasetTable.
DATASETS = ["SelectionMale1", "SelectionMale2", "SelectionBoss", "SelectionFemale2", "SelectionFemale1"]

# Bones that can be severed (child body of the cut joint). Feet have no body
# on the real-mesh rig; the pelvis is the root.
SEVERABLE = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
# Distal end of a bone's axis = its child's anchor; terminal bones mirror the
# anchor through the box centre.
CHILD_OF = {1: 2, 2: 3, 4: 6, 5: 7, 8: 10, 9: 11, 10: 12, 11: 13}
T_STEP = 0.05
T_MAX = {1: 0.60, 2: 0.70, 3: 0.35, 4: 0.75, 5: 0.75, 6: 0.80, 7: 0.80, 8: 0.75, 9: 0.75, 10: 0.80, 11: 0.80}
BANDS = [0.0, 2.0, 4.0, 6.0, 8.0]
RADIUS_MULT = [1.0, 1.5, 2.2]
OVERRIDE_KEYS = ("t_min", "t_max", "band", "radius", "disabled")


# --------------------------------------------------------------------------- geometry
def bone_axis(ds, c):
    A = ds["anchors"][c - 1]
    if c in CHILD_OF:
        E = ds["anchors"][CHILD_OF[c] - 1]
    else:
        b = ds["bones"][c]
        C = (b[3], b[4], b[5])
        E = pc.add(C, pc.sub(C, A))
    return A, E


def trace_loops(edges):
    """Ordered vertex loops from a set of (a, b) edges. Returns [(verts, closed)]."""
    adj = {}
    for a, b in edges:
        adj.setdefault(a, []).append(b)
        adj.setdefault(b, []).append(a)
    unused = set((min(a, b), max(a, b)) for a, b in edges)
    loops = []
    while unused:
        a, b = next(iter(unused))
        # start at an odd vertex when there is one (open chain), else anywhere
        start = a
        for v in adj:
            if len(adj[v]) % 2 == 1 and any((min(v, w), max(v, w)) in unused for w in adj[v]):
                start = v
                break
        path = [start]
        cur, closed = start, False
        while True:
            nxt = None
            for w in adj.get(cur, []):
                if (min(cur, w), max(cur, w)) in unused:
                    nxt = w
                    break
            if nxt is None:
                break
            unused.discard((min(cur, nxt), max(cur, nxt)))
            if nxt == start:
                closed = True
                break
            path.append(nxt)
            cur = nxt
        loops.append((path, closed))
    return loops


def loop_stats(S, loop, n):
    pts = [S.pos[v] for v in loop]
    C = pc.mul((sum(p[0] for p in pts), sum(p[1] for p in pts), sum(p[2] for p in pts)), 1.0 / len(pts))
    R = sum(pc.length(pc.sub(p, C)) for p in pts) / len(pts)
    Rmax = max(pc.length(pc.sub(p, C)) for p in pts)
    return C, R, Rmax


# --------------------------------------------------------------------------- evaluation
def evaluate(ds, c, t, band, radius, caps=False, stub=None, seed=1):
    """Run one cut of bone c at parameter t. Returns (State, info)."""
    S = pc.State(ds)
    S.no_caps = not caps
    A, E = bone_axis(ds, c)
    P = pc.lerp(A, E, t)
    N = pc.norm(pc.sub(E, A))
    S.cut[c] = True
    S.joint_planes[c] = (P, N, band, radius)
    island = pc.split_islands(S, verbose=False)
    st = S.stats
    par_isl = island[pc.PARENT[c]]
    gid = "j%d" % c
    info = dict(t=t, band=band, radius=radius, fallback=st["fallback"], seam=st["seam"], planar=st["planar"],
                moved=st["moved"], loops={}, clean=False, rimR=0.0, rimRmax=0.0, P=P, N=N)
    ok = st["fallback"] == 0
    for side, I in (("child", c), ("parent", par_isl)):
        edges = st["rim"].get((gid, I), [])
        loops = trace_loops(edges)
        loops.sort(key=lambda l: -len(l[0]))
        info["loops"][side] = [(len(l[0]), l[1]) for l in loops]
        if not loops or not loops[0][1] or len(loops[0][0]) < 6:
            ok = False
        # Secondary loops (fused accessories, a hand welded to a thigh) are
        # tolerated when they are small closed rings: the DLL caps each loop
        # separately, a small one with its own centroid fan.
        for lp, closed in loops[1:]:
            if not closed or len(lp) > 20:
                ok = False
        if loops:
            C, R, Rmax = loop_stats(S, loops[0][0], N)
            info["rimR"] = max(info["rimR"], R)
            info["rimRmax"] = max(info["rimRmax"], Rmax)
            info["loop_" + side] = loops[0][0]
    info["clean"] = ok
    if caps:
        build_caps(S, c, par_isl, info, island, stub=stub, rng=random.Random(seed))
    return S, info


def sweep_bone(ds, c, overrides=None, verbose=False):
    """Find the longest clean t run for bone c. Returns a site dict or None."""
    ov = (overrides or {}).get(str(c), {})
    if ov.get("disabled"):
        return None
    box_r = max(ds["bones"][c][0], ds["bones"][c][1])
    tmax = ov.get("t_max", T_MAX[c])
    tmin_ov = ov.get("t_min", 0.0)
    ts = []
    t = 0.0
    while t <= tmax + 1e-6:
        if t >= tmin_ov - 1e-6:
            ts.append(round(t, 3))
        t += T_STEP
    per_t = {}
    for t in ts:
        best = None
        combos = ([(ov["band"], ov["radius"])] if "band" in ov and "radius" in ov
                  else [(b, box_r * m) for b in BANDS for m in RADIUS_MULT])
        for band, radius in combos:
            _, info = evaluate(ds, c, t, band, radius)
            if info["clean"]:
                best = info
                break
            if best is None or (info["fallback"], sum(n for n, _ in info["loops"].get("child", [])[1:])) < \
                    (best["fallback"], sum(n for n, _ in best["loops"].get("child", [])[1:])):
                best = dict(info, clean=False)
        per_t[t] = best
        if verbose:
            print("  bone %2d %-10s t=%.2f clean=%d fallback=%2d band=%.0f R=%.1f rimR=%.1f loops=%s" % (
                c, pc.NAMES[c], t, best["clean"], best["fallback"], best["band"], best["radius"], best["rimR"],
                {k: v[:3] for k, v in best["loops"].items()}))
    # longest clean run
    runs, cur = [], []
    for t in ts:
        if per_t[t]["clean"]:
            cur.append(t)
        else:
            if cur:
                runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    if not runs:
        return None
    run = max(runs, key=len)
    band = max(per_t[t]["band"] for t in run)
    radius = max(per_t[t]["radius"] for t in run)
    # Re-validate the run with the unified parameters; keep the clean core.
    good = []
    for t in run:
        _, info = evaluate(ds, c, t, band, radius)
        if info["clean"]:
            good.append((t, info))
    if not good:
        return None
    # Longest contiguous subset of 'good'.
    best_sub, sub = [], []
    for t, info in good:
        if sub and abs(t - sub[-1][0] - T_STEP) > 1e-3:
            if len(sub) > len(best_sub):
                best_sub = sub
            sub = []
        sub.append((t, info))
    if len(sub) > len(best_sub):
        best_sub = sub
    A, E = bone_axis(ds, c)
    site = dict(bone=c, name=pc.NAMES[c], A=A, E=E, t_min=best_sub[0][0], t_max=best_sub[-1][0], band=band, radius=radius,
                rimR=max(i["rimR"] for _, i in best_sub), rimRmax=max(i["rimRmax"] for _, i in best_sub))
    if "t_min" in ov:
        site["t_min"] = max(site["t_min"], ov["t_min"])
    return site


# --------------------------------------------------------------------------- modelled caps
# Mirror of RagdollBuildModelledCap. All sizes in UU (DrawScale=1).
CAP_SEGMENTS = 8          # bone cylinder segments
CAP_INNER_K = 0.62        # inner (dish) ring radius / rim radius
CAP_DISH_DEPTH = 0.30     # inner ring recess / rim radius
CAP_FLOOR_DEPTH = 0.42    # bone base recess / rim radius
CAP_BONE_K = 0.24         # bone radius / rim radius
CAP_BONE_MIN, CAP_BONE_MAX = 0.8, 2.4
# Wound atlas bands (V in UT 0..255): meat 0..127 (light -> dark), clot 128..191, bone 192..255.
UV_MEAT_LIGHT, UV_MEAT_DARK, UV_CLOT, UV_BONE0, UV_BONE1, UV_BONE_TIP = 20, 100, 170, 198, 250, 222


def ring_frame(N):
    up = (0.0, 0.0, 1.0) if abs(N[2]) < 0.9 else (1.0, 0.0, 0.0)
    X = pc.norm(pc.sub(up, pc.mul(N, pc.dot(up, N))))
    Y = (N[1] * X[2] - N[2] * X[1], N[2] * X[0] - N[0] * X[2], N[0] * X[1] - N[1] * X[0])
    return X, Y


def order_loop(S, loop, C, Nout):
    """Sort the rim loop by angle around Nout (counter-clockwise seen from outside)."""
    X, Y = ring_frame(Nout)
    def ang(v):
        r = pc.sub(S.pos[v], C)
        return math.atan2(pc.dot(r, Y), pc.dot(r, X))
    return sorted(loop, key=ang), X, Y


def bridge(S, ringA, ringB, C, X, Y, mat, uvA, uvB, faces, flip=False):
    """Triangulate between two rings (vertex id lists sorted by angle). u = angle."""
    def ang(v):
        r = pc.sub(S.pos[v], C)
        a = math.atan2(pc.dot(r, Y), pc.dot(r, X))
        return a if a >= 0 else a + 2 * math.pi
    A = [(ang(v), v) for v in ringA]
    B = [(ang(v), v) for v in ringB]
    A.sort(); B.sort()
    def u_of(a): return a / (2 * math.pi) * 255.0
    i = j = 0
    na, nb = len(A), len(B)
    # merge-walk around the circle
    steps = na + nb
    for _ in range(steps):
        a0, va = A[i % na]; a1, va1 = A[(i + 1) % na]
        b0, vb = B[j % nb]; b1, vb1 = B[(j + 1) % nb]
        # unwrap so angles increase monotonically
        a1u = a1 if a1 > a0 else a1 + 2 * math.pi
        b1u = b1 if b1 > b0 else b1 + 2 * math.pi
        if (a1u <= b1u and i < na) or j >= nb:
            tri = (va, va1, vb); uv = ((u_of(a0), uvA), (u_of(a1u), uvA), (u_of(b0), uvB))
            i += 1
        else:
            tri = (va, vb1, vb); uv = ((u_of(a0), uvA), (u_of(b1u), uvB), (u_of(b0), uvB))
            j += 1
        if flip:
            tri = (tri[0], tri[2], tri[1]); uv = (uv[0], uv[2], uv[1])
        if len(set(tri)) < 3:
            continue
        ws = [S.add_wedge(tri[k], min(uv[k][0], 255), uv[k][1]) for k in range(3)]
        faces.append((ws[0], ws[1], ws[2], mat))


# "Meaty" cap (user, 2026-10-11, after the first Blender renders: "make more
# like meaty parts"): a ragged torn LIP outside the skin rim, two lumpy inner
# rings (muscle bundles at alternating depth), a few hanging flesh FLAPS,
# and a fatter bone with a knobbed end.
CAP_LIP_OUT = 0.9         # lip ring pushed out along Nout (UU) ...
CAP_LIP_JITTER = 0.25     # ... and radially jittered by +-this x rim radius
CAP_RING1_K, CAP_RING1_D = 0.70, 0.12    # first inner ring: radius / recess (x rim radius)
CAP_RING2_K, CAP_RING2_D = 0.42, 0.30    # second inner ring
CAP_LUMP = 0.10           # alternate ring verts bulge out / sink in by this x rim radius
CAP_FLAPS = (2, 4)        # hanging flesh flaps per cap
CAP_FLAP_LEN = (2.0, 4.5) # UU
CAP_KNOB = 1.35           # bone end radius / shaft radius


def build_cap(S, loop, Nout, bone, mat, stub_len, rounded, rng, tag, bone_faces):
    """Meaty cap (torn lip + lumpy dish + flaps) and bone stub over an ordered
    planar rim loop. Nout points out of the island."""
    pts = [S.pos[v] for v in loop]
    C = pc.mul((sum(p[0] for p in pts), sum(p[1] for p in pts), sum(p[2] for p in pts)), 1.0 / len(pts))
    R = sum(pc.length(pc.sub(p, C)) for p in pts) / len(pts)
    loop, X, Y = order_loop(S, loop, C, Nout)
    n = len(loop)

    def ring(k_rad, depth, lump, jitter):
        out = []
        for i, v in enumerate(loop):
            r = pc.sub(S.pos[v], C)
            kk = k_rad * (1.0 + rng.uniform(-jitter, jitter))
            dz = -depth * R + (lump * R if i % 2 == 0 else -lump * R * 0.5)
            out.append(S.add_rigid(pc.add(C, pc.add(pc.mul(r, kk), pc.mul(Nout, dz))), bone))
        return out
    # Torn lip: slightly outside the skin, ragged.
    lip = ring(1.0, -CAP_LIP_OUT / max(R, 1.0), 0.0, CAP_LIP_JITTER)
    ring1 = ring(CAP_RING1_K, CAP_RING1_D, CAP_LUMP, 0.08)
    ring2 = ring(CAP_RING2_K, CAP_RING2_D, CAP_LUMP * 0.7, 0.10)
    rb = min(max(CAP_BONE_K * R * 1.15, CAP_BONE_MIN), CAP_BONE_MAX)
    base = []
    for k in range(CAP_SEGMENTS):
        a = 2 * math.pi * k / CAP_SEGMENTS
        d = pc.add(pc.mul(X, math.cos(a) * rb), pc.mul(Y, math.sin(a) * rb))
        base.append(S.add_rigid(pc.add(pc.sub(C, pc.mul(Nout, CAP_FLOOR_DEPTH * R)), d), bone))
    faces = []
    bridge(S, loop, lip, C, X, Y, mat, UV_CLOT, UV_CLOT - 20, faces)        # skin rim -> lip (clot)
    bridge(S, lip, ring1, C, X, Y, mat, UV_CLOT - 20, UV_MEAT_DARK, faces)  # lip -> bundles (dark meat)
    bridge(S, ring1, ring2, C, X, Y, mat, UV_MEAT_DARK, UV_MEAT_LIGHT + 30, faces)
    bridge(S, ring2, base, C, X, Y, mat, UV_MEAT_LIGHT + 30, UV_MEAT_LIGHT, faces)
    # Hanging flaps: a few rim edges extruded outward and drooped (-Z).
    nflaps = rng.randint(*CAP_FLAPS)
    for _ in range(nflaps):
        i = rng.randrange(n)
        va, vb = loop[i], loop[(i + 1) % n]
        L = rng.uniform(*CAP_FLAP_LEN)
        mid = pc.mul(pc.add(S.pos[va], S.pos[vb]), 0.5)
        tipp = pc.add(pc.add(mid, pc.mul(Nout, L * 0.6)), (0.0, 0.0, -L * 0.7))
        tipp = pc.add(tipp, pc.mul(pc.norm(pc.sub(mid, C)), L * 0.3))   # flare outward
        vt = S.add_rigid(tipp, bone)
        faces.append((S.add_wedge(va, 40, UV_CLOT), S.add_wedge(vb, 90, UV_CLOT), S.add_wedge(vt, 65, UV_MEAT_DARK), mat))
    # Bone shaft from the floor to the tip; the tip ring is jittered (broken
    # shaft) or widened into a knob (bone head), then a small fan to the tip.
    tipc = pc.add(C, pc.mul(Nout, stub_len))
    tip, knob = [], []
    for k in range(CAP_SEGMENTS):
        a = 2 * math.pi * k / CAP_SEGMENTS
        if rounded:
            rr, dz = rb * CAP_KNOB, 0.0
        else:
            rr = rb * rng.uniform(0.75, 1.15)
            dz = rng.uniform(-0.35, 0.35) * max(stub_len, 1.2)
        d = pc.add(pc.mul(X, math.cos(a) * rr), pc.mul(Y, math.sin(a) * rr))
        tip.append(S.add_rigid(pc.add(pc.add(tipc, d), pc.mul(Nout, dz)), bone))
        if stub_len > 1.5 and not rounded:
            # mid-shaft knob (condyle) a little below the break
            dk = pc.add(pc.mul(X, math.cos(a) * rb * CAP_KNOB), pc.mul(Y, math.sin(a) * rb * CAP_KNOB))
            knob.append(S.add_rigid(pc.add(pc.add(tipc, dk), pc.mul(Nout, -0.45 * stub_len)), bone))
    bf = []
    def tube(ra, rbv, v0, v1):
        for k in range(CAP_SEGMENTS):
            k2 = (k + 1) % CAP_SEGMENTS
            u0, u1 = 255.0 * k / CAP_SEGMENTS, 255.0 * (k + 1) / CAP_SEGMENTS
            w = [S.add_wedge(ra[k], u0, v0), S.add_wedge(ra[k2], u1, v0),
                 S.add_wedge(rbv[k2], u1, v1), S.add_wedge(rbv[k], u0, v1)]
            bf.append((w[0], w[2], w[1], mat))
            bf.append((w[0], w[3], w[2], mat))
    if knob:
        tube(base, knob, UV_BONE0, UV_BONE0 + 25)
        tube(knob, tip, UV_BONE0 + 25, UV_BONE1)
    else:
        tube(base, tip, UV_BONE0, UV_BONE1)
    tc = S.add_rigid(pc.add(tipc, pc.mul(Nout, rb * 0.7 if rounded else rng.uniform(0.0, 0.3))), bone)
    for k in range(CAP_SEGMENTS):
        k2 = (k + 1) % CAP_SEGMENTS
        bf.append((S.add_wedge(tip[k], 100, UV_BONE_TIP), S.add_wedge(tc, 128, UV_BONE_TIP + 20),
                   S.add_wedge(tip[k2], 156, UV_BONE_TIP), mat))
    S.faces.extend(faces)
    S.faces.extend(bf)
    bone_faces[tag] = bf
    return len(faces) + len(bf)


def fit_template(S, loop, Nout, bone, mat, T, stub_scale, rng, tag, bone_faces):
    """Place a cap template (tools/cap_templates.py) over an ordered planar rim
    loop: the template's rim ring is put ON the loop polygon (polar radius of
    the loop at each ring angle), interior vertices follow that correction
    weighted by their own radius, all scaled by the loop's mean radius, then
    the loop is stitched to the ring. Mirror of BuildTemplateCap (C++)."""
    import cap_templates as ct
    pts = [S.pos[v] for v in loop]
    C = pc.mul((sum(p[0] for p in pts), sum(p[1] for p in pts), sum(p[2] for p in pts)), 1.0 / len(pts))
    R = max(sum(pc.length(pc.sub(p, C)) for p in pts) / len(pts), 1.0)
    loop, X, Y = order_loop(S, loop, C, Nout)
    n = len(loop)
    ang = []
    for v in loop:
        r = pc.sub(S.pos[v], C)
        a = math.atan2(pc.dot(r, Y), pc.dot(r, X))
        ang.append((a if a >= 0 else a + 2 * math.pi, pc.length(r)))
    ang.sort()

    def rho(theta):
        theta %= 2 * math.pi
        for i in range(n):
            a0, r0 = ang[i]
            a1, r1 = ang[(i + 1) % n]
            if a1 < a0:
                a1 += 2 * math.pi
            tt = theta if theta >= a0 else theta + 2 * math.pi
            if a0 <= tt <= a1:
                f = (tt - a0) / (a1 - a0) if a1 > a0 else 0.0
                return r0 + (r1 - r0) * f
        return ang[0][1]
    phi = rng.uniform(0, 2 * math.pi)       # random spin of the template about the axis
    bv = ct.bone_verts(T)
    ids = []
    for vi, (x, y, z) in enumerate(T.v):
        cx, cy = x * math.cos(phi) - y * math.sin(phi), x * math.sin(phi) + y * math.cos(phi)
        r = math.hypot(cx, cy)
        th = math.atan2(cy, cx)
        rr = rho(th) / R
        k = 1.0 + (rr - 1.0) * min(r, 1.0)
        zz = z * (stub_scale if vi in bv else 1.0)
        p = pc.add(C, pc.add(pc.add(pc.mul(X, cx * k * R), pc.mul(Y, cy * k * R)), pc.mul(Nout, zz * R)))
        ids.append(S.add_rigid(p, bone))
    faces, bf = [], []
    for g in ("meat", "bone"):
        for f in T.faces[g]:
            ws = [S.add_wedge(ids[vi], T.vt[ti][0] * 255.0, T.vt[ti][1] * 255.0) for vi, ti in f]
            (bf if g == "bone" else faces).append((ws[0], ws[1], ws[2], mat))
    # Stitch: loop -> template rim ring (ids[0..RIM_M-1]), both on the rim polygon.
    ring = ids[:ct.RIM_M]
    bridge(S, loop, ring, C, X, Y, mat, UV_CLOT, UV_CLOT, faces)
    S.faces.extend(faces)
    S.faces.extend(bf)
    bone_faces[tag] = bf
    return len(faces) + len(bf)


def build_caps(S, c, par_isl, info, island, stub=None, rng=None):
    """Caps for both sides of the cut of bone c (after split_islands with no_caps)."""
    import cap_templates as ct
    rng = rng or random.Random(1)
    tpls = dict(ct.load_templates())
    N = info["N"]
    S.bone_faces = getattr(S, "bone_faces", {})
    # Stump (parent side) protrudes the broken shaft with 70% odds, else the limb does.
    stub_on_parent = (rng.random() < 1.0) if stub is None else (stub == "parent")
    joint_level = info["t"] < 0.12
    stub_scale = rng.uniform(0.8, 1.4)
    n = 0
    for side, I, Nout in (("child", c, pc.mul(N, -1.0)), ("parent", par_isl, N)):
        loop = info.get("loop_" + side)
        if not loop:
            continue
        bone = c if side == "child" else (pc.PARENT[c] if island[pc.PARENT[c]] == I else S.nearest_in_island(island, I, info["P"]))
        if joint_level:
            variant = "head" if side == "child" else "socket"
        else:
            variant = "shaft" if ((side == "parent") == stub_on_parent) else "flush"
        n += fit_template(S, loop, Nout, bone, S.cap_mat, tpls[variant], stub_scale, rng, "%s_%d" % (side, I), S.bone_faces)
    info["cap_faces"] = n
    return n


# --------------------------------------------------------------------------- export
def write_obj(S, path, explode=0.0, bone_faces=None):
    island = [S.island_root(b) if S.bone_alive(b) else -1 for b in range(pc.BONE_COUNT + 24)]
    visl = [island[S.binds[v][0][0]] for v in range(len(S.pos))]
    offs = {}
    if explode > 0:
        for I in set(visl):
            if I in S.joint_planes:
                offs[I] = pc.mul(S.joint_planes[I][1], explode)
            else:
                offs[I] = (0.0, 0.0, 0.0)
    bone_set = set()
    for bf in (bone_faces or {}).values():
        bone_set.update(id(f) for f in bf)
    bone_tuples = set()
    for bf in (bone_faces or {}).values():
        bone_tuples.update(bf)
    groups = {}
    for f in S.faces:
        v0 = S.wedges[f[0]][0]
        I = visl[v0]
        if f[3] == S.cap_mat:
            key = ("bone_%d" % I) if f in bone_tuples else ("cap_%d" % I)
        else:
            key = "Bone_%d_%s" % (I, pc.NAMES[I] if I < pc.BONE_COUNT else "frag")
        groups.setdefault(key, []).append(f)
    with open(path, "w") as fp:
        fp.write("# gore_cuts.py export\n")
        for v, p in enumerate(S.pos):
            o = offs.get(visl[v], (0.0, 0.0, 0.0))
            fp.write("v %.4f %.4f %.4f\n" % (p[0] + o[0], p[1] + o[1], p[2] + o[2]))
        for w in S.wedges:
            fp.write("vt %.4f %.4f\n" % (w[1] / 255.0, 1.0 - w[2] / 255.0))
        for key in sorted(groups):
            fp.write("o %s\n" % key)
            for f in groups[key]:
                fp.write("f %d/%d %d/%d %d/%d\n" % (S.wedges[f[0]][0] + 1, f[0] + 1, S.wedges[f[1]][0] + 1, f[1] + 1,
                                                    S.wedges[f[2]][0] + 1, f[2] + 1))
    print("wrote", path, "objects:", ", ".join(sorted(groups)))


def fmt(v):
    return "FVector( %.3ff, %.3ff, %.3ff )" % tuple(v)


def emit_header(all_sites, path):
    lines = ["// Generated by tools/gore_cuts.py. DO NOT EDIT (hand overrides: tools/rigs/<Sel>.cuts.json).",
             "// Per-model cut sites: for each severable bone the clean range of the cut",
             "// plane A(t) = A + t (E - A) along the bone, the capture band / radius",
             "// the seam clipper uses (FRagdollCutPlane W / R) and the rim radius.",
             "// TMin/TMax = -1: no clean site, the DLL falls back to the heat-seam fit.",
             "// FRagdollCutSite / FRagdollCutSiteTable are declared in RagdollInternal.h.",
             "#ifndef __RAGDOLLCUTSITES_H__", "#define __RAGDOLLCUTSITES_H__", ""]
    lines.append("static const FRagdollCutSiteTable GRagdollCutSites[] =")
    lines.append("{")
    for sel, sites in all_sites:
        lines.append("\t{ TEXT(\"%s\"), {" % sel)
        for b in range(pc.BONE_COUNT):
            s = sites.get(b)
            if s:
                lines.append("\t\t{ %s, %s, %.2ff, %.2ff, %.1ff, %.2ff, %.2ff },   // %s" % (
                    fmt(s["A"]), fmt(s["E"]), s["t_min"], s["t_max"], s["band"], s["radius"], s["rimR"], s["name"]))
            else:
                lines.append("\t\t{ FVector( 0, 0, 0 ), FVector( 0, 0, 1 ), -1.0f, -1.0f, 0.0f, 0.0f, 0.0f },   // %s" % pc.NAMES[b])
        lines.append("\t} },")
    lines.append("};")
    lines.append("static const INT GRagdollCutSiteCount = %d;" % len(all_sites))
    lines.append("")
    lines.append("#endif")
    with open(path, "w") as fp:
        fp.write("\n".join(lines) + "\n")
    print("wrote", path)


def load_overrides(sel):
    p = os.path.join(RIGS_DIR, sel + ".cuts.json")
    if os.path.exists(p):
        with open(p) as fp:
            return json.load(fp)
    return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sel", nargs="?")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--datasets", default=DATASETS_H)
    ap.add_argument("--header", help="write RagdollCutSites.h")
    ap.add_argument("--sweep", action="store_true", help="print the per-t table")
    ap.add_argument("--bone", type=int, nargs="*", default=[])
    ap.add_argument("--t", type=float, default=0.5)
    ap.add_argument("--band", type=float)
    ap.add_argument("--radius", type=float)
    ap.add_argument("--stub", choices=["parent", "child"])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--obj")
    ap.add_argument("--explode", type=float, default=0.0)
    args = ap.parse_args()

    sels = DATASETS if args.all else [args.sel]
    if args.obj:
        ds = pc.load_dataset(args.datasets, args.sel)
        bones = args.bone or [6]
        S = None
        # Several cuts: apply sequentially on one State (planes preset per bone).
        S = pc.State(ds)
        S.no_caps = True
        infos = []
        for c in bones:
            A, E = bone_axis(ds, c)
            P = pc.lerp(A, E, args.t)
            N = pc.norm(pc.sub(E, A))
            box_r = max(ds["bones"][c][0], ds["bones"][c][1])
            S.cut[c] = True
            S.joint_planes[c] = (P, N, args.band if args.band is not None else 4.0,
                                 args.radius if args.radius is not None else box_r * 1.5)
        island = pc.split_islands(S, verbose=True)
        st = S.stats
        rng = random.Random(args.seed)
        S.bone_faces = {}
        for c in bones:
            par_isl = island[pc.PARENT[c]]
            info = dict(t=args.t, P=S.joint_planes[c][0], N=S.joint_planes[c][1], rimR=0.0, loops={})
            for side, I in (("child", c), ("parent", par_isl)):
                loops = trace_loops(st["rim"].get(("j%d" % c, I), []))
                loops.sort(key=lambda l: -len(l[0]))
                print("  bone %d %s side=%s loops=%s" % (c, pc.NAMES[c], side, [(len(l[0]), l[1]) for l in loops]))
                if loops and loops[0][1]:
                    info["loop_" + side] = loops[0][0]
                    C, R, Rmax = loop_stats(S, loops[0][0], info["N"])
                    info["rimR"] = max(info["rimR"], R)
            build_caps(S, c, par_isl, info, island, stub=args.stub, rng=rng)
            print("  caps: %d faces, fallback=%d" % (info.get("cap_faces", 0), st["fallback"]))
        pc.check_mesh(S)
        write_obj(S, args.obj, args.explode, S.bone_faces)
        return

    all_sites = []
    for sel in sels:
        ds = pc.load_dataset(args.datasets, sel)
        ov = load_overrides(sel)
        print("%s: verts=%d faces=%d overrides=%s" % (sel, len(ds["verts"]), len(ds["faces"]), sorted(ov.keys())))
        sites = {}
        for c in (args.bone or SEVERABLE):
            site = sweep_bone(ds, c, ov, verbose=args.sweep)
            if site:
                sites[c] = site
                print("  %-10s t=[%.2f, %.2f] band=%.0f R=%.1f rimR=%.1f (max %.1f) axis %.1f UU" % (
                    site["name"], site["t_min"], site["t_max"], site["band"], site["radius"], site["rimR"], site["rimRmax"],
                    pc.length(pc.sub(site["E"], site["A"]))))
            else:
                print("  %-10s NO CLEAN SITE" % pc.NAMES[c])
        all_sites.append((sel, sites))
    if args.header:
        emit_header(all_sites, args.header)


if __name__ == "__main__":
    main()
