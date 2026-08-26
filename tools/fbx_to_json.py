import bpy, sys, json

"""Dump a Mixamo T-pose FBX to JSON for tools/build_rig_mixamo.py.

Usage:
    blender.exe -b --python-exit-code 1 -P tools/fbx_to_json.py -- <in.fbx> <out.json>

Output JSON:
    { "verts": [[x,y,z], ...],
      "faces": [{ "verts": [v0,v1,v2], "uvs": [[u,v],...] }, ...],
      "bones": [{ "name", "head": [..], "tail": [..], "parent" }, ...],
      "weights": [ { "mixamorig:Hips": 0.7, ... }, ... ] }   # per vertex
"""

fbx_path = sys.argv[sys.argv.index("--") + 1]
out_path = sys.argv[sys.argv.index("--") + 2]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=fbx_path)

mesh = next(o for o in bpy.data.objects if o.type == "MESH")
arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")

verts = [tuple(v.co) for v in mesh.data.vertices]

uv_layer = mesh.data.uv_layers.active.data if mesh.data.uv_layers else None
faces = []
for p in mesh.data.polygons:
    corners = [mesh.data.loops[i].vertex_index for i in p.loop_indices]
    uvs = []
    if uv_layer is not None:
        for i in p.loop_indices:
            uvs.append((uv_layer[i].uv.x, uv_layer[i].uv.y))
    faces.append({"verts": corners, "uvs": uvs})


def vweights(obj, vidx):
    out = {}
    for g in obj.vertex_groups:
        try:
            w = g.weight(vidx)
        except RuntimeError:
            continue
        if w > 0.0:
            out[g.name] = w
    return out


wts = [vweights(mesh, i) for i in range(len(mesh.data.vertices))]

bones = []
for b in arm.data.bones:
    bones.append({
        "name": b.name,
        "head": tuple(b.head_local),
        "tail": tuple(b.tail_local),
        "parent": b.parent.name if b.parent else None,
    })

out = {
    "verts": [list(v) for v in verts],
    "faces": faces,
    "bones": bones,
    "weights": wts,
}
with open(out_path, "w") as f:
    json.dump(out, f)
print(f"dumped {len(verts)} verts, {len(faces)} faces, {len(bones)} bones, uv={'yes' if uv_layer else 'no'} -> {out_path}")
