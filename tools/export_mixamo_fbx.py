"""Export a Selection viewer mesh yaw-corrected to front-facing bind space,
WITH UVs, as an OBJ for Mixamo upload.

Mixamo auto-rig fails on the raw viewer frame (3/4-turn, asymmetric). The
yaw-corrected bind space (mirror-score + leg-split, from build_rig) is a
symmetric front-facing humanoid — what Mixamo expects. UVs are written per
source wedge (one position per wedge, like the game's own wedge layout), so
Mixamo preserves the texture map.

Usage:
    python tools/export_mixamo_fbx.py <Botpack.u> [--model SelectionMale1]
                                       [--out dir]
"""

import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_rig as br


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg")
    ap.add_argument("--model", default="SelectionMale1")
    ap.add_argument("--obj-dir", default="tools/rig_verify")
    ap.add_argument("--blender", default=None,
                    help="path to blender.exe (default: BLENDER_EXE or on PATH)")
    args = ap.parse_args(argv)

    pkg = br.UTPackage(args.pkg)
    d = br.build_rig(pkg, args.model, args.model, bind_mode="legacy")
    bind = d["verts"]          # yaw-corrected, pelvis at origin
    m = d["mesh"]

    # Rebuild the wedge list ourselves so we own the remap, then use the
    # COMPLETE source mesh faces (m.faces, 642) — Mixamo auto-rig fails with
    # "Unknown error while generating motion" on meshes with dropped faces
    # (the legacy build_rig weld-drop leaves rim holes).
    used = sorted({w[0] + m.special_verts for w in m.wedges})
    remap = {old: new for new, old in enumerate(used)}
    wedges = [(remap[w[0] + m.special_verts], w[1], w[2]) for w in m.wedges]
    faces = list(m.faces)

    # Per-wedge positions + per-wedge UVs (1:1 with the source wedges).
    obj_path = os.path.join(args.obj_dir, f"{args.model}_mixamo.obj")
    with open(obj_path, "w") as f:
        f.write(f"# {args.model} yaw-corrected front-facing, pelvis origin, UVs (complete mesh)\n")
        for w in wedges:
            p = bind[w[0]]
            f.write(f"v {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}\n")
        for w in wedges:
            f.write(f"vt {w[1] / 255.0:.4f} {w[2] / 255.0:.4f}\n")
        for fa in faces:
            f.write(f"f {fa[0] + 1}/{fa[0] + 1} {fa[1] + 1}/{fa[1] + 1} {fa[2] + 1}/{fa[2] + 1}\n")
    print(f"  wrote {os.path.abspath(obj_path)} ({len(wedges)} wedges, {len(faces)} faces)")

    # Convert to FBX via Blender (Z-up, preserve UVs).
    blender = args.blender or os.environ.get("BLENDER_EXE") or shutil.which("blender")
    if not blender:
        print("  !! no blender.exe; upload the OBJ directly (Mixamo accepts OBJ)")
        return
    fbx_path = os.path.join(args.obj_dir, f"{args.model}_mixamo.fbx")
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "obj_to_fbx.py")
    if not os.path.exists(script):
        print(f"  !! {script} missing; upload the OBJ directly")
        return
    r = subprocess.run([blender, "-b", "--python-exit-code", "1", "-P", script,
                        "--", os.path.abspath(obj_path), os.path.abspath(fbx_path)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  !! blender failed (rc={r.returncode}): {r.stderr[-800:]}")
    else:
        print(f"\nUPLOAD THIS: {os.path.abspath(fbx_path)}")


if __name__ == "__main__":
    main(sys.argv[1:])
