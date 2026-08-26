"""Offline per-model ragdoll rig builder.

For each Selection viewer mesh (frame 0), yaw-corrects the pose to a
front-facing symmetric "bind space", derives the 14 ragdoll bone centers/half
extents from the mesh geometry, computes the 13 joint anchors, nearest-bone
binds every vertex, runs hard assertions, and emits:

  - a C header RagdollPkg\\Src\\RagdollDatasets.h (verts/faces/wedges/materials
    + bone defs + anchors + per-vertex bind), compiled into the DLL;
  - OBJ overlays (mesh + skeleton) for headless Blender verification renders.

Usage:
    python tools/build_rig.py <Botpack.u> [--models SelectionMale1,...] [--out header]
                              [--bind topo|legacy] [--render] [--obj-dir dir]

Binding defaults to --bind topo (connectivity/geodesic segmentation,
docs/plans/topology-rig.md). --bind legacy keeps the old region-band
classifier for A/B comparison.

With --render, Blender renders the overlay to PNGs (needs blender.exe on PATH or
set BLENDER_EXE). Run --render after --obj to verify bone placement visually.

Everything below uses Unreal units (UU), Z up, mirror symmetry about X=0.
"""

import argparse
import heapq
import math
import os
import shutil
import statistics as st
import sys

from extract_mesh import UTPackage, parse_lod_mesh, find_meshes, mesh_to_world

# Live player mesh name -> Selection viewer mesh (arms-down bind pose). UT
# player meshes have NO pose with hip-length arms (every standing frame hangs
# the hands down by the ankles), so we bind the Selection viewer mesh and
# handle the hand/ankle contact explicitly: arm<->leg faces are dropped and
# the foot/ankle region is bound rigidly to the foot bones. (live, selection)
MODEL_MAP = [
    ("Commando", "SelectionMale1"),
    ("Soldier", "SelectionMale2"),
    ("Boss", "SelectionBoss"),
    ("SGirl", "SelectionFemale2"),
    ("FCommando", "SelectionFemale1"),
]

BONE_NAMES = ["Pelvis", "Spine", "Neck", "Head", "UpArmL", "UpArmR",
              "ForArmL", "ForArmR", "ThighL", "ThighR", "ShinL", "ShinR",
              "FootL", "FootR"]

# Adjacent pairs (may interpenetrate); every other pair must not.
ADJACENT = {
    (0, 1), (1, 2), (2, 3), (1, 4), (1, 5),
    (4, 6), (5, 7), (0, 8), (0, 9), (8, 10), (9, 11), (10, 12), (11, 13),
}

# Canonical masses (kg), reused per model (density adapts via box volume).
CANONICAL_MASS = [8.0, 10.0, 2.0, 4.0, 3.0, 3.0, 2.0, 2.0,
                  6.0, 6.0, 3.0, 3.0, 1.0, 1.0]


def rotz(pts, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return [(p[0] * c - p[1] * s, p[0] * s + p[1] * c, p[2]) for p in pts]


def sym_score(pts):
    tot = 0.0
    px = [p[0] for p in pts]
    py = [p[1] for p in pts]
    pz = [p[2] for p in pts]
    for i, p in enumerate(pts):
        best = min((px[j] + p[0]) ** 2 + (py[j] - p[1]) ** 2 + (pz[j] - p[2]) ** 2
                   for j in range(len(pts)) if j != i)
        tot += best ** 0.5
    return tot / len(pts)


def find_yaw(pts):
    best = (1e30, 0)
    for deg in range(-90, 91, 1):
        sc = sym_score(rotz(pts, deg))
        if sc < best[0]:
            best = (sc, deg)
    return best


def bbox(pts):
    mn = [min(v[i] for v in pts) for i in range(3)]
    mx = [max(v[i] for v in pts) for i in range(3)]
    return mn, mx


def derive_bones(verts):
    """Return (bone_centers[14], bone_halves[14], report-dict).

    verts: bind-space frame-0 verts (pelvis not yet translated to origin).
    Region classification in the yaw-corrected frame:
      - head / neck above the neck line
      - torso+pelvis central (|x|<=arm_thr) from pelvis bottom to neck
      - arms at |x|>arm_thr (split at elbow line)
      - legs central below pelvis (thigh/shin/foot by z)
    Bone centers are centroids of the classified verts (bones follow the mesh);
    half extents are fractions of body height H (robust per model).
    """
    mn, mx = bbox(verts)
    H = mx[2] - mn[2]
    foot = mn[2]

    arm_thr = 11.0
    elbow = foot + 0.52 * H
    shoulder_top = foot + 0.84 * H
    hand_bot = foot + 0.06 * H
    pelvis_bot = foot + 0.42 * H
    pelvis_top = foot + 0.54 * H
    neck_bot = foot + 0.80 * H
    head_bot = foot + 0.855 * H
    knee = foot + 0.30 * H
    ankle = foot + 0.06 * H

    head = [v for v in verts if v[2] >= head_bot]
    neck = [v for v in verts if neck_bot <= v[2] < head_bot]
    central = [v for v in verts if abs(v[0]) <= arm_thr]
    pelvis = [v for v in central if pelvis_bot <= v[2] < pelvis_top]
    spine = [v for v in central if pelvis_top <= v[2] < neck_bot]
    legs = [v for v in central if v[2] < pelvis_bot]
    thigh = [v for v in legs if knee <= v[2]]
    shin = [v for v in legs if ankle <= v[2] < knee]
    foot_v = [v for v in legs if v[2] < ankle]

    # Feet are skipped from the ragdoll (a separate foot body scrambles the
    # ankle). Collect their indices so bind_verts can bind them rigidly to the
    # shin of their side.
    foot_set = set(foot_v)
    foot_idx = {i for i, v in enumerate(verts) if v in foot_set}

    arms = [v for v in verts if abs(v[0]) > arm_thr and hand_bot <= v[2] < shoulder_top]
    upper = [v for v in arms if v[2] >= elbow]
    fore = [v for v in arms if v[2] < elbow]

    # Force-assignment for arm-region verts: hands/forearms sit on the arm line
    # (|x| > arm_thr) but the pose has them reaching the ANKLES, so they stick
    # out beyond the forearm's z-band and nearest-box binding loses them to the
    # shin/foot (the classic hand->leg triangle tear). Rebind every vert on the
    # arm column (down to the feet) to its side's upper-arm/forearm bone.
    arms_wide = [v for v in verts if abs(v[0]) > arm_thr and foot - 1.0 <= v[2] < shoulder_top]
    arm_idx = {i for i, v in enumerate(verts) if v in arms_wide}

    def cen(grp):
        if not grp:
            return None
        return (sum(v[0] for v in grp) / len(grp),
                sum(v[1] for v in grp) / len(grp),
                sum(v[2] for v in grp) / len(grp))

    # Arm centers sit on the OUTER arm line (mean |x| of the outer half of the
    # side's arm verts), not the region centroid: the centroid is dragged inward
    # by inner-arm verts and its box then interpenetrates the thigh/shin.
    def outer_x(grp):
        if not grp:
            return None
        xs = sorted(abs(v[0]) for v in grp)
        outer = xs[len(xs) // 2:]
        return sum(outer) / len(outer)

    def arm_cen(grp):
        if not grp:
            return None
        ax = outer_x(grp)
        s = -1.0 if grp[0][0] < 0 else 1.0
        return (s * ax,
                sum(v[1] for v in grp) / len(grp),
                sum(v[2] for v in grp) / len(grp))

    pelvis_c = cen(pelvis)
    spine_c = cen(spine)
    neck_c = cen(neck)
    head_c = cen(head)
    upperL = arm_cen([v for v in upper if v[0] < 0])
    upperR = arm_cen([v for v in upper if v[0] > 0])
    foreL = arm_cen([v for v in fore if v[0] < 0])
    foreR = arm_cen([v for v in fore if v[0] > 0])
    thighL = cen([v for v in thigh if v[0] < 0])
    thighR = cen([v for v in thigh if v[0] > 0])
    shinL = cen([v for v in shin if v[0] < 0])
    shinR = cen([v for v in shin if v[0] > 0])
    footL = cen([v for v in foot_v if v[0] < 0])
    footR = cen([v for v in foot_v if v[0] > 0])

    # Mirror-average the L/R pairs so the physics skeleton is bilaterally
    # symmetric regardless of small pose asymmetries.
    def sym_pair(cl, cr):
        if cl is None or cr is None:
            return None
        ax = (abs(cl[0]) + abs(cr[0])) / 2.0
        ay = (cl[1] + cr[1]) / 2.0
        az = (cl[2] + cr[2]) / 2.0
        return (-ax, ay, az), (ax, ay, az)

    ua = sym_pair(upperL, upperR)
    fa = sym_pair(foreL, foreR)
    th = sym_pair(thighL, thighR)
    sh = sym_pair(shinL, shinR)
    ft = sym_pair(footL, footR)

    centers = [pelvis_c, spine_c, neck_c, head_c,
               ua[0], ua[1], fa[0], fa[1],
               th[0], th[1], sh[0], sh[1], ft[0], ft[1]]
    if any(c is None for c in centers):
        raise RuntimeError("bone derivation produced empty region; bad landmarks")
    # The central column sits on the mirror axis (X=0). The pelvis-band centroid
    # itself can be off-axis (few verts), so force it: it would otherwise drag
    # the whole symmetric skeleton off-axis when the pelvis becomes the origin.
    for i in range(4):
        centers[i] = (0.0, centers[i][1], centers[i][2])

    # Half extents as fractions of H (H ~ 84 UU for males, ~83 females).
    def hz(f): return H * f
    halves = [
        (9.0, 6.0, hz(0.07)),                      # Pelvis
        (7.0, 5.0, hz(0.11)),                      # Spine
        (2.6, 2.6, hz(0.02)),                      # Neck
        (4.2, 4.2, hz(0.045)),                     # Head
        (2.0, 2.0, hz(0.14)), (2.0, 2.0, hz(0.14)),   # Upper arms
        (1.4, 1.4, hz(0.21)), (1.4, 1.4, hz(0.21)),   # Forearms (reach the hands)
        (3.0, 3.0, hz(0.09)), (3.0, 3.0, hz(0.09)),   # Thighs
        (2.4, 2.4, hz(0.12)), (2.4, 2.4, hz(0.12)),   # Shins
        (2.0, 3.0, hz(0.03)), (2.0, 3.0, hz(0.03)),   # Feet (unused)
    ]

    # Feet are skipped from the ragdoll and bound rigidly to the shins, so each
    # shin box must span thigh-bottom -> foot-bottom or the feet clip the floor
    # (the shin centroid is re-centered on that span).
    foot_bottom = mn[2]
    for b in (10, 11):
        knee_top = centers[8][2] - halves[8][2]   # thigh box bottom (same side)
        new_cz = (knee_top + foot_bottom) / 2.0
        new_hz = (knee_top - foot_bottom) / 2.0 + 1.0
        centers[b] = (centers[b][0], centers[b][1], new_cz)
        halves[b] = (halves[b][0], halves[b][1], new_hz)
    # Forearm+hand verts = the arm column BELOW the elbow (the hands hang down
    # to the ankles in the viewer pose). These are the verts the mesh-fold
    # rotates up about the elbow so the hands leave the ankles.
    fore_idx = {i for i, v in enumerate(verts) if abs(v[0]) > arm_thr
                and v[2] < elbow}
    # Leg verts = central (|x|<=arm_thr) below the pelvis: thigh+shin+foot.
    leg_idx = {i for i, v in enumerate(verts) if v in legs}
    # Arm column mean Y per side (elbow pivot y). Mirror-average for symmetry.
    ayL = st.mean(v[1] for v in arms_wide if v[0] < 0)
    ayR = st.mean(v[1] for v in arms_wide if v[0] > 0)
    arm_y = (ayL + ayR) / 2.0
    report = {
        "H": H, "foot": foot, "arm_thr": arm_thr,
        "n_head": len(head), "n_neck": len(neck), "n_pelvis": len(pelvis),
        "n_spine": len(spine), "n_arm": len(arms), "n_leg": len(legs),
        "n_upper": len(upper), "n_fore": len(fore),
        "n_thigh": len(thigh), "n_shin": len(shin), "n_foot": len(foot_v),
        "upper_c": ua, "fore_c": fa, "arm_idx": arm_idx, "foot_idx": foot_idx,
        "fore_idx": fore_idx, "leg_idx": leg_idx, "elbow": elbow, "arm_y": arm_y,
    }
    return centers, halves, report


def compute_anchors(centers):
    """13 joint anchors in the same order as BuildRagdollImpl's AddJoint calls."""
    c = centers
    arm_x = abs(c[4][0])
    arm_y = (c[4][1] + c[6][1]) / 2.0
    thigh_x = abs(c[8][0])
    mz = lambda a, b: (a[2] + b[2]) / 2.0
    shldr_z = c[4][2] + (c[4][2] - c[6][2]) / 2.0  # upper arm top
    hip_z = c[8][2] + (c[8][2] - c[10][2]) / 2.0   # thigh top
    return [
        (0.0, 0.0, mz(c[0], c[1])),                 # pelvis-spine
        (0.0, 0.0, mz(c[1], c[2])),                 # spine-neck
        (0.0, 0.0, mz(c[2], c[3])),                 # neck-head
        (-arm_x, arm_y, shldr_z), (arm_x, arm_y, shldr_z),  # spine-upperarm L/R
        (-arm_x, arm_y, mz(c[4], c[6])), (arm_x, arm_y, mz(c[5], c[7])),  # elbow
        (-thigh_x, 0.0, hip_z), (thigh_x, 0.0, hip_z),   # pelvis-thigh L/R
        (-thigh_x, 0.0, mz(c[8], c[10])), (thigh_x, 0.0, mz(c[9], c[11])),  # knee
        (-thigh_x, 0.0, mz(c[10], c[12])), (thigh_x, 0.0, mz(c[11], c[13])),  # ankle
    ]


def box_dist(p, c, h):
    """Distance from point p to the bone's AABB [c-h, c+h]. Respects the bone's
    length: a hand vert just below a long forearm box is closer to the forearm
    than to the shin's center, so limbs keep their verts (nearest-center binding
    loses that and glues hands to the legs)."""
    dx = max(abs(p[0] - c[0]) - h[0], 0.0)
    dy = max(abs(p[1] - c[1]) - h[1], 0.0)
    dz = max(abs(p[2] - c[2]) - h[2], 0.0)
    return math.sqrt(dx * dx + dy * dy + dz * dz)


def fold_forearms(verts, centers, pivot_y, elbow_z, fore_idx, angle_deg):
    """Fold the forearm+hand verts up about the elbow so the hands leave the
    ankles (the root cause of every lower-body binding problem). Only the MESH
    verts move; the physics skeleton is unchanged, so the elbows stay straight
    (no hinge/limit changes, no joint pop at spawn) and all previous
    interpenetration checks still hold. The folded hands render attached to
    the (straight) forearm bones via the recomputed bind offsets.

    Rotation is about the X axis (medial-lateral) through each side's elbow,
    which swings the hand from pointing down to pointing up; the fold angle is
    the angular travel from the hanging arm. At 180 the hands point straight
    up beside the head (front-agnostic); slightly less folds them forward.
    pivot_y/elbow_z are in the same frame as verts (pelvis translated to
    origin); the per-side X pivot is the elbow line at abs(centers[4][0]).
    """
    arm_x = abs(centers[4][0])
    a = math.radians(angle_deg)
    ca, sa = math.cos(a), math.sin(a)
    out = list(verts)
    for i in fore_idx:
        s = -1.0 if verts[i][0] < 0 else 1.0
        px, py, pz = s * arm_x, pivot_y, elbow_z
        dx, dy, dz = verts[i][0] - px, verts[i][1] - py, verts[i][2] - pz
        dy2 = dy * ca - dz * sa
        dz2 = dy * sa + dz * ca
        out[i] = (verts[i][0], py + dy2, pz + dz2)
    return out


def bind_verts(verts, centers, halves, arm_idx=None, foot_idx=None, fore_idx=None):
    """Rigid 1-bone binding. Each vertex belongs to exactly ONE bone (weight 1)
    so the mesh shows clean single-color regions — the smooth multi-bone
    blending produced noisy 3-color overlap zones in the lower body.

    Arm and leg bones are mutually exclusive candidate groups so the hands
    (which hang against the legs in the viewer pose) never attach to the legs
    and the thighs never attach to the arms. Foot verts are bound rigidly to
    the shin of their side (feet are skipped from the ragdoll). Forearm+hand
    verts (folded up about the elbow) are bound rigidly to the forearm bone of
    their side — nearest-box would lose them to the head/neck once folded.
    """
    LEG = {8, 9, 10, 11, 12, 13}
    CORE = {0, 1, 2, 3, 8, 9, 10, 11}   # torso+head+thigh+shin (no feet, no arms)
    binds = []
    for i, p in enumerate(verts):
        if foot_idx and i in foot_idx:
            bone = 10 if p[0] < 0 else 11
        elif fore_idx and i in fore_idx:
            bone = 6 if p[0] < 0 else 7
        elif arm_idx and i in arm_idx:
            bone = min(range(14), key=lambda b: box_dist(p, centers[b], halves[b]) if b not in LEG else 1e30)
        else:
            bone = min(CORE, key=lambda b: box_dist(p, centers[b], halves[b]))
        c = centers[bone]
        binds.append([(bone, 1.0, (p[0] - c[0], p[1] - c[1], p[2] - c[2]))])
    return binds


# ---------------------------------------------------------------------------
# Topology-based segmentation (docs/plans/topology-rig.md).
#
# Verts are labeled by mesh CONNECTIVITY and surface distance, never by
# absolute coordinate bands: connected components first (whole limbs/armor
# are separate sheets in these meshes), then geodesic farthest-point sampling
# names the extremity seeds of the main sheet (head/feet/hands), skeleton
# paths between the seeds are split proportionally into bone chains, and
# every vert joins the chain holding its geodesically nearest path-vert.
# ---------------------------------------------------------------------------

LIMB_MIN_VERTS = 18       # smaller floating pieces are accessories
LIMB_MIN_DIAMETER = 0.35  # fraction of model H for a whole-limb sheet
LATERAL_X = 0.22          # fraction of model W: lateral vs central component


def _adjacency(nv, faces):
    adj = [set() for _ in range(nv)]
    for f in faces:
        a, b, c = f[0], f[1], f[2]
        adj[a].update((b, c))
        adj[b].update((a, c))
        adj[c].update((a, b))
    return adj


def _components(adj):
    comp = [-1] * len(adj)
    n = 0
    for s in range(len(adj)):
        if comp[s] >= 0:
            continue
        stack = [s]
        comp[s] = n
        while stack:
            u = stack.pop()
            for v in adj[u]:
                if comp[v] < 0:
                    comp[v] = n
                    stack.append(v)
        n += 1
    return comp, n


def _dijkstra(src, adj, pts):
    dist = [math.inf] * len(adj)
    parent = [-1] * len(adj)
    dist[src] = 0.0
    pq = [(0.0, src)]
    while pq:
        d, u = heapq.heappop(pq)
        if d > dist[u]:
            continue
        for v in adj[u]:
            nd = d + math.dist(pts[u], pts[v])
            if nd < dist[v]:
                dist[v] = nd
                parent[v] = u
                heapq.heappush(pq, (nd, v))
    return dist, parent


def _dijkstra_w(src, adj, pts, vcost):
    """Dijkstra with a per-vertex cost multiplier (routing bias)."""
    dist = [math.inf] * len(adj)
    parent = [-1] * len(adj)
    dist[src] = 0.0
    pq = [(0.0, src)]
    while pq:
        d, u = heapq.heappop(pq)
        if d > dist[u]:
            continue
        for v in adj[u]:
            nd = d + math.dist(pts[u], pts[v]) * vcost(v)
            if nd < dist[v]:
                dist[v] = nd
                parent[v] = u
                heapq.heappush(pq, (nd, v))
    return dist, parent


def _fps_seeds(start, adj, pts, k):
    seeds = [start]
    dist, _ = _dijkstra(start, adj, pts)
    for _ in range(k - 1):
        # Only consider verts reachable from the current seed set: other
        # components sit at infinity and would end the sampling early.
        cand = [i for i, d in enumerate(dist) if d != math.inf and i not in seeds]
        if not cand:
            break
        nxt = max(cand, key=lambda i: dist[i])
        seeds.append(nxt)
        nd, _ = _dijkstra(nxt, adj, pts)
        dist = [min(a, b) for a, b in zip(dist, nd)]
    return seeds


def _path_to(parent, src, dst):
    out, u = [], dst
    while u >= 0:
        out.append(u)
        if u == src:
            break
        u = parent[u]
    out.reverse()
    if not out or out[0] != src:
        return None
    return out


def _path_len(path, pts):
    return sum(math.dist(pts[a], pts[b]) for a, b in zip(path, path[1:]))


def _seg_dist(p, pa, pb):
    """3D distance from point p to segment [pa, pb] (position tuples)."""
    ab = (pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2])
    ap = (p[0] - pa[0], p[1] - pa[1], p[2] - pa[2])
    ll = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]
    t = 0.0 if ll == 0.0 else max(0.0, min(1.0, (ap[0] * ab[0] + ap[1] * ab[1]
                                                 + ap[2] * ab[2]) / ll))
    q = (pa[0] + ab[0] * t, pa[1] + ab[1] * t, pa[2] + ab[2] * t)
    return math.dist(p, q)


def _split_chain_at(chain, pts, z_target):
    """Split an ordered chain at the vert whose z is closest to z_target.
    Landmark-z interpolation is stable where half-path-length splits drift
    (surface wiggle makes length non-monotonic in height)."""
    bi, bd = 0, math.inf
    for i, v in enumerate(chain):
        d = abs(pts[v][2] - z_target)
        if d < bd:
            bi, bd = i, d
    return chain[:bi], chain[bi:]


def segment_labels(verts, faces):
    """Label every vertex with its ragdoll bone index (-1 = accessory).

    Bones follow BONE_NAMES order; feet are folded into their shin at chain
    level (the ragdoll skips foot bodies). Returns (labels, info).
    """
    nv = len(verts)
    adj = _adjacency(nv, faces)
    comp, ncomp = _components(adj)
    members = [[] for _ in range(ncomp)]
    for i, c in enumerate(comp):
        members[c].append(i)
    sizes = sorted((len(m) for m in members), reverse=True)

    mn, mx = bbox(verts)
    H = mx[2] - mn[2]
    floor = mn[2]
    W = mx[0] - mn[0]

    labels = [-1] * nv
    chains = {}
    groin_z = None
    groin_z = None

    def mark(chain, bone):
        for v in chain:
            labels[v] = bone
        chains.setdefault(bone, []).extend(chain)

    # --- dominant sheet: contains both the head and (most of) the body ----
    main = max(range(ncomp), key=lambda c: len(members[c]))
    have_main = len(members[main]) >= 0.5 * nv

    if have_main:
        mv = set(members[main])
        top = max(mv, key=lambda i: verts[i][2])
        seeds = _fps_seeds(top, adj, verts, 18)

        head = max(seeds, key=lambda i: verts[i][2])
        rest = [s for s in seeds if s != head]
        rest.sort(key=lambda i: verts[i][2])

        fL = rest[0]
        fR = next((s for s in rest[1:]
                   if (verts[s][0] < 0) != (verts[fL][0] < 0)), None)

        dL, parL = _dijkstra(fL, adj, verts)
        path_fl_head = _path_to(parL, fL, head)
        parR = None
        if fR is not None:
            _, parR = _dijkstra(fR, adj, verts)

        # Landmarks. Hip line: fraction of floor->crown H (the crotch is
        # NOT reliably findable topologically — the butt crease keeps the
        # two leg->head paths parallel until the lower back, so path-merge
        # "groin" landmarks sit a band too high). Neck line: the z where
        # the arm chains branch off the trunk (topological, falls back to
        # the classic 0.80H).
        crown_z = verts[head][2]
        pelvis_bot_z = floor + 0.42 * H
        pelvis_top_z = floor + 0.54 * H
        groin_z = pelvis_bot_z

        # Trunk = the leg->head path from the hip line up to the crown.
        gi = next((k for k, v in enumerate(path_fl_head)
                   if verts[v][2] >= pelvis_bot_z), 0)
        trunk = path_fl_head[gi:]
        trunk_set = set(trunk)

        # Recenter the trunk path onto the body axis NOW (slab centroids):
        # it serves both as routing bias (limb chains must hug the outer
        # surface, not shortcut through the armpit/groin creases near the
        # axis) and as the assignment skeleton for trunk bones.
        axis_pos = {}
        for v in trunk:
            z = verts[v][2]
            slab = [u for u in mv if abs(verts[u][2] - z) <= 2.0]
            axis_pos[v] = (st.mean(verts[u][0] for u in slab),
                           st.mean(verts[u][1] for u in slab), z)
        axis_segs = [(axis_pos[a], axis_pos[c])
                     for a, c in zip(trunk, trunk[1:])]
        d_axis = [min((_seg_dist(verts[v], pa, pb) for pa, pb in axis_segs),
                      default=1e9) for v in range(nv)]
        # Verts near the axis cost up to 4x: limb routes stay on the outer
        # surface instead of cutting through body creases.
        vcost = lambda v: 1.0 + 3.0 * max(0.0, 1.0 - d_axis[v] / 8.0)

        def zcut(chain, zt):
            j = min(range(len(chain)),
                    key=lambda k: abs(verts[chain[k]][2] - zt))
            return max(1, j)

        # Legs: foot->hip-line paths (axis-biased so they take the outer
        # leg route), knee split at the z-midpoint; shin absorbs the foot.
        leg_segs = []
        for foot, th, sh in ((fL, 8, 10), (fR, 9, 11)):
            if foot is None:
                continue
            _, parF = _dijkstra_w(foot, adj, verts, vcost)
            p = _path_to(parF, foot, path_fl_head[gi])
            p = [v for v in p if verts[v][2] < pelvis_bot_z]
            if len(p) < 2:
                continue
            knee_z = verts[foot][2] + 0.5 * (pelvis_bot_z - verts[foot][2])
            sh_ch, th_ch = _split_chain_at(p, verts, knee_z)
            mark(th_ch, th)
            mark(sh_ch, sh)
            leg_segs.extend((a, c) for a, c in zip(p, p[1:]))

        # Arm chains: hand tip per side = the vert geodesically farthest
        # from the head among verts clear (3D) of the trunk path AND the
        # leg chains — hands hang at thigh height but are the farthest
        # surface points from the skull that are neither trunk nor leg.
        dH = _dijkstra(head, adj, verts)[0]
        arm_chains = {}
        for side in (0, 1):
            sgn = -1.0 if side == 0 else 1.0
            best, bd = None, -1.0
            for v in mv:
                if (verts[v][0] < 0) != (sgn < 0) or dH[v] == math.inf:
                    continue
                if verts[v][2] < floor + 0.10 * H:
                    continue                       # feet zone
                pv = verts[v]
                if any(_seg_dist(pv, verts[a], verts[c]) < 5.0
                       for a, c in zip(trunk, trunk[1:])):
                    continue
                if any(_seg_dist(pv, verts[a], verts[c]) < 5.0
                       for a, c in leg_segs):
                    continue
                if dH[v] > bd:
                    best, bd = v, dH[v]
            if best is None:
                continue
            _, parH = _dijkstra_w(best, adj, verts, vcost)
            ph = _path_to(parH, best, head)
            # keep the chain below the shoulder line; beyond it the route
            # climbs the neck toward the crown
            shoulder_z = floor + 0.80 * H
            arm_ch = [v for v in ph if verts[v][2] <= shoulder_z + 2.0]
            if len(arm_ch) < 2 or arm_ch[-1] == arm_ch[0]:
                continue
            arm_chains[side] = arm_ch

        for side, arm_ch in arm_chains.items():
            hand_z = verts[arm_ch[0]][2]
            elbow_z = hand_z + 0.5 * (shoulder_z - hand_z)
            fa_ch, ua_ch = _split_chain_at(arm_ch, verts, elbow_z)
            mark(fa_ch, 6 + side)
            mark(ua_ch, 4 + side)

        # Trunk bands at the classic floor-fractions (proven across all
        # stock models; they only partition the axis polyline, not the
        # mesh — limbs come from the topology above).
        i1 = zcut(trunk, pelvis_top_z)
        i2 = zcut(trunk, floor + 0.80 * H)
        i3 = zcut(trunk, floor + 0.855 * H)
        mark(trunk[:i1], 0)                        # Pelvis
        mark(trunk[i1:i2], 1)                      # Spine
        mark(trunk[i2:i3], 2)                      # Neck
        mark(trunk[i3:], 3)                        # Head

        # Full assignment: every main-sheet vert joins the bone whose chain
        # POLYLINE it lies closest to in 3D (not geodesic-to-path-verts: the
        # foot->head surface path runs along the back, so front-torso verts
        # would geodesically "touch" thigh/head chains first). Chains were
        # traced over the real limb routes by the geodesic machinery above;
        # the final metric is Euclidean-to-curve, consistent with the physics
        # boxes. Ties prefer limbs (order) over trunk segments.
        segs = {}
        for b, vs in chains.items():
            ordered = list(dict.fromkeys(vs))     # dedupe, keep path order
            segs[b] = [(ordered[i], ordered[i + 1])
                       for i in range(len(ordered) - 1)]
        order = [4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3]
        for v in sorted(mv):
            p = verts[v]
            best, bb = math.inf, -1
            for b in order:
                for a, c in segs.get(b, ()):
                    dd = _seg_dist(p, axis_pos.get(a, verts[a]),
                                   axis_pos.get(c, verts[c]))
                    if dd < best:
                        best, bb = dd, b
            if bb >= 0:
                labels[v] = bb

        # Two majority-vote passes over the 1-ring clean up salt-and-pepper
        # flips where a hand rests against a thigh (both chains pass close).
        for _ in range(2):
            updates = []
            for v in mv:
                nb = [labels[u] for u in adj[v] if labels[u] >= 0]
                if len(nb) >= 3:
                    cur = labels[v]
                    win = st.mode(nb)
                    if win != cur and nb.count(win) >= len(nb) // 2 + 1:
                        updates.append((v, win))
            for v, w in updates:
                labels[v] = w
    else:
        # Boss-style fully split body: no dominant sheet. Central components
        # stack by height onto Head/Spine/Pelvis; limbs map later.
        centrals = [c for c in range(ncomp)
                    if abs(st.mean(verts[i][0] for i in members[c])) <= LATERAL_X * W]
        centrals.sort(key=lambda c: -st.mean(verts[i][2] for i in members[c]))
        for c, bone in zip(centrals, (3, 1, 0)):
            mark(members[c], bone)
        groin_z = floor + 0.45 * H

    pelvis_z = groin_z if groin_z is not None else floor + 0.45 * H

    # --- remaining components: whole-limb sheets or accessories ----------
    islands = []
    for c in range(ncomp):
        mem = members[c]
        if have_main and c == main:
            continue
        if all(labels[i] >= 0 for i in mem):
            continue
        cx = st.mean(verts[i][0] for i in mem)
        cz = st.mean(verts[i][2] for i in mem)
        dia_d, _ = _dijkstra(mem[0], adj, verts)
        dia = max(dia_d[i] for i in mem)
        if len(mem) >= LIMB_MIN_VERTS and dia >= LIMB_MIN_DIAMETER * H \
                and abs(cx) > LATERAL_X * W:
            role = "leg" if cz < pelvis_z else "arm"
            side = 0 if cx < 0 else 1
            # Geodesic diameter endpoints; root = the higher one.
            a = mem[0]
            dd, _ = _dijkstra(a, adj, verts)
            b = max(mem, key=lambda i: dd[i])
            da, _ = _dijkstra(b, adj, verts)
            a2 = max(mem, key=lambda i: da[i])
            root, tip = (a2, b) if verts[a2][2] > verts[b][2] else (b, a2)
            _, parT = _dijkstra(root, adj, verts)
            chain = _path_to(parT, root, tip)
            mid_z = verts[root][2] + 0.5 * (verts[tip][2] - verts[root][2])
            hi_ch, lo_ch = _split_chain_at(chain, verts, mid_z)
            if role == "arm":
                mark(hi_ch, 4 + side)
                mark(lo_ch, 6 + side)
            else:
                mark(hi_ch, 8 + side)
                mark(lo_ch, 10 + side)
            islands.append((c, len(mem), role, "L" if side == 0 else "R"))
        else:
            islands.append((c, len(mem), "accessory", "-"))

    # --- mirror safety: paired bones must not straddle X=0 ---------------
    for bl, br in ((4, 5), (6, 7), (8, 9), (10, 11)):
        for b, o in ((bl, br), (br, bl)):
            own = [i for i in range(nv) if labels[i] == b]
            if not own:
                continue
            maj = -1 if st.mean(verts[i][0] for i in own) < 0 else 1
            bad = [i for i in own if (verts[i][0] < 0) != (maj < 0)]
            if len(bad) > 0.15 * len(own):
                for i in bad:
                    labels[i] = o

    per_bone = [sum(1 for l in labels if l == b) for b in range(12)]
    info = {
        "H": H, "W": W, "floor": floor, "groin_z": groin_z,
        "ncomp": ncomp, "sizes": sizes[:8],
        "per_bone": per_bone,
        "unlabeled": sum(1 for l in labels if l < 0),
        "islands": islands,
    }
    return labels, info


def derive_bones_topo(verts, labels, info):
    """Skeleton from label clusters: centers = cluster centroids (mirror
    averaged), halves = cluster extents. Replaces derive_bones' bands."""
    nv = len(verts)
    clusters = [[] for _ in range(14)]
    for i, l in enumerate(labels):
        if l >= 0:
            clusters[l].append(i)

    def cen(idxs):
        if not idxs:
            return None
        return tuple(st.mean(verts[i][k] for i in idxs) for k in range(3))

    centers = [cen(clusters[b]) for b in range(14)]

    # Mirror-average L/R pairs so the physics skeleton stays symmetric; a
    # missing side clones the populated one instead of failing the build.
    def sym_pair(b):
        cl, cr = centers[b], centers[b + 1]
        if cl is None and cr is None:
            return None
        if cl is None:
            cl = (-cr[0], cr[1], cr[2])
        if cr is None:
            cr = (-cl[0], cl[1], cl[2])
        ax = (abs(cl[0]) + abs(cr[0])) / 2.0
        ay = (cl[1] + cr[1]) / 2.0
        az = (cl[2] + cr[2]) / 2.0
        return (-ax, ay, az), (ax, ay, az)

    for b in (4, 6, 8, 10):
        pair = sym_pair(b)
        if pair is None:
            continue
        centers[b], centers[b + 1] = pair
    for i in range(4):
        if centers[i] is not None:
            centers[i] = (0.0, centers[i][1], centers[i][2])

    # Arm centers sit on the OUTER arm line (mean |x| of the outer half of
    # the bone's verts): chest/shoulder skin legitimately labels as upper-arm
    # but drags the plain centroid into the torso, shoving the physics box
    # through the spine column.
    for b in (4, 5, 6, 7):
        if not clusters[b] or centers[b] is None:
            continue
        c = centers[b]
        xs = sorted(abs(verts[i][0]) for i in clusters[b])
        outer = xs[len(xs) // 2:]
        sgn = -1.0 if c[0] < 0 else 1.0
        centers[b] = (sgn * sum(outer) / len(outer), c[1], c[2])

    # Synthesize a Neck when no verts were labeled (fully-split bodies):
    # the physics skeleton needs a body there regardless of vertex count.
    synth = set()
    if centers[2] is None and centers[1] is not None and centers[3] is not None:
        centers[2] = tuple((a + b) / 2.0 for a, b in zip(centers[1], centers[3]))
        synth.add(2)

    missing = [BONE_NAMES[b] for b in range(12)
               if centers[b] is None and b != 2]
    if missing:
        raise RuntimeError("topology segmentation left bones unlabeled: "
                           + ", ".join(missing))

    # Physics boxes are deliberately SLIMMER than the visual surface (Box3D
    # explodes non-adjacent overlaps), so cluster extents are capped per
    # bone at the classic cross-sections; only the LENGTH axis stays fully
    # cluster-derived (model-proportion adaptive).
    caps = {0: (9.0, 6.0), 1: (7.0, 5.0), 2: (2.6, 2.6), 3: (4.2, 4.2),
            4: (2.2, 2.2), 5: (2.2, 2.2), 6: (1.8, 1.8), 7: (1.8, 1.8),
            8: (3.2, 3.2), 9: (3.2, 3.2), 10: (2.6, 2.6), 11: (2.6, 2.6)}
    halves = []
    for b in range(14):
        cl = clusters[b]
        c = centers[b]
        if not cl or c is None or b >= 12:
            halves.append((2.0, 2.0, 2.5))
            continue
        hx = max(abs(verts[i][0] - c[0]) for i in cl)
        hy = max(abs(verts[i][1] - c[1]) for i in cl)
        hz = max(abs(verts[i][2] - c[2]) for i in cl)
        cx, cy = caps.get(b, (2.0, 2.0))
        hmin = 1.0 if b < 4 else 2.0
        halves.append((max(min(hx, cx), hmin), max(min(hy, cy), hmin),
                       max(hz, hmin)))
    if 2 in synth:
        halves[2] = (2.6, 2.6, max(2.5, abs(centers[3][2] - centers[1][2]) / 6.0))

    # Trunk stack ordering: Spine must stop where Head begins (they are
    # non-adjacent; Box3D explodes overlaps). Shrink whichever box crosses
    # the midpoint between them.
    top_spine = centers[1][2] + halves[1][2]
    bot_head = centers[3][2] - halves[3][2]
    if top_spine > bot_head:
        mid = (top_spine + bot_head) / 2.0
        halves[1] = (halves[1][0], halves[1][1],
                     max(1.0, mid - centers[1][2]))
        halves[3] = (halves[3][0], halves[3][1],
                     max(1.0, centers[3][2] - mid))

    # Shins span thigh-bottom -> floor (feet ride the shin, as before).
    foot_bottom = min(v[2] for v in verts)
    for b in (10, 11):
        knee_top = centers[8][2] - halves[8][2]
        new_cz = (knee_top + foot_bottom) / 2.0
        new_hz = (knee_top - foot_bottom) / 2.0 + 1.0
        centers[b] = (centers[b][0], centers[b][1], new_cz)
        halves[b] = (halves[b][0], halves[b][1], new_hz)
    # Feet bones carry no bodies (skipped); they ride the final shin stats.
    centers[12], centers[13] = centers[10], centers[11]
    halves[12], halves[13] = halves[10], halves[11]

    fore_idx = {i for i, l in enumerate(labels) if l in (6, 7)}
    arm_verts = [i for i, l in enumerate(labels) if l in (4, 5, 6, 7)]
    arm_y = st.mean(verts[i][1] for i in arm_verts) if arm_verts else 0.0
    elbow = (centers[4][2] + centers[6][2]) / 2.0
    report = {
        "H": info["H"], "foot": info["floor"], "arm_thr": 0.0,
        "n_head": sum(1 for l in labels if l == 3),
        "n_neck": sum(1 for l in labels if l == 2),
        "n_pelvis": sum(1 for l in labels if l == 0),
        "n_spine": sum(1 for l in labels if l == 1),
        "n_arm": sum(1 for l in labels if l in (4, 5, 6, 7)),
        "n_leg": sum(1 for l in labels if l in (8, 9, 10, 11)),
        "arm_idx": set(), "foot_idx": set(), "fore_idx": fore_idx,
        "elbow": elbow, "arm_y": arm_y, "synth": synth,
    }
    return centers, halves, report


def bind_verts_topo(verts, centers, halves, labels):
    """Rigid 1-bone binding from segmentation labels; unlabeled accessory
    verts take the nearest bone box (their sheets touch only their part)."""
    CORE = list(range(12))
    binds = []
    for i, p in enumerate(verts):
        bone = labels[i] if 0 <= labels[i] < 14 else \
            min(CORE, key=lambda b: box_dist(p, centers[b], halves[b]))
        c = centers[bone]
        binds.append([(bone, 1.0, (p[0] - c[0], p[1] - c[1], p[2] - c[2]))])
    return binds


def flatten_binds(binds, n):
    """Flatten per-vertex binds into dense NumVerts*3 arrays for the header."""
    bones, offs, weights = [], [], []
    for entry in binds:
        for k in range(3):
            if k < len(entry):
                b, w, o = entry[k]
                bones.append(b); weights.append(w); offs.append(o)
            else:
                bones.append(0); weights.append(0.0); offs.append((0.0, 0.0, 0.0))
    return bones, offs, weights


def verify(verts, centers, halves, bones, offs, model, allow_empty=()):
    """Hard assertions + numeric report. Raise on failure.

    Physics boxes are deliberately thinner than the visual mesh surface, so
    "verts inside their box" is NOT a criterion. The real checks:
      - no bone starvation (every bone owns >= 4 verts; synthesized bones in
        allow_empty are exempt but warned about);
      - each bone's verts are near its center (mean offset well under 2x the
        box diagonal = the bone is actually ON its vert cluster);
      - no non-adjacent bodies interpenetrate (Box3D explodes those).
    """
    problems = []
    per_bone = [0] * 14
    mean_off = [0.0] * 14
    for i, b in enumerate(bones):
        per_bone[b] += 1
        mean_off[b] += math.sqrt(offs[i][0] ** 2 + offs[i][1] ** 2 + offs[i][2] ** 2)
    print(f"  per-bone verts: {per_bone}")
    # Feet are skipped from the ragdoll (bones 12/13 never get verts by design).
    for b in range(12):
        n = max(per_bone[b], 1)
        mean_off[b] /= n
        diag = 2.0 * math.sqrt(halves[b][0] ** 2 + halves[b][1] ** 2 + halves[b][2] ** 2)
        print(f"    {BONE_NAMES[b]:9s} n={per_bone[b]:3d} meanOff={mean_off[b]:5.1f} "
              f"diag={diag:5.1f} ratio={mean_off[b] / max(diag, 1e-6):.2f}")
        if per_bone[b] < 2:
            if b in allow_empty:
                print(f"    NOTE: {BONE_NAMES[b]} synthesized (no labeled verts)")
            else:
                problems.append(f"bone {BONE_NAMES[b]} starved ({per_bone[b]} verts)")
        if mean_off[b] > 2.0 * diag:
            problems.append(f"bone {BONE_NAMES[b]} verts far from center "
                            f"(meanOff {mean_off[b]:.1f} > 2x diag {diag:.1f})")

    # Non-adjacent bodies must not interpenetrate. Bones 12/13 are skipped:
    # the real-mesh ragdoll spawns 12 bodies (no feet).
    for a in range(12):
        for b in range(a + 1, 12):
            if (a, b) in ADJACENT or (b, a) in ADJACENT:
                continue
            dx = abs(centers[a][0] - centers[b][0])
            dy = abs(centers[a][1] - centers[b][1])
            dz = abs(centers[a][2] - centers[b][2])
            if (dx < halves[a][0] + halves[b][0] and
                    dy < halves[a][1] + halves[b][1] and
                    dz < halves[a][2] + halves[b][2]):
                problems.append(
                    f"non-adjacent {BONE_NAMES[a]}/{BONE_NAMES[b]} interpenetrate "
                    f"(d=({dx:.1f},{dy:.1f},{dz:.1f}) hsum=({halves[a][0]+halves[b][0]:.1f},"
                    f"{halves[a][1]+halves[b][1]:.1f},{halves[a][2]+halves[b][2]:.1f}))")
    if problems:
        raise RuntimeError(f"{model} rig verification FAILED:\n  " + "\n  ".join(problems))
    print(f"  OK: no starvation, bones on clusters, no non-adjacent interpenetration")
    return per_bone


def build_rig(pkg, sel_name, live_name=None, frame=0, fold_deg=0,
              bind_mode="topo"):
    """Full pipeline for one Selection viewer mesh (bind frame 0 by default)."""
    idx = None
    for i, e, cn in find_meshes(pkg):
        if pkg.name(e.object_name) == sel_name and cn == "Core.LodMesh":
            idx = i
            break
    if idx is None:
        raise RuntimeError(f"mesh {sel_name} not found")
    m = parse_lod_mesh(pkg, idx)
    if frame < 0 or frame >= m.anim_frames:
        raise RuntimeError(f"{sel_name}: frame {frame} out of range ({m.anim_frames})")

    # Derive from the wedge-referenced verts ONLY (the first SpecialVerts per
    # frame are invisible attachment points).
    used = sorted({w[0] + m.special_verts for w in m.wedges})
    world = [mesh_to_world(m.verts[frame * m.frame_verts + k], m) for k in used]
    remap = {old: new for new, old in enumerate(used)}

    # 1) Center on the body centroid, then yaw-correct to bind space. The
    #    mirror-score search has two 90-deg-apart optima (width vs depth axis);
    #    the leg-split metric disambiguates to front-facing (legs split across X).
    ctr = (sum(v[0] for v in world) / len(world),
           sum(v[1] for v in world) / len(world))
    cen = [(v[0] - ctr[0], v[1] - ctr[1], v[2]) for v in world]
    mir, yaw = find_yaw(cen)

    def split_frac(pts):
        low = [v for v in pts if v[2] < -8.0]
        return (sum(1 for v in low if abs(v[0]) > 3.0) / max(len(low), 1), len(low))

    yaw, (split0, n_low) = _pick_facing(rotz(cen, yaw), yaw, rotz(cen, yaw + 90))
    bind = rotz(cen, yaw)

    # Dense face->vert triples (topology input; translation-independent).
    dfaces = [tuple(remap[m.wedges[fa[k]][0] + m.special_verts]
                    for k in range(3)) for fa in m.faces]

    # 2) Derive bones from geometry.
    labels = None
    if bind_mode == "topo":
        labels, seginfo = segment_labels(bind, dfaces)
        centers, halves, rep = derive_bones_topo(bind, labels, seginfo)
    else:
        centers, halves, rep = derive_bones(bind)

    # 2b) Guarantee non-adjacent clearance: the long forearm/upper-arm boxes must
    #     clear the thigh/shin boxes on the same side. Push the arm bone outward
    #     (never inward) by whatever the geometry needs + a small gap.
    centers = _enforce_clearance(centers, halves)

    # 3) Translate so pelvis sits at the origin (canonical convention).
    px, py, pz = centers[0]
    bind = [(v[0] - px, v[1] - py, v[2] - pz) for v in bind]
    centers = [(c[0] - px, c[1] - py, c[2] - pz) for c in centers]

    # 4) Anchors + bind (smooth multi-bone weights; feet rigid on the shin).
    anchors = compute_anchors(centers)

    # 4b) Verify the PHYSICS skeleton on the pre-fold verts (the fold only
    #     repositions mesh verts; bones/boxes/anchors are unchanged, so the
    #     "verts near center" sanity check is only meaningful before the fold).
    if labels is None:
        binds = bind_verts(bind, centers, halves, rep["arm_idx"],
                           rep["foot_idx"], rep["fore_idx"])
    else:
        binds = bind_verts_topo(bind, centers, halves, labels)
    per_bone = verify(bind, centers, halves,
                      [b[0][0] for b in binds], [b[0][2] for b in binds],
                      sel_name, allow_empty=rep.get("synth", ()))

    # 4c) Fold the forearm+hand verts up about the elbow so the hands leave the
    #     ankles. Only mesh positions change (skeleton stays straight). Then
    #     rebind so the offsets reflect the folded pose.
    if fold_deg > 0:
        bind = fold_forearms(bind, centers, rep["arm_y"] - py,
                             rep["elbow"] - pz, rep["fore_idx"], fold_deg)
    if labels is None:
        binds = bind_verts(bind, centers, halves, rep["arm_idx"],
                           rep["foot_idx"], rep["fore_idx"])
    else:
        binds = bind_verts_topo(bind, centers, halves, labels)
    bind_bones, bind_offsets, bind_weights = flatten_binds(binds, len(bind))

    print(f"  yaw={yaw:+d} mirror={mir:.2f} legSplit={split0:.2f} (n={n_low}) H={rep['H']:.1f} "
          f"n_head={rep['n_head']} n_neck={rep['n_neck']} n_pelvis={rep['n_pelvis']} "
          f"n_spine={rep['n_spine']} n_arm={rep['n_arm']} n_leg={rep['n_leg']}")
    if labels is not None:
        isl = ", ".join(f"C{c}:{n}v:{r}{s}" for c, n, r, s in seginfo["islands"])
        print(f"  topo: comps={seginfo['ncomp']} sizes={seginfo['sizes']} "
              f"unlabeled(accessory-proximity)={seginfo['unlabeled']} islands=[{isl}]")
    print(f"  pelvis={tuple(round(x,1) for x in centers[0])} "
          f"spine={tuple(round(x,1) for x in centers[1])} "
          f"head={tuple(round(x,1) for x in centers[3])}")
    print(f"  upArmL={tuple(round(x,1) for x in centers[4])} "
          f"foreArmL={tuple(round(x,1) for x in centers[6])} "
          f"thighL={tuple(round(x,1) for x in centers[8])} "
          f"footL={tuple(round(x,1) for x in centers[12])}")

    # Wedges: reindex to the dense used-vert array (removes the gap left by the
    # SpecialVerts that are baked into the raw iVertex values).
    wedges = [(remap[w[0] + m.special_verts], w[1], w[2]) for w in m.wedges]

    # Legacy mode only: drop the fused hand<->leg "weld" faces. Topology
    # audits proved these meshes contain NO true hand-leg welded triangles
    # (limbs are separate sheets), so the topo path keeps the mesh complete.
    if labels is None:
        faces = [f for f in m.faces
                 if not _is_arm_leg_face(f, wedges, rep["fore_idx"], rep["leg_idx"])]
        print(f"  dropped {len(m.faces) - len(faces)} weld faces "
              f"(kept {len(faces)}/{len(m.faces)})")
    else:
        faces = list(m.faces)

    return {
        "live_name": live_name or sel_name,
        "sel_name": sel_name,
        "verts": bind,
        "faces": faces,
        "wedges": wedges,
        "materials": m.materials,
        "centers": centers,
        "halves": halves,
        "anchors": anchors,
        "bind_bones": bind_bones,
        "bind_offsets": bind_offsets,
        "bind_weights": bind_weights,
        "per_bone": per_bone,
        "report": rep,
        "yaw": yaw,
        "mirror": mir,
        "mesh": m,
    }


def _enforce_clearance(centers, halves):
    """Move arm bone centers outward (never inward) so their boxes clear the
    same-side thigh/shin/foot boxes with a 1.5 UU gap. Returns new centers."""
    gap = 1.5
    centers = [c[:] for c in centers]
    for arm_bone, leg_side in [(4, 0), (5, 1), (6, 0), (7, 1)]:
        # leg bones on the same side (L: 8,10,12; R: 9,11,13); pelvis is
        # central but wide, so arms must clear it too
        leg_bones = [0, 8, 10, 12] if leg_side == 0 else [0, 9, 11, 13]
        sign = -1.0 if centers[arm_bone][0] < 0 else 1.0
        ax = abs(centers[arm_bone][0])
        a_hx = halves[arm_bone][0]
        need = a_hx
        for lb in leg_bones:
            lx = abs(centers[lb][0])
            # z-overlap check: arm box must actually overlap this leg box in Z
            # before X clearance matters (avoids over-pushing distant pairs).
            az0 = centers[arm_bone][2] - halves[arm_bone][2]
            az1 = centers[arm_bone][2] + halves[arm_bone][2]
            lz0 = centers[lb][2] - halves[lb][2]
            lz1 = centers[lb][2] + halves[lb][2]
            if az1 > lz0 and az0 < lz1:
                need = max(need, a_hx + lx + halves[lb][0] + gap)
        centers[arm_bone] = (sign * max(ax, need), centers[arm_bone][1],
                             centers[arm_bone][2])
    return centers


def _is_arm_leg_face(fa, wedges, fore_idx, leg_idx):
    """True if a face bridges an ARM/forearm+hand vert to a LEG/foot vert.

    The viewer pose hangs the hands down against the legs/feet, so the mesh
    contains triangles that connect forearm+hand verts to thigh/shin/foot verts.
    Those stretch into long spikes the moment the ragdoll separates the limbs,
    so they are dropped. Membership is by the derived vertex classification
    (fore_idx vs leg_idx), NOT by spatial bands: the old x/z threshold check
    misread foot-bottom verts (|x| up to ~12, z<-25) as "arm verts" and deleted
    the feet's own bottom faces, leaving holes in the soles.
    """
    ws = {wedges[fa[k]][0] for k in range(3)}
    fore = set(fore_idx or ())
    leg = set(leg_idx or ())
    return bool(ws & fore) and bool(ws & leg)


def _add_foot_boxes(verts, wedges, faces, bind_bones, bind_offsets, bind_weights,
                    centers, halves, foot_bot):
    """Rebuild each foot as a closed box bound rigidly to its shin bone.

    The Selection viewer feet are so low-poly that their "soles" are SHARED
    triangles with the hands that hang to the floor in the bind pose. Those
    bridges must be dropped (they'd spike when the ragdoll separates the
    limbs), which leaves the feet open at the bottom. Feet are skipped from
    the ragdoll and rigidly bound to the shins, so a clean procedural box per
    foot guarantees closed, intact feet. Each box corner binds to the shin of
    its side (bone 10=L, 11=R) with an offset.

    The box is derived from the SHIN bone (center x/y, half extents) and sits
    on the mesh floor, so both feet are the same size and symmetric. The source
    foot verts are too few/scattered to define a sane footprint (e.g. the right
    foot's leg verts can span 22 UU), which made the old bbox-based box absurdly
    long and asymmetric.
    """
    for side in (-1.0, 1.0):
        bone = 10 if side < 0 else 11
        bc = centers[bone]
        hx = halves[bone][0] * 0.85
        hy = halves[bone][1] * 0.85
        # Foot is short: a third of the shin height, sitting on the floor.
        hz = max(halves[bone][2] * 0.30, 2.5)
        cx = bc[0]
        cy = bc[1]
        cz = foot_bot + hz
        base_v = len(verts)
        base_w = len(wedges)
        corners = []
        for sx in (-1.0, 1.0):
            for sy in (-1.0, 1.0):
                for sz in (-1.0, 1.0):
                    corners.append((cx + hx * sx, cy + hy * sy, cz + hz * sz))
        for ci, c in enumerate(corners):
            verts.append(c)
            wedges.append((base_v + ci, 0.0, 0.0))
            for k in range(3):
                bind_bones.append(bone)
                bind_weights.append(1.0 if k == 0 else 0.0)
                bind_offsets.append((c[0] - bc[0], c[1] - bc[1], c[2] - bc[2]))
        # 12 tris for the 6 box faces. Quads per face with corner indices.
        quads = [
            (0, 1, 3, 2),   # -x
            (4, 6, 7, 5),   # +x
            (0, 2, 6, 4),   # -y
            (1, 5, 7, 3),   # +y
            (2, 3, 7, 6),   # -z  (bottom, facing down)
            (0, 4, 5, 1),   # +z
        ]
        for q in quads:
            faces.append((base_w + q[0], base_w + q[1], base_w + q[2], 0))
            faces.append((base_w + q[0], base_w + q[2], base_w + q[3], 0))
    return verts, wedges, faces, bind_bones, bind_offsets, bind_weights


def _pick_facing(Wa, ya, Wb):
    """Pick the yaw (of the two 90-deg-apart optima) whose lower body splits
    into two X clusters (front-facing, width along X) vs overlaps (side view)."""
    sa, na = _split(Wa)
    sb, nb = _split(Wb)
    if sa >= sb:
        return ya, (sa, na)
    return ya + 90, (sb, nb)


def _split(pts):
    low = [v for v in pts if v[2] < -8.0]
    if not low:
        return 0.0, 0
    return sum(1 for v in low if abs(v[0]) > 3.0) / len(low), len(low)


def emit_header(models, out_path):
    """Write RagdollDatasets.h with per-model static arrays + lookup table."""
    out = []
    out.append("// Generated by tools/build_rig.py. DO NOT EDIT.")
    out.append("// Per-model ragdoll datasets (bind-space UU, DrawScale=1, pelvis")
    out.append("// at origin). Wedge iVertex values already include the source")
    out.append("// SpecialVerts offset so they index the dense vert array directly.")
    out.append("#pragma once")
    out.append("")
    out.append('#include "RagdollInternal.h"')
    out.append("")

    def v3(name, pts):
        out.append(f"static const FVector {name}[{len(pts)}] = {{")
        for p in pts:
            out.append(f"\tFVector({p[0]:.6f}, {p[1]:.6f}, {p[2]:.6f}),")
        out.append("};")
        out.append("")

    def faces(name, faces):
        out.append(f"static const FMeshFace {name}[{len(faces)}] = {{")
        for f in faces:
            out.append(f"\t{{ {{ {f[0]}, {f[1]}, {f[2]} }}, {f[3]} }},")
        out.append("};")
        out.append("")

    def wedges(name, wedges):
        out.append(f"static const FMeshWedge {name}[{len(wedges)}] = {{")
        for w in wedges:
            out.append(f"\t{{ {w[0]}, {{ {w[1]}, {w[2]} }} }},")
        out.append("};")
        out.append("")

    def mats(name, mats):
        out.append(f"static const FMeshMaterial {name}[{len(mats)}] = {{")
        for mm in mats:
            out.append(f"\t{{ {mm[0]}u, {mm[1]} }},")
        out.append("};")
        out.append("")

    def bones(name, centers, halves):
        out.append(f"static const FRagdollBoneDef {name}[14] = {{")
        for b, c in enumerate(centers):
            h = halves[b]
            out.append(f"\t{{ {h[0]:.4f}, {h[1]:.4f}, {h[2]:.4f}, "
                       f"{c[0]:.4f}, {c[1]:.4f}, {c[2]:.4f}, {CANONICAL_MASS[b]:.1f} }},")
        out.append("};")
        out.append("")

    def ints(name, values, per):
        out.append(f"static const INT {name}[{len(values)}] = {{")
        for i in range(0, len(values), per):
            out.append("\t" + ", ".join(str(x) for x in values[i:i + per]) + ",")
        out.append("};")
        out.append("")

    def floats(name, values, per):
        out.append(f"static const FLOAT {name}[{len(values)}] = {{")
        for i in range(0, len(values), per):
            out.append("\t" + ", ".join(f"{x:.4f}" for x in values[i:i + per]) + ",")
        out.append("};")
        out.append("")

    for d in models:
        tag = d["sel_name"]
        v3(f"G{tag}_Verts", d["verts"])
        faces(f"G{tag}_Faces", d["faces"])
        wedges(f"G{tag}_Wedges", d["wedges"])
        mats(f"G{tag}_Materials", d["materials"])
        bones(f"G{tag}_Bones", d["centers"], d["halves"])
        v3(f"G{tag}_Anchors", d["anchors"])
        ints(f"G{tag}_BindBones", d["bind_bones"], 12)
        v3(f"G{tag}_BindOffsets", d["bind_offsets"])
        floats(f"G{tag}_BindWeights", d["bind_weights"], 12)

    out.append("// Model lookup table: live pawn mesh name -> dataset.")
    out.append("static const FRagdollDatasetEntry GRagdollDatasetTable[] = {")
    for d in models:
        tag = d["sel_name"]
        out.append(
            f'\t{{ TEXT("{d["live_name"]}"), TEXT("{tag}"), {len(d["verts"])}, '
            f'{len(d["faces"])}, {len(d["wedges"])}, {len(d["materials"])}, '
            f'G{tag}_Verts, G{tag}_Faces, G{tag}_Wedges, G{tag}_Materials, '
            f'G{tag}_Bones, G{tag}_Anchors, '
            f'G{tag}_BindBones, G{tag}_BindOffsets, G{tag}_BindWeights }},')
    out.append("};")
    out.append(f"static const INT GRagdollDatasetCount = {len(models)};")
    out.append("")
    with open(out_path, "w") as f:
        f.write("\n".join(out))
    print(f"wrote {out_path} ({len(models)} models)")


def emit_obj_overlay(d, obj_dir):
    os.makedirs(obj_dir, exist_ok=True)
    base = d["sel_name"]
    mesh_path = os.path.join(obj_dir, f"{base}_mesh.obj")
    skel_path = os.path.join(obj_dir, f"{base}_skeleton.obj")
    bones_path = os.path.join(obj_dir, f"{base}_mesh_bones.obj")

    # Mesh: one position per wedge (matches export_obj indexing).
    with open(mesh_path, "w") as f:
        f.write(f"# {base} bind-space frame 0 (yaw {d['yaw']:+d})\n")
        for w in d["wedges"]:
            p = d["verts"][w[0]]
            f.write(f"v {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}\n")
        for fa in d["faces"]:
            f.write(f"f {fa[0]+1} {fa[1]+1} {fa[2]+1}\n")

    # Skeleton: each bone is an OBJ group (o Skel_i) of 8 box verts + 6 quads.
    with open(skel_path, "w") as f:
        f.write(f"# {base} derived skeleton (12 boxes; feet skipped)\n")
        for b, c in enumerate(d["centers"][:12]):
            h = d["halves"][b]
            f.write(f"o Skel_{b}_{BONE_NAMES[b]}\n")
            for cx0 in (-1, 1):
                for cy0 in (-1, 1):
                    for cz0 in (-1, 1):
                        f.write(f"v {c[0]+h[0]*cx0:.3f} {c[1]+h[1]*cy0:.3f} {c[2]+h[2]*cz0:.3f}\n")
            quads = [(1, 3, 2, 0), (5, 7, 6, 4), (5, 1, 0, 4), (7, 3, 2, 6),
                     (6, 2, 0, 4), (7, 3, 1, 5)]
            for q in quads:
                off = b * 8 + 1
                f.write(f"f {off+q[0]} {off+q[1]} {off+q[2]}\n")
                f.write(f"f {off+q[0]} {off+q[2]} {off+q[3]}\n")

    # Mesh split into per-dominant-bone objects so the binding is visible:
    # each face is grouped by the dominant bone of its first vertex.
    vert_bone = {i: d["bind_bones"][i * 3] for i in range(len(d["verts"]))}
    with open(bones_path, "w") as f:
        f.write(f"# {base} mesh colored by dominant bone assignment\n")
        for b in range(12):
            group_faces = [fi for fi, fa in enumerate(d["faces"])
                           if vert_bone.get(d["wedges"][fa[0]][0], -1) == b]
            if not group_faces:
                continue
            f.write(f"o Bone_{b}_{BONE_NAMES[b]}\n")
            for w in d["wedges"]:
                p = d["verts"][w[0]]
                f.write(f"v {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}\n")
            for fi in group_faces:
                fa = d["faces"][fi]
                f.write(f"f {fa[0]+1} {fa[1]+1} {fa[2]+1}\n")
    print(f"  wrote {mesh_path}\n  wrote {skel_path}\n  wrote {bones_path}")
    return mesh_path, skel_path, bones_path


def render_verify(blender, bones_path, skel_path, obj_dir, base):
    script = os.path.join(obj_dir, "render_verify.py")
    _write_render_script(script)
    # Blender resolves relative paths against its own CWD (C:\) — use absolute.
    bones_path = os.path.abspath(bones_path)
    skel_path = os.path.abspath(skel_path)
    front = os.path.abspath(os.path.join(obj_dir, f"{base}_front.png"))
    threeq = os.path.abspath(os.path.join(obj_dir, f"{base}_3q.png"))
    back = os.path.abspath(os.path.join(obj_dir, f"{base}_back.png"))
    cmd = [blender, "-b", "--python-exit-code", "1", "-P", os.path.abspath(script),
           "--", bones_path, skel_path, front, threeq, back]
    print("  blender:", " ".join(cmd))
    r = subprocess_run(cmd)
    if r.returncode != 0:
        print(f"  !! blender render failed (rc={r.returncode})")
    else:
        print(f"  wrote {front}\n  wrote {threeq}")


def _write_render_script(path):
    script = r'''
# Headless Blender overlay render: bone-colored mesh + skeleton boxes.
# argv: mesh_bones.obj skeleton.obj out_front out_3q out_side
import bpy, sys
from mathutils import Vector
argv = sys.argv[sys.argv.index("--") + 1:]
bones_obj, skel_obj, out_front, out_3q, out_side = argv
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.obj_import(filepath=bones_obj)
bpy.ops.wm.obj_import(filepath=skel_obj)
scene = bpy.context.scene
palette = [(0.95,0.3,0.3),(0.3,0.95,0.3),(0.3,0.3,0.95),(0.95,0.95,0.2),(1,0.55,0),
           (1,0.15,0.5),(0.55,1,0),(0,1,1),(0.65,0.35,1),(0.95,0.65,0.2),(0.2,0.95,0.65),
           (0.9,0.2,0.85),(0.55,0.55,0.55),(1,1,1)]
def mat(name, col, alpha=1.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = False
    m.diffuse_color = (col[0], col[1], col[2], alpha)
    return m
# Color mesh groups by bone index, skeletons as thin wireframe overlay.
for o in [o for o in scene.objects if o.type == "MESH"]:
    if "Skel_" in o.name:
        o.data.materials.append(mat(o.name, (0.05, 0.05, 0.05), 1.0))
        o.show_wire = True
        o.show_all_edges = True
        # Keep the box faces see-through so the mesh shows through: only the
        # edges are drawn, faces get a faint transparent fill.
        o.data.materials[0].diffuse_color = (0.0, 0.0, 0.0, 0.15)
    elif "Bone_" in o.name:
        idx = int(o.name.split("_")[1])
        o.data.materials.append(mat(o.name, palette[idx % len(palette)]))
    else:
        o.data.materials.append(mat("m", (0.8, 0.8, 0.8)))
pts = [o.matrix_world @ v.co for o in scene.objects if o.type == "MESH" for v in o.data.vertices]
mn = Vector((min(v.x for v in pts), min(v.y for v in pts), min(v.z for v in pts)))
mx = Vector((max(v.x for v in pts), max(v.y for v in pts), max(v.z for v in pts)))
ctr = (mn + mx) / 2
rad = max((mx - mn).length * 1.6, 60.0)
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.color_type = "MATERIAL"
scene.render.resolution_x = 700
scene.render.resolution_y = 700
scene.render.image_settings.file_format = "PNG"
def render_shot(out_path, cam_pos):
    cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("C"))
    scene.collection.objects.link(cam)
    pos = Vector(cam_pos)
    cam.location = pos
    dirv = (ctr - pos)
    cam.rotation_euler = dirv.to_track_quat("-Z", "Y").to_euler()
    scene.camera = cam
    scene.render.filepath = out_path
    bpy.ops.render.render(write_still=True)
    scene.collection.objects.unlink(cam)
render_shot(out_front, (0.0, rad, rad * 0.35))
render_shot(out_3q, (rad * 0.7, rad * 0.7, rad * 0.5))
render_shot(out_side, (rad, 0.0, rad * 0.35))
'''
    with open(path, "w") as f:
        f.write(script)


def subprocess_run(cmd):
    import subprocess
    return subprocess.run(cmd, capture_output=True, text=True)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg")
    ap.add_argument("--models", default=None,
                    help="comma list of Selection mesh names; default all mapped")
    ap.add_argument("--out", default="RagdollPkg/Src/RagdollDatasets.h")
    ap.add_argument("--obj-dir", default="tools/rig_verify")
    ap.add_argument("--fold", type=float, default=0.0,
                    help="forearm fold angle in degrees (0 = natural standing pose; default 0)")
    ap.add_argument("--bind", choices=("topo", "legacy"), default="topo",
                    help="binding strategy: topology segmentation (default) "
                         "or legacy region-band classification")
    ap.add_argument("--render", action="store_true",
                    help="render OBJ overlays with headless Blender")
    args = ap.parse_args(argv)

    pkg = UTPackage(args.pkg)
    if args.models:
        wanted = set(args.models.split(","))
        pairs = [(l, s) for l, s in MODEL_MAP if s in wanted]
    else:
        pairs = MODEL_MAP

    built = []
    for live, sel in pairs:
        print(f"\n=== {sel} (live: {live}) ===")
        d = build_rig(pkg, sel, live, fold_deg=args.fold, bind_mode=args.bind)
        built.append(d)
        emit_obj_overlay(d, args.obj_dir)

    emit_header(built, args.out)

    if args.render:
        blender = os.environ.get("BLENDER_EXE") or shutil.which("blender")
        if not blender:
            print("!! no blender.exe (set BLENDER_EXE)")
        else:
            for d in built:
                base = d["sel_name"]
                bones_path = os.path.join(args.obj_dir, f"{base}_mesh_bones.obj")
                skel_path = os.path.join(args.obj_dir, f"{base}_skeleton.obj")
                render_verify(blender, bones_path, skel_path, args.obj_dir, base)


if __name__ == "__main__":
    main(sys.argv[1:])
