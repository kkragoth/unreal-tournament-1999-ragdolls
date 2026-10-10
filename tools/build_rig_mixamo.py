"""Build the SelectionMale1 ragdoll dataset from the Mixamo-rigged T-pose FBX.

The user pushes the yaw-corrected front-facing SelectionMale1 OBJ (with UVs) to
mixamo.com, auto-rigs it, and downloads the T-pose FBX. That FBX carries:
  - the exact bind-space mesh (464 verts, 642 tris, UVs),
  - a 41-bone humanoid skeleton (Hips..Head/Shoulder/Arm/ForeArm/Hand/Thigh/Leg/
    Foot/Toe) with anatomically-correct joint positions,
  - per-vertex skin weights (Mixamo's auto-rig, far better than our geometric
    binding).

This tool builds the RagdollDatasets.h entry from the skeleton + weights:
  - 14 ragdoll bone centers = Mixamo joint positions (mid head/tail), mirror
    averaged L/R;
  - half extents = scaled bone lengths (skeleton spans), clamped;
  - 13 joint anchors = the Mixamo joint positions directly (shoulder/elbow/
    hip/knee/ankle/neck/head);
  - per-vertex binds = top-3 Mixamo skin weights, re-indexed to ragdoll bones;
  - verts/faces/wedges/materials/UVs straight from the FBX mesh.

Usage:
    python tools/build_rig_mixamo.py <tpose.json> --out-dir tools/rig_verify
    python tools/build_rig_mixamo.py <tpose.json> --header RagdollPkg/Src/RagdollDatasets.h
"""

import argparse
import json
import math
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_rig as br

DEFAULT_PKG = r"C:\Users\kkragoth\dev\Unreal Tournament\Unreal Tournament\System\BotPack.u"

# Mixamo bone name -> (ragdoll bone index). Finger/toe bones collapse into
# their parent limb; feet exist only so verts bind to the shin (ragdoll skips
# foot bodies, feet ride the shins).
MIXAMO_MAP = {
    "mixamorig:Hips": 0,
    "mixamorig:Spine": 1, "mixamorig:Spine1": 1, "mixamorig:Spine2": 1,
    "mixamorig:Neck": 2,
    "mixamorig:Head": 3, "mixamorig:HeadTop_End": 3,
    "mixamorig:LeftShoulder": 4, "mixamorig:LeftArm": 4,
    "mixamorig:RightShoulder": 5, "mixamorig:RightArm": 5,
    "mixamorig:LeftForeArm": 6, "mixamorig:LeftHand": 6,
    "mixamorig:LeftHandThumb1": 6, "mixamorig:LeftHandThumb2": 6,
    "mixamorig:LeftHandThumb3": 6, "mixamorig:LeftHandThumb4": 6,
    "mixamorig:LeftHandIndex1": 6, "mixamorig:LeftHandIndex2": 6,
    "mixamorig:LeftHandIndex3": 6, "mixamorig:LeftHandIndex4": 6,
    "mixamorig:RightForeArm": 7, "mixamorig:RightHand": 7,
    "mixamorig:RightHandThumb1": 7, "mixamorig:RightHandThumb2": 7,
    "mixamorig:RightHandThumb3": 7, "mixamorig:RightHandThumb4": 7,
    "mixamorig:RightHandIndex1": 7, "mixamorig:RightHandIndex2": 7,
    "mixamorig:RightHandIndex3": 7, "mixamorig:RightHandIndex4": 7,
    "mixamorig:LeftUpLeg": 8, "mixamorig:RightUpLeg": 9,
    "mixamorig:LeftLeg": 10, "mixamorig:RightLeg": 11,
    "mixamorig:LeftFoot": 12, "mixamorig:LeftToeBase": 12, "mixamorig:LeftToe_End": 12,
    "mixamorig:RightFoot": 13, "mixamorig:RightToeBase": 13, "mixamorig:RightToe_End": 13,
}

# Ragdoll bone index -> canonical joint anchor derived from the skeleton.
# Order matches BuildRagdollImpl's AddJoint calls (13 anchors):
#   pelvis-spine, spine-neck, neck-head, spine-upperarm L/R, elbow L/R,
#   pelvis-thigh L/R, knee L/R, ankle L/R
JOINT_BONES = [
    ("mixamorig:Spine", None),      # pelvis-spine
    ("mixamorig:Neck", None),       # spine-neck
    ("mixamorig:Head", None),       # neck-head
    ("mixamorig:LeftArm", "L"),     # spine-upperarm L (shoulder joint proper)
    ("mixamorig:RightArm", "R"),    # spine-upperarm R
    ("mixamorig:LeftForeArm", "L"),  # elbow L
    ("mixamorig:RightForeArm", "R"),  # elbow R
    ("mixamorig:LeftUpLeg", "L"),   # pelvis-thigh L
    ("mixamorig:RightUpLeg", "R"),  # pelvis-thigh R
    ("mixamorig:LeftLeg", "L"),     # knee L
    ("mixamorig:RightLeg", "R"),    # knee R
    ("mixamorig:LeftFoot", "L"),    # ankle L
    ("mixamorig:RightFoot", "R"),   # ankle R
]


def load(data):
    """Return (verts, faces[{verts,uvs}], bones[{name,head,tail,parent}], weights[])."""
    return data["verts"], data["faces"], data["bones"], data["weights"]


def bone_head(bones, name):
    for b in bones:
        if b["name"] == name:
            return b["head"]
    return None


def build_rig_mixamo(data, live_name="Commando", sel_name="SelectionMale1"):
    verts, faces, bones, weights = load(data)

    # --- 1) Per-vertex labels from the skin weights -----------------------
    # Every vertex gets its dominant ragdoll bone. Feet are skipped from the
    # ragdoll; their verts ride the shins. Neck gets the skull (Mixamo's fit
    # puts the whole low-poly head on the Neck bone), but the physics Head
    # body is still fed from the spatially top-most region so it has verts.
    labels = []
    for i, w in enumerate(weights):
        agg = {}
        for bname, wt in w.items():
            b = MIXAMO_MAP.get(bname)
            if b is None:
                continue
            if b in (12, 13):
                b = 10 if b == 12 else 11
            agg[b] = agg.get(b, 0.0) + wt
        labels.append(max(agg, key=agg.get) if agg else -1)

    # Spatial fallback: any unlabeled vert (or an empty bone) gets the nearest
    # ragdoll bone box by position. Head body gets the skull verts that Mixamo
    # put on the Neck bone (they are the top-most verts of the torso).
    skeleton_head_z = None
    hb = next((x for x in bones if x["name"] == "mixamorig:Head"), None)
    if hb:
        skeleton_head_z = hb["head"][2]

    # --- 2) Cluster centers/halves from the labeled verts -----------------
    clusters = [[] for _ in range(14)]
    for i, b in enumerate(labels):
        if b >= 0:
            clusters[b].append(i)
    # Merge the skull (top verts) into the Head cluster even though Mixamo
    # weighted them to Neck: the ragdoll spawns a Head body and the skull must
    # ride it, not the neck hinge.
    if skeleton_head_z is not None:
        for i, v in enumerate(verts):
            if v[2] > skeleton_head_z and labels[i] in (1, 2):
                clusters[3].append(i)
                labels[i] = 3
    # Any bone with no verts absorbs its nearest-positional neighbors.
    for b in range(12):
        if clusters[b]:
            continue
        src = clusters[b - 1] if b > 0 else clusters[b + 1]
        if not src:
            continue
        c = tuple(st.mean(verts[i][k] for i in src) for k in range(3))
        cand = sorted((i for i in range(len(verts)) if labels[i] >= 0),
                      key=lambda i: math.dist(verts[i], c))
        take = cand[:max(3, len(src) // 4)]
        for i in take:
            clusters[b].append(i)
            labels[i] = b

    def cen(idxs):
        if not idxs:
            return None
        return tuple(st.mean(verts[i][k] for i in idxs) for k in range(3))

    centers_raw = [cen(clusters[b]) for b in range(14)]

    # Mirror-average L/R pairs for a symmetric physics skeleton.
    def sym_pair(cl, cr):
        if cl is None or cr is None:
            return None
        ax = (abs(cl[0]) + abs(cr[0])) / 2.0
        ay = (cl[1] + cr[1]) / 2.0
        az = (cl[2] + cr[2]) / 2.0
        return (-ax, ay, az), (ax, ay, az)

    for b in (4, 6, 8, 10, 12):
        pair = sym_pair(centers_raw[b], centers_raw[b + 1])
        if pair:
            centers_raw[b], centers_raw[b + 1] = pair
    centers = []
    for i, c in enumerate(centers_raw):
        if c is None:
            # Synthesize from a neighbor so the skeleton stays complete.
            nb = centers_raw[i - 1] if i > 0 else centers_raw[i + 1]
            if nb is None:
                nb = (0.0, 0.0, 0.0)
            if i in (5, 7, 9, 11, 13):
                c = (-nb[0], nb[1], nb[2])
            else:
                c = nb
        centers.append(c)
    for i in range(4):
        if centers[i]:
            centers[i] = (0.0, centers[i][1], centers[i][2])

    # Half extents from cluster extents, capped (physics boxes are slimmer
    # than the visual surface so non-adjacent bodies don't interpenetrate).
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
    # Trunk stack ordering: Spine must stop where Head begins.
    top_spine = centers[1][2] + halves[1][2]
    bot_head = centers[3][2] - halves[3][2]
    if top_spine > bot_head:
        mid = (top_spine + bot_head) / 2.0
        halves[1] = (halves[1][0], halves[1][1], max(1.0, mid - centers[1][2]))
        halves[3] = (halves[3][0], halves[3][1], max(1.0, centers[3][2] - mid))
    # Shins span thigh-bottom -> foot-bottom so the feet ride them.
    mnz = min(v[2] for v in verts)
    for b, thigh in [(10, 8), (11, 9)]:
        knee_top = centers[thigh][2] - halves[thigh][2]
        new_cz = (knee_top + mnz) / 2.0
        new_hz = (knee_top - mnz) / 2.0 + 1.0
        centers[b] = (centers[b][0], centers[b][1], new_cz)
        halves[b] = (halves[b][0], halves[b][1], new_hz)

    # --- 3) Joint anchors from the skeleton -------------------------------
    def joint_pos(name, side):
        b = next((x for x in bones if x["name"] == name), None)
        if b is None:
            return None
        return b["head"]

    anchors = []
    for name, side in JOINT_BONES:
        p = joint_pos(name, side)
        if p is None:
            p = (0.0, 0.0, 0.0)
        anchors.append(p)

    # --- 4) Translate so pelvis center sits at the origin ----------------
    px, py, pz = centers[0]
    verts = [(v[0] - px, v[1] - py, v[2] - pz) for v in verts]
    centers = [(c[0] - px, c[1] - py, c[2] - pz) for c in centers]
    anchors = [(a[0] - px, a[1] - py, a[2] - pz) for a in anchors]

    # --- 5) Per-vertex binds from skin weights (top-3, normalized) -------
    bind_bones = []
    bind_offsets = []
    bind_weights = []
    n_bone_verts = [0] * 14
    for i, w in enumerate(weights):
        agg = {}
        for bname, wt in w.items():
            b = MIXAMO_MAP.get(bname)
            if b is None:
                continue
            if b in (12, 13):
                b = 10 if b == 12 else 11
            agg[b] = agg.get(b, 0.0) + wt
        # The physics Head body must own its skull: verts the cluster pass
        # assigned to Head (skull merged from Neck) bind there, overriding
        # Mixamo's neck-only weight so the skull follows the head body.
        if labels[i] == 3:
            agg[3] = agg.get(3, 0.0) + max(agg.values()) if agg else 1.0
        top = sorted(agg.items(), key=lambda kv: -kv[1])[:3]
        tot = sum(v for _, v in top) or 1.0
        top = [(b, wt / tot) for b, wt in top]
        for b, wt in top:
            n_bone_verts[b] += 1
        for k in range(3):
            if k < len(top):
                b, wt = top[k]
                c = centers[b]
                bind_bones.append(b)
                bind_offsets.append((verts[i][0] - c[0],
                                     verts[i][1] - c[1],
                                     verts[i][2] - c[2]))
                bind_weights.append(wt)
            else:
                bind_bones.append(0)
                bind_offsets.append((0.0, 0.0, 0.0))
                bind_weights.append(0.0)

    # --- 6) Mesh geometry (verts/faces/wedges/materials) ------------------
    # One wedge per face corner (vertex + uv), dedup not needed (the dataset
    # supports 1:1; the renderer uses Wedges(iWedge).iVertex).
    wedges = []
    faces_out = []
    src_mats = data.get("materials")
    for fi, f in enumerate(faces):
        ws = f["verts"]
        uvs = f.get("uvs") or [(0.0, 0.0)] * 3
        base = len(wedges)
        for k in range(3):
            u = uvs[k][0] * 255.0 if uvs else 0.0
            v = uvs[k][1] * 255.0 if uvs else 0.0
            wedges.append((ws[k], max(0, min(255, round(u))), max(0, min(255, round(v)))))
        mi = f.get("mat", 0) if src_mats else 0
        faces_out.append((base, base + 1, base + 2, mi))
    if src_mats:
        # tools/tpose_rig.py rigs carry the source mesh materials: keep the
        # authored texture slots (MultiSkins 0..3 resolve per material in the
        # renderer) and force PF_TwoSided so twisted limbs show no holes.
        materials = [(int(fl) | 0x00000100, int(ti)) for fl, ti in src_mats]
    else:
        materials = [(0x00000100, 0)]  # PF_TwoSided, texture slot 0 (body skin)

    per_bone = n_bone_verts
    print("  per-bone verts:", per_bone)
    for b in range(12):
        if per_bone[b] < 2:
            print(f"  !! bone {br.BONE_NAMES[b]} under-weighted ({per_bone[b]})")

    return {
        "live_name": live_name,
        "sel_name": sel_name,
        "verts": verts,
        "faces": faces_out,
        "wedges": wedges,
        "materials": materials,
        "centers": centers,
        "halves": halves,
        "anchors": anchors,
        "bind_bones": bind_bones,
        "bind_offsets": bind_offsets,
        "bind_weights": bind_weights,
        "per_bone": per_bone,
        "yaw": 0,
        "mirror": 0.0,
        "mesh": None,
    }


def emit_objs(d, obj_dir):
    import os
    os.makedirs(obj_dir, exist_ok=True)
    base = d["sel_name"]
    mesh_path = os.path.join(obj_dir, f"{base}_mixamo_mesh.obj")
    skel_path = os.path.join(obj_dir, f"{base}_mixamo_skeleton.obj")
    with open(mesh_path, "w") as f:
        f.write(f"# {base} Mixamo bind-space mesh\n")
        for p in d["verts"]:
            f.write(f"v {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}\n")
        for w in d["wedges"]:
            f.write(f"vt {w[1]/255:.4f} {w[2]/255:.4f}\n")
        for fa in d["faces"]:
            f.write(f"f {fa[0]+1}/{fa[0]+1} {fa[1]+1}/{fa[1]+1} {fa[2]+1}/{fa[2]+1}\n")
    with open(skel_path, "w") as f:
        f.write(f"# {base} Mixamo skeleton (12 boxes)\n")
        for b, c in enumerate(d["centers"][:12]):
            h = d["halves"][b]
            f.write(f"o Skel_{b}_{br.BONE_NAMES[b]}\n")
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


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("fbx_json")
    ap.add_argument("--header", default=None)
    ap.add_argument("--obj-dir", default="tools/rig_verify")
    ap.add_argument("--live", default="Commando")
    ap.add_argument("--sel", default="SelectionMale1")
    ap.add_argument("--mixamo-dir", default="tools/rigs",
                    help="dir of <Sel>.json rigs: tools/tpose_rig.py output "
                         "(default) or fbx_to_json.py dumps (tools/mixamo_rigs)")
    ap.add_argument("--pkg", default=DEFAULT_PKG,
                    help="Botpack.u, only needed for un-rigged models (legacy fallback)")
    args = ap.parse_args(argv)

    with open(args.fbx_json) as f:
        data = json.load(f)
    d = build_rig_mixamo(data, args.live, args.sel)
    emit_objs(d, args.obj_dir)

    # Print a skeleton summary for verification.
    print("  pelvis=", tuple(round(x, 1) for x in d["centers"][0]))
    print("  spine =", tuple(round(x, 1) for x in d["centers"][1]))
    print("  head  =", tuple(round(x, 1) for x in d["centers"][3]))
    print("  upArmL=", tuple(round(x, 1) for x in d["centers"][4]))
    print("  foreL =", tuple(round(x, 1) for x in d["centers"][6]))
    print("  thighL=", tuple(round(x, 1) for x in d["centers"][8]))
    print("  shinL =", tuple(round(x, 1) for x in d["centers"][10]))
    print("  ankleL=", tuple(round(x, 1) for x in d["anchors"][11]))

    if args.header:
        # Rebuild the header: this model's Mixamo dataset first, then every
        # other model that has a <Sel>.json in --mixamo-dir gets its own Mixamo
        # dataset, and only models without a Mixamo rig fall back to the legacy
        # pipeline (topo fails on some models — don't use it here).
        pkg = None
        done = {args.sel}
        built = [d]
        for live, sel in br.MODEL_MAP:
            if sel in done:
                continue
            j = os.path.join(args.mixamo_dir, f"{sel}.json")
            if os.path.exists(j):
                with open(j) as f:
                    data = json.load(f)
                print(f"\n=== {sel} (live: {live}) Mixamo dataset ===")
                built.append(build_rig_mixamo(data, live, sel))
                done.add(sel)
        for live, sel in br.MODEL_MAP:
            if sel in done:
                continue
            print(f"\n=== {sel} (live: {live}) legacy fallback ===")
            if pkg is None:
                pkg = br.UTPackage(args.pkg)
            b = br.build_rig(pkg, sel, live, bind_mode="legacy")
            built.append(b)
        br.emit_header(built, args.header)


if __name__ == "__main__":
    main(sys.argv[1:])
