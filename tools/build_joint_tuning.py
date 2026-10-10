#!/usr/bin/env python3
"""Per-model ragdoll joint tuning -> RagdollPkg/Src/RagdollJointTuning.h.

Each model (dataset) can carry its own joint table in a hand-edited
``tools/rigs/<Sel>.tuning.json``.  This script turns those files into the
``GRagdollJointTuning[]`` table the native build compiles in; at runtime
``BuildRagdollImpl`` looks the dataset name up and uses the table instead of
the global defaults (the console multipliers ``JointLimitScale`` /
``JointFrictionScale`` still apply on top).  A model without a tuning file,
or a joint missing from one, keeps the global defaults.

Usage::

    python tools/build_joint_tuning.py --init      # write missing <Sel>.tuning.json with the defaults
    python tools/build_joint_tuning.py             # (re)generate RagdollPkg/Src/RagdollJointTuning.h

Then rebuild the DLL (no ``ucc make`` needed: nothing changes on the script
side).

Tuning file format (angles in degrees, friction in Nm, anchor offsets in
Unreal units of the canonical bind pose, +X = model's left, +Y = front,
+Z = up)::

    {
      "dataset": "SelectionFemale1",
      "joints": {
        "spine":      {"type": "ball",  "cone_deg": 34.4, "twist_deg": [-22.9, 22.9], "friction_nm": 0.8,  "cone_axis": [0, 0, 0], "anchor_offset_uu": [0, 0, 0]},
        "forearm_l":  {"type": "hinge", "range_deg": [-2.9, 126.1], "rest_deg": 20.1, "friction_nm": 0.15, "anchor_offset_uu": [0, 0, 0]},
        ...
      }
    }

Joint names, in native order (child body of each joint): spine, neck, head,
upperarm_l, upperarm_r, forearm_l, forearm_r, thigh_l, thigh_r, shin_l,
shin_r.  Ball joints: cone half-angle (Box3D clamps it to 90 deg) around
``cone_axis`` (bind-space direction the limb points at the cone's centre;
[0,0,0] = the hanging limb) + twist range about the limb axis.  Hinges: one-sided flexion range (elbow positive =
forward flex, knee negative = backward flex) plus ``rest_deg``, the angle the
joint spring pulls toward (slightly flexed: elbow +20, knee -15); see
AddJoint in RagdollCarcass.cpp for the axis conventions.
"""
import argparse
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

JOINTS = ["spine", "neck", "head", "upperarm_l", "upperarm_r", "forearm_l",
          "forearm_r", "thigh_l", "thigh_r", "shin_l", "shin_r"]

# Global defaults, identical to the AddJoint table in RagdollCarcass.cpp
# (radians there; degrees here, rounded to 0.1).
def _ball(cone, twist, fric, axis=(0, 0, 0)):
    return {"type": "ball", "cone_deg": round(math.degrees(cone), 1),
            "twist_deg": [round(-math.degrees(twist), 1), round(math.degrees(twist), 1)],
            "friction_nm": fric, "cone_axis": list(axis), "anchor_offset_uu": [0, 0, 0]}

def _hinge(lo, hi, fric, rest=0.0):
    return {"type": "hinge", "range_deg": [round(math.degrees(lo), 1), round(math.degrees(hi), 1)],
            "rest_deg": round(math.degrees(rest), 1),
            "friction_nm": fric, "anchor_offset_uu": [0, 0, 0]}

DEFAULTS = {
    "spine":      _ball(0.45, 0.25, 0.8),
    "neck":       _ball(0.55, 0.35, 0.5),
    "head":       _ball(0.50, 0.35, 0.3),
    # cone_axis: direction the cone is centred on, bind space (left = -X,
    # front = +Y, up = +Z); [0,0,0] = the hanging limb. Box3D clamps every
    # cone to 90 deg, hence the off-centre shoulders/hips (see AddJoint).
    "upperarm_l": _ball(1.57, 0.60, 0.3, (-0.80, 0.55, -0.15)),
    "upperarm_r": _ball(1.57, 0.60, 0.3, (0.80, 0.55, -0.15)),
    # rest_deg: the joint spring's rest angle (slightly flexed so limbs do
    # not settle on the hyperextension stop and read as stiff straight limbs).
    "forearm_l":  _hinge(-0.05, 2.20, 0.15, 0.35),
    "forearm_r":  _hinge(-0.05, 2.20, 0.15, 0.35),
    "thigh_l":    _ball(0.87, 0.35, 0.5, (-0.20, 0.45, -0.87)),
    "thigh_r":    _ball(0.87, 0.35, 0.5, (0.20, 0.45, -0.87)),
    "shin_l":     _hinge(-1.75, 0.05, 0.25, -0.26),
    "shin_r":     _hinge(-1.75, 0.05, 0.25, -0.26),
}


def dataset_names(header_path):
    """Dataset names from RagdollDatasets.h ({ TEXT("Commando"), TEXT("SelectionMale1"), ...})."""
    names = []
    with open(header_path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = re.match(r'\s*\{\s*TEXT\("[^"]+"\),\s*TEXT\("([^"]+)"\)', line)
            if m:
                names.append(m.group(1))
    return names


def load_tuning(path):
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    joints = data.get("joints", {})
    unknown = sorted(set(joints) - set(JOINTS))
    if unknown:
        sys.exit(f"{path}: unknown joint names {unknown}; valid: {JOINTS}")
    return data


def spec_line(name, j):
    """One FRagdollJointSpec initializer (radians) plus the anchor offset."""
    d = DEFAULTS[name]
    typ = j.get("type", d["type"])
    fric = float(j.get("friction_nm", d["friction_nm"]))
    off = j.get("anchor_offset_uu", d["anchor_offset_uu"])
    if len(off) != 3:
        sys.exit(f"{name}: anchor_offset_uu must have 3 numbers")
    if typ == "ball":
        cone = math.radians(float(j.get("cone_deg", d.get("cone_deg", 0.0))))
        if cone > math.pi / 2 + 1e-6:
            print(f"warning: {name}: cone_deg {math.degrees(cone):.1f} > 90, Box3D clamps it to 90", file=sys.stderr)
        tw = j.get("twist_deg", d.get("twist_deg", [0.0, 0.0]))
        ax = j.get("cone_axis", d.get("cone_axis", [0, 0, 0]))
        if len(ax) != 3:
            sys.exit(f"{name}: cone_axis must have 3 numbers")
        spec = (f"{{ JK_Ball,  {cone:.4f}f, {math.radians(float(tw[0])):.4f}f, {math.radians(float(tw[1])):.4f}f, {fric:.3f}f, "
                f"{float(ax[0]):.3f}f, {float(ax[1]):.3f}f, {float(ax[2]):.3f}f }}")
    elif typ == "hinge":
        rng = j.get("range_deg", d.get("range_deg", [0.0, 0.0]))
        rest = float(j.get("rest_deg", d.get("rest_deg", 0.0)))
        if not (float(rng[0]) <= rest <= float(rng[1])):
            print(f"warning: {name}: rest_deg {rest:.1f} outside range_deg {rng}; AddJoint clamps it", file=sys.stderr)
        spec = f"{{ JK_Hinge, {math.radians(float(rng[0])):.4f}f, {math.radians(float(rng[1])):.4f}f, {math.radians(rest):.4f}f, {fric:.3f}f, 0.0f, 0.0f, 0.0f }}"
    else:
        sys.exit(f"{name}: type must be 'ball' or 'hinge'")
    return spec, f"FVector({float(off[0]):.3f}f, {float(off[1]):.3f}f, {float(off[2]):.3f}f)"


def dump_tuning(data):
    """JSON with one line per joint (json.dump(indent=2) spreads every array over four lines)."""
    out = ["{", f'  "dataset": {json.dumps(data["dataset"])},']
    if "note" in data:
        out.append(f'  "note": {json.dumps(data["note"])},')
    out.append('  "joints": {')
    items = list(data["joints"].items())
    for i, (name, j) in enumerate(items):
        comma = "," if i < len(items) - 1 else ""
        out.append(f'    {json.dumps(name) + ":":14} {json.dumps(j)}{comma}')
    out.extend(["  }", "}"])
    return "\n".join(out) + "\n"


def emit_header(tunings, out_path):
    lines = [
        "// Generated by tools/build_joint_tuning.py from tools/rigs/<Sel>.tuning.json.",
        "// DO NOT EDIT: edit the tuning files and rerun the script, then rebuild the DLL.",
        "// Per-model joint tables (child-body order: spine, neck, head, upperarm L/R,",
        "// forearm L/R, thigh L/R, shin L/R). Ball: cone, twist lo/hi; hinge: lo/hi/rest;",
        "// all radians; friction Nm. Anchor offsets in canonical UU (DrawScale=1).",
        "",
        "static const FRagdollJointTuning GRagdollJointTuning[] =",
        "{",
    ]
    for name in sorted(tunings):
        joints = tunings[name].get("joints", {})
        specs, offs = [], []
        for jn in JOINTS:
            s, o = spec_line(jn, joints.get(jn, {}))
            specs.append(f"\t\t\t{s},  // {jn}")
            offs.append(f"\t\t\t{o},")
        lines.append(f'\t{{ TEXT("{name}"),')
        lines.append("\t\t{")
        lines.extend(specs)
        lines.append("\t\t},")
        lines.append("\t\t{")
        lines.extend(offs)
        lines.append("\t\t}")
        lines.append("\t},")
    lines.append("};")
    lines.append(f"static const INT GRagdollJointTuningCount = {len(tunings)};")
    lines.append("")
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rigs-dir", default=os.path.join(ROOT, "tools", "rigs"))
    ap.add_argument("--datasets", default=os.path.join(ROOT, "RagdollPkg", "Src", "RagdollDatasets.h"))
    ap.add_argument("--header", default=os.path.join(ROOT, "RagdollPkg", "Src", "RagdollJointTuning.h"))
    ap.add_argument("--init", action="store_true", help="write a <Sel>.tuning.json with the global defaults for every dataset that lacks one")
    args = ap.parse_args()

    names = dataset_names(args.datasets)
    if not names:
        sys.exit(f"no dataset entries found in {args.datasets}")

    if args.init:
        for name in names:
            path = os.path.join(args.rigs_dir, f"{name}.tuning.json")
            if os.path.exists(path):
                print(f"keep   {path}")
                continue
            data = {
                "dataset": name,
                "note": "Per-model joint tuning (degrees / Nm / UU). Starts as a copy of the global defaults; "
                        "edit, then run tools/build_joint_tuning.py and rebuild the DLL.",
                "joints": {jn: DEFAULTS[jn] for jn in JOINTS},
            }
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(dump_tuning(data))
            print(f"wrote  {path}")

    tunings = {}
    for name in names:
        path = os.path.join(args.rigs_dir, f"{name}.tuning.json")
        if os.path.exists(path):
            data = load_tuning(path)
            if data.get("dataset", name) != name:
                sys.exit(f"{path}: 'dataset' is {data.get('dataset')!r}, expected {name!r}")
            tunings[name] = data
    emit_header(tunings, args.header)
    print(f"header {args.header}: {len(tunings)} model(s) [{', '.join(sorted(tunings))}]; "
          f"{len(names) - len(tunings)} on global defaults")


if __name__ == "__main__":
    main()
