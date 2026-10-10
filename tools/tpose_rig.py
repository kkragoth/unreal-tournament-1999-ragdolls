"""Offline T-pose + Blender auto-rig for the UT99 Selection viewer meshes.

Pipeline per model (no Mixamo round trip):
  1. extract frame 0 of the Selection mesh, yaw-correct to a front-facing
     bind space (same math as build_rig.py);
  2. segment the surface into arm-L / arm-R / rest by seeded geodesic
     Voronoi on the mesh graph: seeds come from the chest-band cross-section
     connectivity (trunk ring vs arm rings), hands are recovered through the
     forearm by surface distance, feet are never arm candidates;
  3. swing each arm rigidly about its shoulder pivot into a horizontal
     T-pose (smooth geodesic falloff at the armpit seam);
  4. derive a humanoid armature (mixamorig bone names) from the T-posed
     segments and let Blender bone-heat skin it (tools/blender_autorig.py);
  5. rotate the arm bones back down and write a rig JSON in the
     fbx_to_json.py format (verts/faces+uvs/bones/weights) with the ORIGINAL
     arms-down vertices + the T-pose heat weights, ready for
     build_rig_mixamo.py.

Usage:
    python tools/tpose_rig.py <Botpack.u> [--models SelectionMale1,...]
                              [--out-dir tools/rigs] [--blender <exe>]
                              [--scratch dir] [--stats-only]
"""

import argparse
import heapq
import json
import math
import os
import shutil
import statistics as st
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_mesh import UTPackage, parse_lod_mesh, find_meshes, mesh_to_world  # noqa: E402
import build_rig as br  # noqa: E402

MODEL_MAP = br.MODEL_MAP
DEFAULT_BLENDER = r"C:\Program Files\Blender Foundation\Blender 3.6\blender.exe"


# ----------------------------------------------------------------------------
# 1) extraction + yaw correction
# ----------------------------------------------------------------------------
def load_bind(pkg, sel_name, frame=0):
    """Return dict(verts, faces, wedges, wfaces, mesh) in front-facing bind
    space. verts are dense (wedge-referenced only), faces index dense verts,
    wedges carry (dense_vert, u8, v8), wfaces index wedges."""
    idx = None
    for i, e, cn in find_meshes(pkg):
        if pkg.name(e.object_name) == sel_name and cn == "Core.LodMesh":
            idx = i
            break
    if idx is None:
        raise RuntimeError(f"mesh {sel_name} not found")
    m = parse_lod_mesh(pkg, idx)
    used = sorted({w[0] + m.special_verts for w in m.wedges})
    world = [mesh_to_world(m.verts[frame * m.frame_verts + k], m) for k in used]
    remap = {old: new for new, old in enumerate(used)}
    ctr = (st.mean(v[0] for v in world), st.mean(v[1] for v in world))
    cen = [(v[0] - ctr[0], v[1] - ctr[1], v[2]) for v in world]
    mir, yaw = br.find_yaw(cen)
    yaw, _ = br._pick_facing(br.rotz(cen, yaw), yaw, br.rotz(cen, yaw + 90))
    bind = br.rotz(cen, yaw)
    wedges = [(remap[w[0] + m.special_verts], w[1], w[2]) for w in m.wedges]
    faces = [tuple(wedges[fa[k]][0] for k in range(3)) for fa in m.faces]
    return {"verts": bind, "faces": faces, "wedges": wedges,
            "wfaces": [tuple(fa[:3]) for fa in m.faces],
            "fmat": [fa[3] for fa in m.faces], "mesh": m,
            "yaw": yaw, "mirror": mir}


def subdivide(d):
    """One level of 1:4 midpoint subdivision (shape-preserving: no smoothing,
    the UT silhouette stays as authored). Edge midpoints are shared through
    the dense vertex graph; each face gets its own three mid-wedges with the
    averaged corner UVs, so UV seams survive. 4x faces, ~4x verts."""
    V = list(d["verts"])
    wedges = list(d["wedges"])
    wfaces, fmat = d["wfaces"], d["fmat"]
    edge_mid = {}
    new_wfaces, new_fmat = [], []
    for fi, (w0, w1, w2) in enumerate(wfaces):
        ws = (w0, w1, w2)
        mids = []
        for k in range(3):
            a, b = ws[k], ws[(k + 1) % 3]
            va, vb = wedges[a][0], wedges[b][0]
            key = (min(va, vb), max(va, vb))
            if key not in edge_mid:
                edge_mid[key] = len(V)
                V.append(tuple((V[va][j] + V[vb][j]) / 2.0 for j in range(3)))
            u = int(round((wedges[a][1] + wedges[b][1]) / 2.0))
            v = int(round((wedges[a][2] + wedges[b][2]) / 2.0))
            wedges.append((edge_mid[key], u, v))
            mids.append(len(wedges) - 1)
        m0, m1, m2 = mids
        for tri in ((w0, m0, m2), (m0, w1, m1), (m2, m1, w2), (m0, m1, m2)):
            new_wfaces.append(tri)
            new_fmat.append(fmat[fi])
    out = dict(d)
    out["verts"] = V
    out["wedges"] = wedges
    out["wfaces"] = new_wfaces
    out["fmat"] = new_fmat
    out["faces"] = [tuple(wedges[w][0] for w in fa) for fa in new_wfaces]
    return out


# ----------------------------------------------------------------------------
# 2) graph helpers
# ----------------------------------------------------------------------------
def adjacency(nv, faces):
    adj = [set() for _ in range(nv)]
    for a, b, c in faces:
        adj[a].update((b, c))
        adj[b].update((a, c))
        adj[c].update((a, b))
    return adj


def multi_dijkstra(seeds, adj, pts, allowed=None):
    """Multi-source Dijkstra. seeds: {vert: label}. Returns (dist, label)."""
    dist = [math.inf] * len(adj)
    lab = [None] * len(adj)
    pq = []
    for v, l in seeds.items():
        dist[v] = 0.0
        lab[v] = l
        pq.append((0.0, v))
    heapq.heapify(pq)
    while pq:
        d, u = heapq.heappop(pq)
        if d > dist[u]:
            continue
        for v in adj[u]:
            if allowed is not None and not allowed[v]:
                continue
            nd = d + math.dist(pts[u], pts[v])
            if nd < dist[v]:
                dist[v] = nd
                lab[v] = lab[u]
                heapq.heappush(pq, (nd, v))
    return dist, lab


def components(adj):
    comp = [-1] * len(adj)
    n = 0
    for s in range(len(adj)):
        if comp[s] >= 0:
            continue
        comp[s] = n
        stack = [s]
        while stack:
            u = stack.pop()
            for v in adj[u]:
                if comp[v] < 0:
                    comp[v] = n
                    stack.append(v)
        n += 1
    return comp, n


def _band_components(verts, adj, lo, hi):
    band = [i for i, v in enumerate(verts) if lo <= v[2] <= hi]
    bs, seen, comps = set(band), set(), []
    for s0 in band:
        if s0 in seen:
            continue
        stack, c = [s0], []
        seen.add(s0)
        while stack:
            u = stack.pop()
            c.append(u)
            for w in adj[u]:
                if w in bs and w not in seen:
                    seen.add(w)
                    stack.append(w)
        comps.append(c)
    return comps


# ----------------------------------------------------------------------------
# 3) segmentation: arms vs rest
# ----------------------------------------------------------------------------
def find_arm_seeds(verts, adj, floor, H):
    """Return (seeds{vert: -1|0|1}, thr, how).

    Preferred: slide a chest-height band (0.14H tall) from 0.50H up to
    0.80H; the first band whose induced subgraph splits into a central torso
    component plus one lateral component per side gives topological seeds
    (whole arm rings vs the trunk). thr = midpoint of the torso/arm |x| gap.

    Fallback (arms touching the trunk in every band): per-slab |x| gap -
    for each 4 UU slab in the zone take the widest |x| gap between 5 UU and
    14 UU; thr = median of the slab gap midpoints."""
    zlo = floor + 0.50 * H
    k = 0.0
    while zlo + k + 0.14 * H <= floor + 0.80 * H:
        lo, hi = zlo + k, zlo + k + 0.14 * H
        comps = [c for c in _band_components(verts, adj, lo, hi) if len(c) >= 6]
        cx = [st.mean(verts[i][0] for i in c) for c in comps]
        cen = [c for c, x in zip(comps, cx) if abs(x) < 4.0]
        latL = [c for c, x in zip(comps, cx) if x < -8.0]
        latR = [c for c, x in zip(comps, cx) if x > 8.0]
        if cen and latL and latR:
            torso = max(cen, key=len)
            armL = max(latL, key=len)
            armR = max(latR, key=len)
            tmax = max(abs(verts[i][0]) for i in torso)
            amin = min(min(abs(verts[i][0]) for i in armL),
                       min(abs(verts[i][0]) for i in armR))
            thr = (tmax + amin) / 2.0 if amin > tmax else amin
            seeds = {}
            for i in torso:
                seeds[i] = 0
            for i in armL:
                seeds[i] = -1
            for i in armR:
                seeds[i] = 1
            anchors = set(armL) | set(armR)
            _seed_upper(verts, seeds, floor, H, thr, hi)
            return seeds, thr, anchors, f"band {lo:.1f}..{hi:.1f} torso<={tmax:.1f} arm>={amin:.1f}"
        k += 0.02 * H
    mids = []
    z = zlo
    while z < floor + 0.78 * H:
        xs = sorted(abs(v[0]) for v in verts if z <= v[2] < z + 4.0)
        best, mid = 0.0, None
        for a, b in zip(xs, xs[1:]):
            if a >= 5.0 and b <= 14.0 and b - a > best:
                best, mid = b - a, (a + b) / 2.0
        if mid is not None and best > 1.0:
            mids.append(mid)
        z += 4.0
    thr = st.median(mids) if mids else 11.0
    seeds = {}
    _seed_upper(verts, seeds, floor, H, thr, zlo)
    anchors = {i for i, l in seeds.items() if l != 0}
    return seeds, thr, anchors, f"slab-gap thr={thr:.1f} from {len(mids)} slabs"


def _seed_upper(verts, seeds, floor, H, thr, z_from):
    """Above the separating band the chest/arm |x| gap is reliable (thr came
    from the band): everything lateral up to the shoulder line is arm, the
    tighter central column is trunk. Without this the geodesic Voronoi cuts
    the arm off mid-biceps (forearm seeds are farther from the deltoid than
    the shoulder-top trunk seeds)."""
    for i, v in enumerate(verts):
        if i in seeds or not (z_from <= v[2] <= floor + 0.80 * H):
            continue
        ax = abs(v[0])
        if ax > thr + 0.5:
            seeds[i] = -1 if v[0] < 0 else 1
        elif ax < thr - 3.0:
            seeds[i] = 0


def segment_arms(verts, faces):
    """Return (side[], info): side = -1 (left arm), +1 (right arm), 0 (rest)."""
    nv = len(verts)
    adj = adjacency(nv, faces)
    comp, ncomp = components(adj)
    mn, mx = br.bbox(verts)
    floor, H, W = mn[2], mx[2] - mn[2], mx[0] - mn[0]
    seeds, thr, anchors, how = find_arm_seeds(verts, adj, floor, H)

    # Rest seeds outside the chest band: head, feet (any width), trunk core
    # and leg cores. Hands are NOT seeded - they are reached through the
    # forearm by surface distance, never through the thigh.
    for i, v in enumerate(verts):
        z, ax = v[2], abs(v[0])
        if i in seeds:
            continue
        if z > floor + 0.86 * H:
            seeds[i] = 0
        elif z < floor + 0.05 * H:
            seeds[i] = 0
        elif floor + 0.40 * H <= z <= floor + 0.80 * H and ax < thr - 2.0:
            seeds[i] = 0
        elif z < floor + 0.40 * H and ax < thr - 3.5:
            seeds[i] = 0
    dist, lab = multi_dijkstra(seeds, adj, verts)

    # Components never reached by any seed (floating accessories): take the
    # label of the nearest labelled vertex.
    side = [lab[i] for i in range(nv)]
    labelled = [i for i in range(nv) if side[i] is not None]
    for i in range(nv):
        if side[i] is None:
            j = min(labelled, key=lambda k: math.dist(verts[i], verts[k]))
            side[i] = side[j]

    # An arm label must be connected to an arm ring: lateral verts of a
    # component that holds no anchor (e.g. the Boss's wide chest plates) go
    # back to the trunk unless their whole component is arm-only (a floating
    # arm accessory).
    comp_anchor = {comp[i] for i in anchors}
    comp_rest = {comp[i] for i in range(nv) if side[i] == 0}
    for i in range(nv):
        if side[i] != 0 and comp[i] not in comp_anchor and comp[i] in comp_rest:
            side[i] = 0

    # Two majority passes to clean single flips at the seam.
    for _ in range(2):
        upd = []
        for v in range(nv):
            nb = [side[u] for u in adj[v]]
            if len(nb) >= 3:
                win = st.mode(nb)
                if win != side[v] and nb.count(win) >= len(nb) // 2 + 1:
                    upd.append((v, win))
        for v, w in upd:
            side[v] = w

    info = {"floor": floor, "H": H, "W": W, "thr": thr, "how": how,
            "ncomp": ncomp,
            "n_seed_arm": sum(1 for s in seeds.values() if s != 0),
            "nL": side.count(-1), "nR": side.count(1), "adj": adj, "comp": comp}
    return side, info


# ----------------------------------------------------------------------------
# 4) T-pose
# ----------------------------------------------------------------------------
def rodrigues(axis, ang):
    x, y, z = axis
    c, s, C = math.cos(ang), math.sin(ang), 1 - math.cos(ang)
    return ((c + x * x * C, x * y * C - z * s, x * z * C + y * s),
            (y * x * C + z * s, c + y * y * C, y * z * C - x * s),
            (z * x * C - y * s, z * y * C + x * s, c + z * z * C))


def mat_apply(M, p):
    return tuple(M[r][0] * p[0] + M[r][1] * p[1] + M[r][2] * p[2] for r in range(3))


def rot_about(M, pivot, p):
    rel = tuple(p[k] - pivot[k] for k in range(3))
    r = mat_apply(M, rel)
    return tuple(pivot[k] + r[k] for k in range(3))


def norm(v):
    l = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / l for c in v)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def tpose(verts, side, adj, falloff=5.0):
    """Swing both arms to horizontal. Returns (new_verts, arms{side: spec}).

    Pivot = shoulder seam centroid (arm verts touching the trunk) when the
    arm is attached, else the arm's root cap (top 12% of its height, armor
    sheets). Pivots are mirror-averaged so both arms swing identically. Tip =
    geodesically farthest arm verts from the root cap (works for detached
    arms too). spec: pivot, axis, angle, seam, per-vertex blend weights."""
    nv = len(verts)
    out = list(verts)
    arms = {}
    pre = {}
    for s in (-1, 1):
        A = [i for i in range(nv) if side[i] == s]
        if len(A) < 10:
            continue
        seam = [i for i in A if any(side[u] != s for u in adj[i])]
        ztop = max(verts[i][2] for i in A)
        zbot = min(verts[i][2] for i in A)
        root = [i for i in A if verts[i][2] >= ztop - 0.12 * (ztop - zbot)]
        if len(seam) >= 5:
            pivot = tuple(st.mean(verts[i][k] for i in seam) for k in range(3))
        else:
            pivot = tuple(st.mean(verts[i][k] for i in root) for k in range(3))
            pivot = (pivot[0], pivot[1], pivot[2] - 2.0)
        allowed = [side[i] == s for i in range(nv)]
        droot, _ = multi_dijkstra({i: 0 for i in root}, adj, verts, allowed)
        reach = sorted((i for i in A if droot[i] != math.inf), key=lambda i: -droot[i])
        tip = reach[:max(3, len(reach) // 7)]
        tipc = tuple(st.mean(verts[i][k] for i in tip) for k in range(3))
        pre[s] = [A, seam, pivot, tipc]
    if len(pre) == 2:
        pL, pR = pre[-1][2], pre[1][2]
        ax = (abs(pL[0]) + abs(pR[0])) / 2.0
        ay = (pL[1] + pR[1]) / 2.0
        az = (pL[2] + pR[2]) / 2.0
        pre[-1][2] = (-ax, ay, az)
        pre[1][2] = (ax, ay, az)
    for s, (A, seam, pivot, tipc) in pre.items():
        d = norm(tuple(tipc[k] - pivot[k] for k in range(3)))
        t = (float(s), 0.0, 0.0)
        axis = cross(d, t)
        sa = math.sqrt(sum(c * c for c in axis))
        ca = max(-1.0, min(1.0, sum(d[k] * t[k] for k in range(3))))
        ang = math.atan2(sa, ca)
        axis = norm(axis) if sa > 1e-6 else (0.0, 1.0, 0.0)
        if len(seam) >= 5:
            allowed = [side[i] == s for i in range(nv)]
            dseam, _ = multi_dijkstra({i: 0 for i in seam}, adj, verts, allowed)
        else:
            dseam = [math.inf] * nv
        blend = {}
        for i in A:
            w = 1.0 if dseam[i] == math.inf else min(1.0, dseam[i] / falloff)
            w = w * w * (3 - 2 * w)                      # smoothstep
            blend[i] = w
            out[i] = rot_about(rodrigues(axis, ang * w), pivot, verts[i])
        arms[s] = {"pivot": pivot, "axis": axis, "angle": ang, "seam": seam,
                   "blend": blend, "tip": tipc, "dir": d,
                   "len": math.dist(pivot, tipc)}
    return out, arms


# ----------------------------------------------------------------------------
# 5) armature from the T-posed segments
# ----------------------------------------------------------------------------
def derive_armature(T, side, floor, H, thr, arms):
    """Return a list of bones [{name, head, tail, parent}] in T-pose space.

    Bone names follow Mixamo so build_rig_mixamo.MIXAMO_MAP applies as-is.
    Trunk joints come from the trunk's slab centroids at height fractions;
    leg joints from per-side leg slab centroids; arm joints from slab
    centroids along the horizontal arm at fixed fractions of its length."""
    nv = len(T)
    rest = [i for i in range(nv) if side[i] == 0]

    def cen(idxs, fb):
        if not idxs:
            return fb
        return tuple(st.mean(T[i][k] for i in idxs) for k in range(3))

    def trunk_y(zc, half=3.0):
        sl = [i for i in rest if abs(T[i][2] - zc) <= half and abs(T[i][0]) <= thr]
        return st.mean(T[i][1] for i in sl) if sl else 0.0

    # Crotch: lowest rest vert near the mid-plane above the knees.
    mid = [i for i in rest if abs(T[i][0]) < 2.5 and T[i][2] > floor + 0.2 * H]
    crotch_z = min(T[i][2] for i in mid) if mid else floor + 0.45 * H
    # Legs stay separate below the crotch; the PELVIS band itself is the
    # proven 0.42..0.54H window (a spread stance pushes the mid-plane low
    # point down the inner thighs, well below the real pelvis).
    legs_top = crotch_z
    pelvis_bot = max(floor + 0.42 * H, min(crotch_z, floor + 0.50 * H))
    pelvis_top = pelvis_bot + 0.11 * H
    hip_z = pelvis_bot + 0.04 * H
    legs = {s: [i for i in rest if T[i][2] < hip_z and (T[i][0] < 0) == (s < 0)]
            for s in (-1, 1)}

    neck_z = floor + 0.80 * H
    if arms:
        pz = st.mean(a["pivot"][2] for a in arms.values())
        neck_z = max(floor + 0.78 * H, min(pz + 1.5, floor + 0.84 * H))
    head_z = max(neck_z + 0.03 * H, floor + 0.855 * H)
    zs = sorted(T[i][2] for i in rest)
    crown_z = zs[int(0.985 * (len(zs) - 1))]

    bones = []

    def add(name, head, tail, parent):
        bones.append({"name": name, "head": tuple(head), "tail": tuple(tail),
                      "parent": parent})

    # trunk. Hips spans the whole pelvis (crotch -> lower belly) so the heat
    # solver gives the pelvis body its own verts instead of handing them to
    # the spine/thighs; the spine chain starts above it.
    sp = [pelvis_top + (neck_z - pelvis_top) * f for f in (0.0, 0.33, 0.66, 1.0)]
    add("mixamorig:Hips", (0, trunk_y(pelvis_bot), pelvis_bot - 1.0), (0, trunk_y(pelvis_top), pelvis_top), None)
    add("mixamorig:Spine", (0, trunk_y(sp[0]), sp[0]), (0, trunk_y(sp[1]), sp[1]), "mixamorig:Hips")
    add("mixamorig:Spine1", (0, trunk_y(sp[1]), sp[1]), (0, trunk_y(sp[2]), sp[2]), "mixamorig:Spine")
    add("mixamorig:Spine2", (0, trunk_y(sp[2]), sp[2]), (0, trunk_y(sp[3]), sp[3]), "mixamorig:Spine1")
    add("mixamorig:Neck", (0, trunk_y(neck_z), neck_z), (0, trunk_y(head_z), head_z), "mixamorig:Spine2")
    hy = trunk_y(head_z + 0.5 * (crown_z - head_z), half=6.0)
    add("mixamorig:Head", (0, trunk_y(head_z), head_z), (0, hy, crown_z), "mixamorig:Neck")
    add("mixamorig:HeadTop_End", (0, hy, crown_z), (0, hy, crown_z + 3.0), "mixamorig:Head")

    # legs
    for s, L, R in ((-1, "Left", "L"), (1, "Right", "R")):
        lv = legs[s]
        # Hip joint sits above the thigh column: take the upper-thigh slab
        # (clear of the crotch/pelvis-front verts that sit near x=0 and would
        # drag a plain band mean toward the mid-plane) and lift it to hip_z.
        th = cen([i for i in lv if hip_z - 0.12 * H <= T[i][2] <= hip_z - 0.04 * H],
                 (s * 0.12 * H, trunk_y(hip_z), hip_z))
        hip = (th[0] if abs(th[0]) >= 3.0 else s * max(3.0, abs(th[0])), th[1], hip_z)
        knee_z = floor + 0.31 * H
        knee = cen([i for i in lv if abs(T[i][2] - knee_z) <= 3.0], (hip[0], hip[1], knee_z))
        knee = (knee[0], knee[1], knee_z)
        ank_z = floor + 0.07 * H
        ank = cen([i for i in lv if ank_z - 2.0 <= T[i][2] <= ank_z + 3.0], (knee[0], knee[1], ank_z))
        ank = (ank[0], ank[1], ank_z)
        foot = [i for i in lv if T[i][2] < floor + 0.04 * H]
        fy = sorted(T[i][1] for i in foot) if foot else [ank[1]]
        toe_y = fy[int(0.8 * (len(fy) - 1))]
        toe = cen([i for i in foot if T[i][1] >= toe_y], (ank[0], ank[1] + 6.0, floor + 1.0))
        toe = (toe[0], toe[1], floor + 1.0)
        add(f"mixamorig:{L}UpLeg", hip, knee, "mixamorig:Hips")
        add(f"mixamorig:{L}Leg", knee, ank, f"mixamorig:{L}UpLeg")
        add(f"mixamorig:{L}Foot", ank, toe, f"mixamorig:{L}Leg")
        add(f"mixamorig:{L}ToeBase", toe, (toe[0], toe[1] + 3.0, toe[2]), f"mixamorig:{L}Foot")

    # arms (horizontal, along +-X from the pivot)
    for s, L in ((-1, "Left"), (1, "Right")):
        a = arms.get(s)
        if not a:
            continue
        A = [i for i in range(nv) if side[i] == s]
        piv, ln = a["pivot"], a["len"]

        def along(f, half=2.5):
            x = piv[0] + s * ln * f
            sl = [i for i in A if abs(T[i][0] - x) <= half]
            c = cen(sl, (x, piv[1], piv[2]))
            return (x, c[1], c[2])
        elbow, wrist, tip = along(0.47), along(0.76), along(0.97, 3.0)
        clav = (s * 2.0, trunk_y(piv[2]), piv[2] + 1.0)
        add(f"mixamorig:{L}Shoulder", clav, piv, "mixamorig:Spine2")
        add(f"mixamorig:{L}Arm", piv, elbow, f"mixamorig:{L}Shoulder")
        add(f"mixamorig:{L}ForeArm", elbow, wrist, f"mixamorig:{L}Arm")
        add(f"mixamorig:{L}Hand", wrist, tip, f"mixamorig:{L}ForeArm")
    return bones


ARM_CHAIN = ("Arm", "ForeArm", "Hand")


def unpose_bones(bones, arms):
    """Rotate the arm chain bones back to the arms-down bind pose."""
    out = []
    for b in bones:
        nb = dict(b)
        for s, L in ((-1, "Left"), (1, "Right")):
            a = arms.get(s)
            if a and any(b["name"] == f"mixamorig:{L}{n}" for n in ARM_CHAIN):
                M = rodrigues(a["axis"], -a["angle"])
                nb["head"] = rot_about(M, a["pivot"], b["head"])
                nb["tail"] = rot_about(M, a["pivot"], b["tail"])
        out.append(nb)
    return out


def seg_dist(p, pa, pb):
    return br._seg_dist(p, pa, pb)


def fill_weights(weights, verts, bones, side):
    """Give unweighted verts inverse-distance weights to the two nearest
    bone segments of their own segment (arm side / rest)."""
    segs = []
    for b in bones:
        n = b["name"]
        if "Left" in n:
            bs = -1
        elif "Right" in n:
            bs = 1
        else:
            bs = 0
        is_arm = any(n.endswith(x) for x in ("Shoulder",) + ARM_CHAIN)
        segs.append((n, b["head"], b["tail"], bs if is_arm else 0))
    filled = 0
    for i, w in enumerate(weights):
        if w:
            continue
        cand = [(seg_dist(verts[i], h, t) + 0.5, n) for n, h, t, bs in segs if bs == side[i]]
        cand.sort()
        top = cand[:2]
        tot = sum(1.0 / d ** 2 for d, _ in top)
        weights[i] = {n: (1.0 / d ** 2) / tot for d, n in top}
        filled += 1
    return filled


# ----------------------------------------------------------------------------
# 6) I/O
# ----------------------------------------------------------------------------
def write_obj(path, verts, wedges, wfaces, groups=None):
    """OBJ with shared positions + per-wedge UVs. groups: vert->name; faces
    are grouped into objects by the majority group of their corners."""
    with open(path, "w") as f:
        for p in verts:
            f.write(f"v {p[0]:.4f} {p[1]:.4f} {p[2]:.4f}\n")
        for w in wedges:
            f.write(f"vt {w[1] / 255.0:.4f} {1.0 - w[2] / 255.0:.4f}\n")
        order = list(range(len(wfaces)))

        def fgroup(fi):
            return st.mode([groups[wedges[wi][0]] for wi in wfaces[fi]])
        if groups:
            order.sort(key=fgroup)
        cur = None
        for fi in order:
            fa = wfaces[fi]
            if groups:
                g = fgroup(fi)
                if g != cur:
                    f.write(f"o {g}\n")
                    cur = g
            f.write("f " + " ".join(f"{wedges[wi][0] + 1}/{wi + 1}" for wi in fa) + "\n")


def write_skeleton_obj(path, bones, r=0.6):
    """Bones as thin octahedra so renders show the armature."""
    with open(path, "w") as f:
        n = 0
        for b in bones:
            h, t = b["head"], b["tail"]
            d = norm(tuple(t[k] - h[k] for k in range(3)))
            u = norm(cross(d, (0, 0, 1) if abs(d[2]) < 0.9 else (1, 0, 0)))
            v = cross(d, u)
            m = tuple(h[k] + (t[k] - h[k]) * 0.2 for k in range(3))
            ring = [tuple(m[k] + r * (u[k] * cx + v[k] * cy) for k in range(3))
                    for cx, cy in ((1, 0), (0, 1), (-1, 0), (0, -1))]
            f.write(f"o Skel_{b['name'].replace('mixamorig:', '')}\n")
            for p in [h] + ring + [t]:
                f.write(f"v {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}\n")
            o = n + 1
            for i in range(4):
                a, c = o + 1 + i, o + 1 + (i + 1) % 4
                f.write(f"f {o} {a} {c}\nf {o + 5} {c} {a}\n")
            n += 6


def dominant_groups(weights, verts):
    """vert -> 'Bone_<ragdoll idx>' by dominant mapped weight (for renders)."""
    import build_rig_mixamo as brm
    out = {}
    for i, w in enumerate(weights):
        agg = {}
        for n, wt in w.items():
            b = brm.MIXAMO_MAP.get(n)
            if b is None:
                continue
            agg[b] = agg.get(b, 0.0) + wt
        out[i] = f"Bone_{max(agg, key=agg.get) if agg else 0}_x"
    return out


# ----------------------------------------------------------------------------
# 7) driver
# ----------------------------------------------------------------------------
def process(pkg, sel, args, blender):
    d = load_bind(pkg, sel)
    for _ in range(max(0, args.subdivide)):
        d = subdivide(d)
    V, F = d["verts"], d["faces"]
    side, info = segment_arms(V, F)
    print(f"\n== {sel}: {len(V)}v {len(F)}f yaw={d['yaw']:+d} H={info['H']:.1f} "
          f"W={info['W']:.1f} floor={info['floor']:.1f} comps={info['ncomp']} "
          f"seeds[{info['how']}] armseeds={info['n_seed_arm']} "
          f"L={info['nL']} R={info['nR']}")
    for s, nm in ((-1, "L"), (1, "R")):
        A = [V[i] for i in range(len(V)) if side[i] == s]
        if A:
            mn, mx = br.bbox(A)
            print(f"   arm{nm}: x {mn[0]:.1f}..{mx[0]:.1f} y {mn[1]:.1f}..{mx[1]:.1f} "
                  f"z {mn[2]:.1f}..{mx[2]:.1f}")
    T, arms = tpose(V, side, info["adj"])
    for s, a in arms.items():
        print(f"   pivot{s:+d}={tuple(round(x, 1) for x in a['pivot'])} "
              f"ang={math.degrees(a['angle']):.1f} len={a['len']:.1f} seam={len(a['seam'])}")
    grp = {i: ("armL" if side[i] == -1 else "armR" if side[i] == 1 else "rest")
           for i in range(len(V))}
    sc = args.scratch
    write_obj(os.path.join(sc, f"{sel}_seg.obj"), V, d["wedges"], d["wfaces"], grp)
    write_obj(os.path.join(sc, f"{sel}_tpose.obj"), T, d["wedges"], d["wfaces"], grp)

    bones = derive_armature(T, side, info["floor"], info["H"], info["thr"], arms)
    write_skeleton_obj(os.path.join(sc, f"{sel}_tpose_skel.obj"), bones)
    if args.stats_only:
        return

    # Blender bone-heat skinning in the T-pose.
    rig_in = os.path.join(sc, f"{sel}_autorig_in.json")
    rig_out = os.path.join(sc, f"{sel}_autorig_out.json")
    with open(rig_in, "w") as f:
        json.dump({"name": sel, "verts": T, "faces": F, "bones": bones}, f)
    cmd = [blender, "-b", "--python-exit-code", "1", "-P",
           os.path.join(os.path.dirname(os.path.abspath(__file__)), "blender_autorig.py"),
           "--", os.path.abspath(rig_in), os.path.abspath(rig_out)]
    if args.save_blend:
        cmd.append(os.path.abspath(os.path.join(sc, f"{sel}_tpose_rig.blend")))
    r = subprocess.run(cmd, capture_output=True, text=True)
    tail = [l for l in r.stdout.splitlines() if "autorig" in l or "failed" in l.lower()]
    print("   blender:", "; ".join(tail[-3:]) or r.stderr[-400:])
    if r.returncode != 0 or not os.path.exists(rig_out):
        print(r.stdout[-1500:], r.stderr[-1500:])
        raise RuntimeError(f"{sel}: blender autorig failed")
    with open(rig_out) as f:
        ro = json.load(f)
    weights = ro["weights"]
    nf = fill_weights(weights, T, bones, side)
    print(f"   weights: method={ro['method']} unweighted={len(ro['unweighted'])} filled={nf}")
    # keep top-4 per vert, renormalised
    for i, w in enumerate(weights):
        top = sorted(w.items(), key=lambda kv: -kv[1])[:4]
        tot = sum(v for _, v in top) or 1.0
        weights[i] = {k: v / tot for k, v in top}

    # Hand check: verts that were segmented as arm must not be weighted to
    # leg bones, and leg verts must not be weighted to arm bones.
    leak = 0
    for i, w in enumerate(weights):
        armw = sum(v for k, v in w.items() if any(k.endswith(x) for x in ARM_CHAIN + ("Shoulder",)))
        legw = sum(v for k, v in w.items() if any(x in k for x in ("UpLeg", "Leg", "Foot", "Toe")))
        if side[i] != 0 and legw > 0.25:
            leak += 1
        if side[i] == 0 and V[i][2] < info["floor"] + 0.45 * info["H"] and armw > 0.25:
            leak += 1
    print(f"   arm<->leg weight leaks: {leak}")

    # Un-pose: arm chain bones back down; verts are the ORIGINAL bind verts.
    bones_down = unpose_bones(bones, arms)
    faces_out = []
    for fi, wf in enumerate(d["wfaces"]):
        faces_out.append({"verts": [d["wedges"][wi][0] for wi in wf],
                          "uvs": [[d["wedges"][wi][1] / 255.0, d["wedges"][wi][2] / 255.0] for wi in wf],
                          "mat": d["fmat"][fi]})
    rig = {"verts": [list(v) for v in V], "faces": faces_out, "bones": bones_down,
           "weights": weights,
           "materials": [list(mm) for mm in d["mesh"].materials],
           "source": {"tool": "tools/tpose_rig.py", "sel": sel, "yaw": d["yaw"],
                      "subdivide": args.subdivide,
                      "tpose": {str(s): {"pivot": a["pivot"], "axis": a["axis"],
                                         "angle": a["angle"]} for s, a in arms.items()}}}
    os.makedirs(args.out_dir, exist_ok=True)
    out = os.path.join(args.out_dir, f"{sel}.json")
    with open(out, "w") as f:
        json.dump(rig, f)
    print(f"   wrote {out}")

    # Render inputs: T-pose + skeleton, and arms-down mesh coloured by the
    # dominant ragdoll bone.
    write_skeleton_obj(os.path.join(sc, f"{sel}_down_skel.obj"), bones_down)
    write_obj(os.path.join(sc, f"{sel}_bones.obj"), V, d["wedges"], d["wfaces"],
              dominant_groups(weights, V))
    write_obj(os.path.join(sc, f"{sel}_tpose_bones.obj"), T, d["wedges"], d["wfaces"],
              dominant_groups(weights, T))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg")
    ap.add_argument("--models", default=",".join(s for _, s in MODEL_MAP))
    ap.add_argument("--out-dir", default="tools/rigs")
    ap.add_argument("--scratch", default="tools/rig_verify")
    ap.add_argument("--blender", default=None)
    ap.add_argument("--save-blend", action="store_true")
    ap.add_argument("--subdivide", type=int, default=1,
                    help="levels of 1:4 midpoint subdivision before rigging "
                         "(default 1: ~4x verts so bends have verts to blend over; 0 = source mesh)")
    ap.add_argument("--stats-only", action="store_true")
    args = ap.parse_args(argv)
    blender = args.blender or os.environ.get("BLENDER_EXE") or shutil.which("blender") \
        or DEFAULT_BLENDER
    pkg = UTPackage(args.pkg)
    os.makedirs(args.scratch, exist_ok=True)
    for sel in args.models.split(","):
        process(pkg, sel, args, blender)


if __name__ == "__main__":
    main(sys.argv[1:])
