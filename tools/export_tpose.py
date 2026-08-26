"""Export a Selection viewer mesh re-posed into a T-pose, WITH UVs, for Mixamo.

Mixamo's auto-rig needs a clean T-pose input: the stock Selection meshes hang
the hands down by the ankles, which makes Mixamo weight the hanging hands to
the FOOT bones (verified on the first T-Pose.fbx upload: HAND z=-37 ->
LeftFoot 0.68) and the OBJ from tools/rig_verify has no UVs, so the returned
FBX has none either.

This tool fixes both:
  1. Rotates the arm column (|x|>arm_thr, foot..shoulder) out to horizontal
     about the derived shoulder joint -> a straight T-pose. Hands leave the
     legs, so Mixamo can rig the arms correctly.
  2. Writes a Wavefront OBJ with one vertex + one UV per SOURCE WEDGE (the
     game mesh carries real UVs, 1..253), so Mixamo preserves the texture map.

Output: <obj-dir>/<model>_tpose.obj  (upload this to mixamo.com)

Usage:
    python tools/export_tpose.py <Botpack.u> [--model SelectionMale1]
                                  [--obj-dir tools/rig_verify] [--render]
"""

import argparse
import math
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_rig as br


def pose_tpose(bind, centers, rep, arm_thr=11.0):
    """Return verts with the arms rotated to horizontal about the shoulders.

    The bind pose hangs the arms straight down (hands at the ankles). To make
    a T-pose we rotate the whole arm column (|x|>arm_thr, from the feet up to
    the shoulder line) about the derived shoulder joint by -/+90 deg about the
    Y axis: the right arm (+x side) swings out to +X, the left (-x) to -X.
    Verts closer to the body than arm_thr (deltoid/chest) stay on the torso, so
    the armpit seam stays attached.
    """
    out = [v[:] for v in bind]
    # Shoulder joint: upper-arm box top, on the outer arm line (from
    # compute_anchors' shldr_z / arm_x math).
    shldr_z = centers[4][2] + (centers[4][2] - centers[6][2]) / 2.0
    arm_y = (centers[4][1] + centers[6][1]) / 2.0
    for i in rep["arm_idx"]:
        side = -1.0 if bind[i][0] < 0 else 1.0
        px = side * arm_thr
        dx, dy, dz = bind[i][0] - px, bind[i][1] - arm_y, bind[i][2] - shldr_z
        a = math.radians(-90.0 * side)   # right: -90, left: +90 about +Y
        ca, sa = math.cos(a), math.sin(a)
        nx = dx * ca + dz * sa
        nz = -dx * sa + dz * ca
        out[i] = (px + nx, arm_y + dy, shldr_z + nz)
    return out


def export_tpose_obj(m, bind, wedges, faces, out_path):
    """Write an OBJ with one position + one UV per source wedge (T-posed)."""
    with open(out_path, "w") as f:
        f.write(f"# {m.name} T-pose (Mixamo upload), bind-space, pelvis origin\n")
        for w in wedges:
            p = bind[w[0]]
            f.write(f"v {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}\n")
        for w in wedges:
            f.write(f"vt {w[1] / 255.0:.4f} {w[2] / 255.0:.4f}\n")
        for fa in faces:
            f.write(f"f {fa[0] + 1}/{fa[0] + 1} {fa[1] + 1}/{fa[1] + 1} {fa[2] + 1}/{fa[2] + 1}\n")
    print(f"  wrote {out_path} ({len(wedges)} wedges, {len(faces)} faces)")


def render_check(blender, obj_path, out_dir, base):
    """Headless Blender render of the T-pose OBJ so the user can verify before
    uploading. Reuses the rig_verify render script (reads argv: mesh, skel,
    out_front, out_3q, out_side) with a dummy empty skeleton file."""
    script = os.path.join(out_dir, "render_verify.py")
    # Point at the repo's existing render script (it already exists there).
    if not os.path.exists(script):
        print(f"  !! render script missing at {script}; install tools/rig_verify first")
        return
    import subprocess
    skel = os.path.join(out_dir, "_empty.obj")
    with open(skel, "w") as f:
        f.write("")
    obj_path = os.path.abspath(obj_path)
    front = os.path.abspath(os.path.join(out_dir, f"{base}_tpose_front.png"))
    threeq = os.path.abspath(os.path.join(out_dir, f"{base}_tpose_3q.png"))
    side = os.path.abspath(os.path.join(out_dir, f"{base}_tpose_side.png"))
    cmd = [blender, "-b", "--python-exit-code", "1", "-P", os.path.abspath(script),
           "--", obj_path, skel, front, threeq, side]
    print("  blender:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  !! render failed (rc={r.returncode})\n{r.stderr[-1200:]}")
    else:
        print(f"  wrote {front}\n  wrote {threeq}\n  wrote {side}")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg")
    ap.add_argument("--model", default="SelectionMale1")
    ap.add_argument("--obj-dir", default="tools/rig_verify")
    ap.add_argument("--render", action="store_true",
                    help="render PNGs with headless Blender (needs blender.exe)")
    args = ap.parse_args(argv)

    pkg = br.UTPackage(args.pkg)
    d = br.build_rig(pkg, args.model, args.model, bind_mode="legacy")
    bind = pose_tpose(d["verts"], d["centers"], d["report"])
    m = d["mesh"]

    # Wedges with remapped dense-vert indices + source UVs (same as build_rig's
    # final wedge list; rebuild here so we own the remap).
    used = sorted({w[0] + m.special_verts for w in m.wedges})
    remap = {old: new for new, old in enumerate(used)}
    wedges = [(remap[w[0] + m.special_verts], w[1], w[2]) for w in m.wedges]
    faces = list(m.faces)   # complete source mesh (legacy mode drops none here)

    os.makedirs(args.obj_dir, exist_ok=True)
    out_path = os.path.join(args.obj_dir, f"{args.model}_tpose.obj")
    export_tpose_obj(m, bind, wedges, faces, out_path)

    # Report the pose + bbox so the user can sanity-check before uploading.
    xs = [p[0] for p in bind]; ys = [p[1] for p in bind]; zs = [p[2] for p in bind]
    print(f"  T-pose bbox X {min(xs):.1f}..{max(xs):.1f}  Y {min(ys):.1f}..{max(ys):.1f}"
          f"  Z {min(zs):.1f}..{max(zs):.1f}")
    print(f"  hands now at x~{max(xs):.1f} (were ~13); body height {max(zs)-min(zs):.1f}")

    if args.render:
        blender = os.environ.get("BLENDER_EXE") or shutil.which("blender")
        if not blender:
            print("  !! no blender.exe (set BLENDER_EXE)")
        else:
            render_check(blender, out_path, args.obj_dir, args.model)

    print(f"\nUPLOAD THIS: {os.path.abspath(out_path)}")


if __name__ == "__main__":
    main(sys.argv[1:])
