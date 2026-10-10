# Headless Blender render of one or more group-tagged OBJs side by side.
# argv: out.png spacing view group1 [group2 ...]   (group = a.obj[+b.obj...] at one offset)
#   view: front | 3q | side | back
# Each OBJ's objects are colored by their name ("armL", "armR", "rest",
# "Bone_<i>_..." or anything else) so segmentation / skinning is visible.
import bpy, sys
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
out_path, spacing, view = argv[0], float(argv[1]), argv[2]
objs = argv[3:]
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene

PAL = {"armL": (0.95, 0.3, 0.3), "armR": (0.3, 0.4, 0.95), "rest": (0.75, 0.75, 0.75),
       "legL": (0.95, 0.65, 0.2), "legR": (0.2, 0.9, 0.6), "head": (0.95, 0.95, 0.3),
       "torso": (0.7, 0.7, 0.7), "pelvis": (0.6, 0.4, 0.9)}
BONES = [(0.95, 0.3, 0.3), (0.3, 0.95, 0.3), (0.3, 0.3, 0.95), (0.95, 0.95, 0.2),
         (1, 0.55, 0), (1, 0.15, 0.5), (0.55, 1, 0), (0, 1, 1), (0.65, 0.35, 1),
         (0.95, 0.65, 0.2), (0.2, 0.95, 0.65), (0.9, 0.2, 0.85), (0.55, 0.55, 0.55),
         (1, 1, 1)]


def mat(name, col, alpha=1.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = False
    m.diffuse_color = (col[0], col[1], col[2], alpha)
    return m


def color_for(name):
    base = name.split(".")[0]
    if base.startswith("Bone_"):
        return BONES[int(base.split("_")[1]) % len(BONES)]
    for k, c in PAL.items():
        if base == k or base.startswith(k):
            return c
    return (0.8, 0.8, 0.8)


for n, group in enumerate(objs):
    new = []
    for path in group.split("+"):
        before = set(bpy.data.objects)
        bpy.ops.wm.obj_import(filepath=path, forward_axis="Y", up_axis="Z")
        new += [o for o in bpy.data.objects if o not in before]
    for o in new:
        o.location.x += n * spacing
        if o.type != "MESH":
            continue
        if "Skel_" in o.name:
            o.data.materials.append(mat(o.name, (0.0, 0.0, 0.0), 0.2))
            o.show_wire = True
            o.show_all_edges = True
        else:
            o.data.materials.append(mat(o.name, color_for(o.name)))

# Meshes that carry per-vertex colours (weight-blended OBJs) render with them.
if any(o.type == "MESH" and len(o.data.color_attributes) for o in scene.objects):
    bpy.context.scene.display.shading.color_type = "VERTEX"
bpy.context.view_layer.update()
pts = [o.matrix_world @ v.co for o in scene.objects if o.type == "MESH" for v in o.data.vertices]
mn = Vector((min(v.x for v in pts), min(v.y for v in pts), min(v.z for v in pts)))
mx = Vector((max(v.x for v in pts), max(v.y for v in pts), max(v.z for v in pts)))
ctr = (mn + mx) / 2
ext = mx - mn
scene.render.engine = "BLENDER_WORKBENCH"
if scene.display.shading.color_type != "VERTEX":
    scene.display.shading.color_type = "MATERIAL"
scene.display.shading.light = "STUDIO"
scene.display.shading.show_backface_culling = False
W = ext.x if view in ("front", "back") else ext.y if view == "side" else ext.length
aspect = max(0.5, min(4.0, (W + 10) / (ext.z + 10)))
scene.render.resolution_y = 700
scene.render.resolution_x = int(700 * aspect)
scene.render.image_settings.file_format = "PNG"

cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("C"))
cam.data.type = "ORTHO"
cam.data.clip_end = 100000.0
scene.collection.objects.link(cam)
dirs = {"front": Vector((0, -1, 0)), "back": Vector((0, 1, 0)),
        "side": Vector((1, 0, 0)), "3q": Vector((0.7, -0.7, 0.3)).normalized()}
d = dirs.get(view, dirs["front"])
cam.location = ctr - d * (ext.length * 2 + 50)
cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
cam.data.ortho_scale = max(W + 10, (ext.z + 10) * aspect)
scene.camera = cam
scene.render.filepath = out_path
bpy.ops.render.render(write_still=True)
print("wrote", out_path)
