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
                              [--render] [--obj-dir dir]

With --render, Blender renders the overlay to PNGs (needs blender.exe on PATH or
set BLENDER_EXE). Run --render after --obj to verify bone placement visually.

Everything below uses Unreal units (UU), Z up, mirror symmetry about X=0.
"""

import argparse
import math
import os
import shutil
import statistics as st
import sys

from extract_mesh import UTPackage, parse_lod_mesh, find_meshes, mesh_to_world

# Live player mesh name -> Selection dataset name (face counts match per pair).
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
        (2.0, 4.0, hz(0.035)), (2.0, 4.0, hz(0.035)), # Feet
    ]
    report = {
        "H": H, "foot": foot, "arm_thr": arm_thr,
        "n_head": len(head), "n_neck": len(neck), "n_pelvis": len(pelvis),
        "n_spine": len(spine), "n_arm": len(arms), "n_leg": len(legs),
        "n_upper": len(upper), "n_fore": len(fore),
        "n_thigh": len(thigh), "n_shin": len(shin), "n_foot": len(foot_v),
        "upper_c": ua, "fore_c": fa, "arm_idx": arm_idx,
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
    hip_z = c[8][2] - (c[8][2] - c[10][2]) / 2.0   # thigh top
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


def bind_verts(verts, centers, halves, arm_idx=None):
    """Smooth multi-bone skinning weights. Returns per-vertex lists of up to 3
    (bone, weight, offset) entries (weight sum = 1, weight[0] dominant).

    Weights are inverse box-distance over the 3 nearest bones, so joints blend
    smoothly (verts at a knee transition between thigh and shin instead of
    tearing) while the core of each limb stays near-rigid to its bone (sharp
    power falloff). Vertices on the arm column (hands reach the ankles in the
    viewer pose) EXCLUDE the leg bones from the candidate set — otherwise the
    hands bind to the shin/foot and the arms tear off when the limbs separate.
    """
    LEG = {8, 9, 10, 11, 12, 13}
    binds = []
    for i, p in enumerate(verts):
        cand = range(14) if not (arm_idx and i in arm_idx) else [b for b in range(14) if b not in LEG]
        ds = sorted((box_dist(p, centers[b], halves[b]), b) for b in cand)
        top = ds[:3]
        inv = [1.0 / (d + 0.1) ** 3 for d, _ in top]
        wsum = sum(inv)
        entry = []
        for (_, b), w in zip(top, inv):
            c = centers[b]
            entry.append((b, w / wsum, (p[0] - c[0], p[1] - c[1], p[2] - c[2])))
        binds.append(entry)
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


def verify(verts, centers, halves, bones, offs, model):
    """Hard assertions + numeric report. Raise on failure.

    Physics boxes are deliberately thinner than the visual mesh surface, so
    "verts inside their box" is NOT a criterion. The real checks:
      - no bone starvation (every bone owns >= 4 verts);
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
    for b in range(14):
        n = max(per_bone[b], 1)
        mean_off[b] /= n
        diag = 2.0 * math.sqrt(halves[b][0] ** 2 + halves[b][1] ** 2 + halves[b][2] ** 2)
        print(f"    {BONE_NAMES[b]:9s} n={per_bone[b]:3d} meanOff={mean_off[b]:5.1f} "
              f"diag={diag:5.1f} ratio={mean_off[b] / max(diag, 1e-6):.2f}")
        if per_bone[b] < 2:
            problems.append(f"bone {BONE_NAMES[b]} starved ({per_bone[b]} verts)")
        if mean_off[b] > 2.0 * diag:
            problems.append(f"bone {BONE_NAMES[b]} verts far from center "
                            f"(meanOff {mean_off[b]:.1f} > 2x diag {diag:.1f})")

    # Non-adjacent bodies must not interpenetrate.
    for a in range(14):
        for b in range(a + 1, 14):
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


def build_rig(pkg, sel_name, live_name):
    """Full pipeline for one Selection model. Returns dict of arrays."""
    idx = None
    for i, e, cn in find_meshes(pkg):
        if pkg.name(e.object_name) == sel_name and cn == "Core.LodMesh":
            idx = i
            break
    if idx is None:
        raise RuntimeError(f"mesh {sel_name} not found")
    m = parse_lod_mesh(pkg, idx)

    # Derive from the wedge-referenced verts ONLY (the first SpecialVerts per
    # frame are invisible attachment points, e.g. female back straps at y=40).
    used = sorted({w[0] + m.special_verts for w in m.wedges})
    world = [mesh_to_world(m.verts[k], m) for k in used]
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

    # 2) Derive bones from geometry.
    centers, halves, rep = derive_bones(bind)

    # 2b) Guarantee non-adjacent clearance: the long forearm/upper-arm boxes must
    #     clear the thigh/shin boxes on the same side. Push the arm bone outward
    #     (never inward) by whatever the geometry needs + a small gap.
    centers = _enforce_clearance(centers, halves)

    # 3) Translate so pelvis sits at the origin (canonical convention).
    px, py, pz = centers[0]
    bind = [(v[0] - px, v[1] - py, v[2] - pz) for v in bind]
    centers = [(c[0] - px, c[1] - py, c[2] - pz) for c in centers]

    # 4) Anchors + bind (smooth multi-bone weights).
    anchors = compute_anchors(centers)
    binds = bind_verts(bind, centers, halves, rep["arm_idx"])
    bind_bones, bind_offsets, bind_weights = flatten_binds(binds, len(bind))

    # 5) Assertions (dominant bone per vertex, for the no-starvation/nearest check).
    per_bone = verify(bind, centers, halves,
                      [b[0][0] for b in binds], [b[0][2] for b in binds], sel_name)

    print(f"  yaw={yaw:+d} mirror={mir:.2f} legSplit={split0:.2f} (n={n_low}) H={rep['H']:.1f} "
          f"n_head={rep['n_head']} n_neck={rep['n_neck']} n_pelvis={rep['n_pelvis']} "
          f"n_spine={rep['n_spine']} n_arm={rep['n_arm']} n_leg={rep['n_leg']}")
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

    return {
        "live_name": live_name,
        "sel_name": sel_name,
        "verts": bind,
        "faces": m.faces,
        "wedges": wedges,
        "materials": m.materials,
        "centers": centers,
        "halves": halves,
        "anchors": anchors,
        "bind_bones": bind_bones,
        "bind_offsets": bind_offsets,
        "bind_weights": bind_weights,
        "per_bone": per_bone,
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
        # leg bones on the same side (L: 8,10,12; R: 9,11,13)
        leg_bones = [8, 10, 12] if leg_side == 0 else [9, 11, 13]
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
                need = max(need, lx + halves[lb][0] + gap)
        centers[arm_bone] = (sign * max(ax, need), centers[arm_bone][1],
                             centers[arm_bone][2])
    return centers


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

    # Mesh: one position per wedge (matches export_obj indexing).
    with open(mesh_path, "w") as f:
        f.write(f"# {base} bind-space frame 0 (yaw {d['yaw']:+d})\n")
        for w in d["wedges"]:
            p = d["verts"][w[0]]
            f.write(f"v {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}\n")
        for fa in d["faces"]:
            f.write(f"f {fa[0]+1} {fa[1]+1} {fa[2]+1}\n")

    # Skeleton: each bone is an OBJ group (o Bone_i) of 8 box verts + 6 quads.
    with open(skel_path, "w") as f:
        f.write(f"# {base} derived skeleton (14 boxes)\n")
        for b, c in enumerate(d["centers"]):
            h = d["halves"][b]
            f.write(f"o Bone_{b}_{BONE_NAMES[b]}\n")
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
    print(f"  wrote {mesh_path}\n  wrote {skel_path}")
    return mesh_path, skel_path


def render_verify(blender, mesh_path, skel_path, obj_dir, base):
    script = os.path.join(obj_dir, "render_verify.py")
    _write_render_script(script)
    # Blender resolves relative paths against its own CWD (C:\) — use absolute.
    mesh_path = os.path.abspath(mesh_path)
    skel_path = os.path.abspath(skel_path)
    front = os.path.abspath(os.path.join(obj_dir, f"{base}_front.png"))
    threeq = os.path.abspath(os.path.join(obj_dir, f"{base}_3q.png"))
    cmd = [blender, "-b", "--python-exit-code", "1", "-P", os.path.abspath(script),
           "--", mesh_path, skel_path, front, threeq]
    print("  blender:", " ".join(cmd))
    r = subprocess_run(cmd)
    if r.returncode != 0:
        print(f"  !! blender render failed (rc={r.returncode})")
    else:
        print(f"  wrote {front}\n  wrote {threeq}")


def _write_render_script(path):
    script = r'''
# Headless Blender overlay render: mesh (gray) + skeleton boxes (colored).
import bpy, sys, math
from mathutils import Vector
argv = sys.argv[sys.argv.index("--") + 1:]
mesh_obj, skel_obj, out_front, out_3q = argv
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.obj_import(filepath=mesh_obj)
bpy.ops.wm.obj_import(filepath=skel_obj)
scene = bpy.context.scene
palette = [(0.9,0.2,0.2),(0.2,0.9,0.2),(0.2,0.2,0.9),(0.9,0.9,0.2),(1,0.5,0),
           (1,0,0.5),(0.5,1,0),(0,1,1),(0.6,0.3,1),(0.9,0.6,0.2),(0.2,0.9,0.6),
           (0.9,0.2,0.8),(0.5,0.5,0.5),(1,1,1)]
def mat_color(name, col):
    m = bpy.data.materials.new(name)
    m.use_nodes = False
    m.diffuse_color = (col[0], col[1], col[2], 1.0)
    return m
meshes = [o for o in scene.objects if o.type == "MESH"]
for o in meshes:
    if "Bone_" in o.name:
        idx = int(o.name.split("_")[1])
        o.data.materials.append(mat_color(o.name, palette[idx % len(palette)]))
        o.show_wire = True
        o.show_all_edges = True
    else:
        o.data.materials.append(mat_color("MeshMat", (0.8, 0.8, 0.8)))
        o.data.materials[0].use_nodes = False
# Frame the whole body.
pts = [o.matrix_world @ v.co for o in meshes for v in o.data.vertices]
mn = Vector((min(v.x for v in pts), min(v.y for v in pts), min(v.z for v in pts)))
mx = Vector((max(v.x for v in pts), max(v.y for v in pts), max(v.z for v in pts)))
ctr = (mn + mx) / 2
rad = max((mx - mn).length * 1.5, 60.0)
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.color_type = "MATERIAL"
scene.display.shading.show_cavity = True
scene.render.resolution_x = 640
scene.render.resolution_y = 640
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
render_shot(out_front, (0.0, rad, rad * 0.45))
render_shot(out_3q, (rad * 0.75, rad * 0.75, rad * 0.55))
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
        d = build_rig(pkg, sel, live)
        built.append(d)
        mesh_path, skel_path = emit_obj_overlay(d, args.obj_dir)

    emit_header(built, args.out)

    if args.render:
        blender = os.environ.get("BLENDER_EXE") or shutil.which("blender")
        if not blender:
            print("!! no blender.exe (set BLENDER_EXE)")
        else:
            for d in built:
                base = d["sel_name"]
                mesh_path = os.path.join(args.obj_dir, f"{base}_mesh.obj")
                skel_path = os.path.join(args.obj_dir, f"{base}_skeleton.obj")
                render_verify(blender, mesh_path, skel_path, args.obj_dir, base)


if __name__ == "__main__":
    main(sys.argv[1:])
