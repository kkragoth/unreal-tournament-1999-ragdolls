"""Offline ragdoll T-pose frame analysis.

For every animation frame of a player mesh, score how well the frame's
geometry aligns with the ragdoll skeleton's spread rest pose
(GRagdollBoneDefs, RagdollCarcass.cpp lines 44-60). Reproduces the native
frame-picker scoring in CreatePlayerRagdollMesh (UnRagdollMesh.cpp) and adds
targeted T-pose metrics so we can see WHY a frame wins or fails, without the
build/deploy/playtest loop.

Metrics per frame (all in actor-local space, DrawScale=1):
  Fit / LongAxis      - native FitScale: SkeletonBodyLen / longest bbox axis
  PerBone[0..13]      - native nearest-bone-center assignment counts
  MinBone, Range, Mean - native picker criteria (min desc, range asc, mean asc)
  ArmSpread           - frac of upper-body verts (Z>10) with |X|>10
                        (arms held out clear of the +/-8 torso = T-pose arms)
  LegAlign            - mean |X| of lower-body verts (Z<-5), near 7 = legs
                        straight down on the +-7 skeleton leg line
  LegInX              - frac of lower-body verts with |X| in [2,14]
  InBox               - overall frac of verts inside their assigned bone's box

Usage:
    python3 ragdoll_frame_analysis.py <Botpack.u> [export_idx] [--top N] [--obj]
"""

import csv
import os
import sys
from dataclasses import dataclass

from extract_mesh import UTPackage, parse_lod_mesh, ut_rotation_matrix, mesh_to_world


# GRagdollBoneDefs from RagdollCarcass.cpp lines 44-60:
# (HX, HY, HZ, CX, CY, CZ, MassKg) - half extents + center offset from pelvis.
BONES = [
    (7.0, 7.0, 5.0, 0.0, 0.0, 0.0, 8.0),    # 0 Pelvis
    (8.0, 6.0, 10.0, 0.0, 0.0, 16.0, 10.0),  # 1 Spine
    (3.0, 3.0, 4.5, 0.0, 0.0, 25.0, 2.0),    # 2 Neck
    (4.5, 4.5, 5.0, 0.0, 0.0, 33.0, 4.0),    # 3 Head
    (2.0, 2.0, 7.0, -13.0, 6.0, 24.0, 3.0),  # 4 UpperArmL
    (2.0, 2.0, 7.0, 13.0, 6.0, 24.0, 3.0),   # 5 UpperArmR
    (1.5, 1.5, 6.0, -13.0, 8.0, 6.0, 2.0),   # 6 ForearmL
    (1.5, 1.5, 6.0, 13.0, 8.0, 6.0, 2.0),    # 7 ForearmR
    (3.0, 3.0, 8.0, -7.0, 0.0, -12.0, 6.0),  # 8 ThighL
    (3.0, 3.0, 8.0, 7.0, 0.0, -12.0, 6.0),   # 9 ThighR
    (2.5, 2.5, 8.0, -7.0, 0.0, -30.0, 3.0),  # 10 ShinL
    (2.5, 2.5, 8.0, 7.0, 0.0, -30.0, 3.0),   # 11 ShinR
    (2.0, 4.0, 2.5, -6.5, 2.0, -37.0, 1.0),  # 12 FootL
    (2.0, 4.0, 2.5, 6.5, 2.0, -37.0, 1.0),   # 13 FootR
]
BONE_NAMES = ["Pelvis", "Spine", "Neck", "Head", "UpArmL", "UpArmR",
              "ForArmL", "ForArmR", "ThighL", "ThighR", "ShinL", "ShinR",
              "FootL", "FootR"]

SKELETON_BODY_LEN = (BONES[3][5] + BONES[3][2]) - (BONES[12][5] - BONES[12][2])
# = (33 + 5) - (-37 - 2.5) = 77.5


@dataclass
class FrameScore:
    f: int
    seq: str
    fit: float
    long_axis: float
    per_bone: list
    min_bone: int
    range: int
    mean: float
    arm_spread: float
    leg_align: float
    leg_inx: float
    in_box: float
    bbox: tuple
    standing: bool
    fit_minz: float
    fit_maxz: float


def world_verts(m, frame):
    """Actor-local verts of one frame: (vert-Origin) scaled, then rotated by RotOrigin.

    Matches mesh_to_world() in extract_mesh (engine GetFrame order).
    """
    from extract_mesh import mesh_to_world
    base = frame * m.frame_verts
    return [mesh_to_world(m.verts[base + k], m) for k in range(m.frame_verts)]


def score_frame(m, verts, frame, seq):
    n = len(verts)
    mn = [min(v[i] for v in verts) for i in range(3)]
    mx = [max(v[i] for v in verts) for i in range(3)]
    fd = (mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2])
    long_axis = max(fd)
    fit = SKELETON_BODY_LEN / long_axis if long_axis > 0 else 1.0
    fit_minz = mn[2] * fit
    fit_maxz = mx[2] * fit

    # Standing-neutral gate (feeds the "player character standing" look):
    #  - Z span in a normal human-height band (excludes crouches below, and the
    #    arms-up "spike" dead frames that reach 120-175)
    #  - arms present: X span >= 0.7*Z (a spike corpse is narrow, ~0.2)
    #  - feet reach the skeleton foot line and head reaches the head line after
    #    the body-length fit (excludes feet-up victory/jump poses)
    zs, xs, ys = fd[2], fd[0], fd[1]
    standing = (67.0 <= zs <= 105.0
                and 0.7 * zs <= xs <= 1.5 * zs
                and ys <= zs
                and fit_minz <= -30.0
                and fit_maxz >= +30.0)

    per_bone = [0] * 14
    dist_sum = 0.0
    in_box_total = 0
    for p in verts:
        px, py, pz = p[0] * fit, p[1] * fit, p[2] * fit
        best = 0
        best_d = 1e30
        for b, (hx, hy, hz, cx, cy, cz, _) in enumerate(BONES):
            dx = px - cx
            dy = py - cy
            dz = pz - cz
            d = dx * dx + dy * dy + dz * dz
            if d < best_d:
                best_d = d
                best = b
        per_bone[best] += 1
        dist_sum += best_d ** 0.5
        hx, hy, hz, cx, cy, cz, _ = BONES[best]
        if abs(px - cx) <= hx and abs(py - cy) <= hy and abs(pz - cz) <= hz:
            in_box_total += 1

    max_count = max(per_bone)
    min_count = min(per_bone)

    # Arm spread: upper-body verts (Z > 10) with |X| > 10 (clear of +/-8 torso).
    upper = [p for p in verts if p[2] > 10.0]
    arm_spread = (sum(1 for p in upper if abs(p[0]) > 10.0) / len(upper)
                  if upper else 0.0)
    # Leg alignment: lower-body verts (Z < -5) - only legs live down there.
    lower = [p for p in verts if p[2] < -5.0]
    if lower:
        leg_align = sum(abs(p[0]) for p in lower) / len(lower)
        leg_inx = sum(1 for p in lower if 2.0 <= abs(p[0]) <= 14.0) / len(lower)
    else:
        leg_align = 0.0
        leg_inx = 0.0

    return FrameScore(
        f=frame, seq=seq, fit=fit, long_axis=long_axis,
        per_bone=per_bone, min_bone=min_count, range=max_count - min_count,
        mean=dist_sum / n, arm_spread=arm_spread, leg_align=leg_align,
        leg_inx=leg_inx, in_box=in_box_total / n,
        bbox=(mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]),
        standing=standing, fit_minz=fit_minz, fit_maxz=fit_maxz,
    )


def seq_of_frame(m, frame, pkg):
    """Name of the tightest seq containing frame, else 'All'."""
    best = None
    for name, _, start, num, _ in m.anim_seqs:
        if start <= frame < start + num:
            if best is None or num < best[1]:
                best = (pkg.name(name), num)
    return best[0] if best else "?"


def export_skeleton_obj(out_path, mesh=None):
    """Draw the skeleton's 14 rest-pose boxes as an OBJ for Blender overlay.

    The boxes are defined in skeleton rest space (pelvis at origin, Z up,
    toes +Y). When a mesh is passed, corners are transformed by the same
    mesh_to_world transform so the boxes land in the mesh's world frame
    (matches the native ragdoll: skeleton placed in the mesh's actor frame).
    """
    with open(out_path, "w") as f:
        f.write("# ragdoll skeleton rest pose (GRagdollBoneDefs)\n")
        for b, (hx, hy, hz, cx, cy, cz, _) in enumerate(BONES):
            base = b * 8
            for cx0 in (-1, 1):
                for cy0 in (-1, 1):
                    for cz0 in (-1, 1):
                        v = (cx + hx * cx0, cy + hy * cy0, cz + hz * cz0)
                        if mesh is not None:
                            v = mesh_to_world(v, mesh)
                        f.write(f"v {v[0]:.2f} {v[1]:.2f} {v[2]:.2f}\n")
            faces = [(0, 3, 2, 1), (4, 5, 6, 7), (1, 5, 6, 2),
                     (0, 4, 7, 3), (3, 7, 6, 2), (0, 1, 5, 4)]
            for q in faces:
                f.write(f"f {base+q[0]+1} {base+q[1]+1} {base+q[2]+1}\n")
                f.write(f"f {base+q[0]+1} {base+q[2]+1} {base+q[3]+1}\n")
    print(f"  wrote {out_path}")


def export_frame_obj(pkg, m, frame, out_path):
    from extract_mesh import export_obj
    export_obj(pkg, m, out_path, frame=frame)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        sys.exit(2)
    pkg = UTPackage(argv[1])
    idx = int(argv[2]) if len(argv) > 2 and argv[2].lstrip("-").isdigit() else 3251
    top_n = 10
    want_obj = False
    if "--top" in argv:
        top_n = int(argv[argv.index("--top") + 1])
    if "--obj" in argv:
        want_obj = True

    m = parse_lod_mesh(pkg, idx)
    print(f"== frame analysis {m.name} ({m.frame_verts} verts, {m.anim_frames} frames, "
          f"{len(m.anim_seqs)} seqs) skelBodyLen={SKELETON_BODY_LEN:.1f}")

    scores = []
    for f in range(m.anim_frames):
        verts = world_verts(m, f)
        scores.append(score_frame(m, verts, f, seq_of_frame(m, f, pkg)))

    # Rank by the native picker criteria: min desc, range asc, mean asc.
    ranked = sorted(scores, key=lambda s: (-s.min_bone, s.range, s.mean))
    print(f"\n=== top {top_n} by native picker (min bone count desc) ===")
    print("  #   f seq      fit   min range mean  arm  legX  legAlign inBox  longAx")
    for i, s in enumerate(ranked[:top_n]):
        print(f"  {i:2d} {s.f:4d} {s.seq:<8s} {s.fit:5.2f} {s.min_bone:3d} {s.range:4d} "
              f"{s.mean:5.1f} {s.arm_spread:4.2f} {s.leg_inx:4.2f} {s.leg_align:5.1f} "
              f"{s.in_box:5.2f} {s.long_axis:6.1f}")

    # Standing-gated picker: only neutral standing frames (see score_frame),
    # then the same coverage criteria. This is the criterion to port to native.
    standing_frames = [s for s in scores if s.standing]
    stand_ranked = sorted(standing_frames, key=lambda s: (-s.min_bone, s.range, s.mean))
    print(f"\n=== top {top_n} STANDING picker ({len(standing_frames)}/700 pass) ===")
    print("  #   f seq      fit   min range mean  arm  legX  legAlign inBox  zspan xspan")
    for i, s in enumerate(stand_ranked[:top_n]):
        print(f"  {i:2d} {s.f:4d} {s.seq:<8s} {s.fit:5.2f} {s.min_bone:3d} {s.range:4d} "
              f"{s.mean:5.1f} {s.arm_spread:4.2f} {s.leg_inx:4.2f} {s.leg_align:5.1f} "
              f"{s.in_box:5.2f} {s.bbox[5] - s.bbox[2]:5.1f} {s.bbox[3] - s.bbox[0]:5.1f}")

    def top_by(key, label):
        sub = sorted(scores, key=lambda s: -getattr(s, key))
        print(f"\n=== top {top_n} by {label} ===")
        print("  #   f seq      fit   min range  mean  arm  legX  legAlign inBox")
        for i, s in enumerate(sub[:top_n]):
            print(f"  {i:2d} {s.f:4d} {s.seq:<8s} {s.fit:5.2f} {s.min_bone:3d} {s.range:4d} "
                  f"{s.mean:5.1f} {s.arm_spread:4.2f} {s.leg_inx:4.2f} {s.leg_align:5.1f} "
                  f"{s.in_box:5.2f}")
        return sub[:top_n]

    best_arm = top_by("arm_spread", "arm spread")
    best_leg = top_by("leg_inx", "leg alignment")
    best_inbox = top_by("in_box", "in-box fit")

    # CSV dump of every frame.
    csv_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "ragdoll_frames.csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["frame", "seq", "fit", "long_axis", "min_bone", "range",
                    "mean", "arm_spread", "leg_inx", "leg_align", "in_box",
                    "standing", "fit_minz", "fit_maxz",
                    "bbox_minx", "bbox_miny", "bbox_minz",
                    "bbox_maxx", "bbox_maxy", "bbox_maxz",
                    ] + [f"b{i}_{n}" for i, n in enumerate(BONE_NAMES)])
        for s in scores:
            w.writerow([s.f, s.seq, round(s.fit, 4), round(s.long_axis, 2),
                        s.min_bone, s.range, round(s.mean, 2),
                        round(s.arm_spread, 4), round(s.leg_inx, 4),
                        round(s.leg_align, 2), round(s.in_box, 4),
                        int(s.standing), round(s.fit_minz, 2), round(s.fit_maxz, 2),
                        *[round(x, 2) for x in s.bbox],
                        *s.per_bone])
    print(f"\nwrote {csv_path} ({len(scores)} frames)")

    if want_obj:
        tmp = os.environ.get("TEMP", "/tmp")
        skel = os.path.join(tmp, "ragdoll_skeleton.obj")
        export_skeleton_obj(skel, mesh=m)
        for label, s in [("picker1", ranked[0]), ("picker2", ranked[1]),
                         ("picker3", ranked[2]), ("arm", best_arm[0]),
                         ("leg", best_leg[0]), ("inbox", best_inbox[0])]:
            out = os.path.join(tmp, f"ragdoll_{label}_f{s.f}.obj")
            export_frame_obj(pkg, m, s.f, out)


if __name__ == "__main__":
    main(sys.argv)
