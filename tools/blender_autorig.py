# Headless Blender: build a mesh + armature from JSON, bone-heat skin it, dump
# per-vertex weights. Runs under Blender's python (3.6 and 5.x):
#
#   blender -b --python-exit-code 1 -P tools/blender_autorig.py -- in.json out.json [save.blend]
#
# in.json : { "name": str, "verts": [[x,y,z],...], "faces": [[a,b,c],...],
#             "bones": [{"name","head","tail","parent"}, ...] }
# out.json: { "weights": [ {bone: w, ...}, ... ],   # per vertex
#             "unweighted": [vert ids with no weight],
#             "method": "heat" | "envelope" }
import bpy, sys, json

argv = sys.argv[sys.argv.index("--") + 1:]
in_path, out_path = argv[0], argv[1]
save_path = argv[2] if len(argv) > 2 else None

with open(in_path) as f:
    data = json.load(f)

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene

# --- mesh ---------------------------------------------------------------
me = bpy.data.meshes.new(data["name"] + "_mesh")
me.from_pydata([tuple(v) for v in data["verts"]], [], [tuple(f) for f in data["faces"]])
me.validate(verbose=False)
me.update()
ob = bpy.data.objects.new(data["name"], me)
scene.collection.objects.link(ob)
if len(me.vertices) != len(data["verts"]):
    raise RuntimeError(f"vertex count changed during mesh build: {len(me.vertices)} != {len(data['verts'])}")

# --- armature -----------------------------------------------------------
arm_data = bpy.data.armatures.new(data["name"] + "_arm")
arm = bpy.data.objects.new(data["name"] + "_rig", arm_data)
scene.collection.objects.link(arm)
bpy.context.view_layer.objects.active = arm
arm.select_set(True)
bpy.ops.object.mode_set(mode="EDIT")
eb = arm_data.edit_bones
made = {}
for b in data["bones"]:
    e = eb.new(b["name"])
    e.head = tuple(b["head"])
    e.tail = tuple(b["tail"])
    e.use_connect = False
    made[b["name"]] = e
for b in data["bones"]:
    if b.get("parent"):
        made[b["name"]].parent = made[b["parent"]]
bpy.ops.object.mode_set(mode="OBJECT")

# --- automatic (bone heat) weights --------------------------------------
for o in bpy.data.objects:
    o.select_set(False)
ob.select_set(True)
arm.select_set(True)
bpy.context.view_layer.objects.active = arm
method = "heat"
try:
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")
except Exception as ex:  # noqa: BLE001
    print("ARMATURE_AUTO failed:", ex)
    method = "envelope"
    bpy.ops.object.parent_set(type="ARMATURE_ENVELOPE")

names = {g.index: g.name for g in ob.vertex_groups}
weights = []
unweighted = []
for v in me.vertices:
    w = {names[g.group]: g.weight for g in v.groups if g.weight > 1e-4}
    if not w:
        unweighted.append(v.index)
    weights.append(w)

if save_path:
    bpy.ops.wm.save_as_mainfile(filepath=save_path)

with open(out_path, "w") as f:
    json.dump({"weights": weights, "unweighted": unweighted, "method": method}, f)
print(f"autorig: {len(weights)} verts, {len(unweighted)} unweighted, method={method} -> {out_path}")
