
import bpy, sys
from mathutils import Vector
argv = sys.argv[sys.argv.index("--") + 1:]
obj_path, out_front, out_3q, out_back = argv
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.obj_import(filepath=obj_path)
scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.color_type = "MATERIAL"
scene.render.resolution_x = 900
scene.render.resolution_y = 900
scene.render.image_settings.file_format = "PNG"
pts = [o.matrix_world @ v.co for o in scene.objects if o.type == "MESH" for v in o.data.vertices]
mn = Vector((min(v.x for v in pts), min(v.y for v in pts), min(v.z for v in pts)))
mx = Vector((max(v.x for v in pts), max(v.y for v in pts), max(v.z for v in pts)))
ctr = (mn + mx) / 2
rad = max((mx - mn).length * 1.6, 60.0)
def render_shot(out_path, cam_pos):
    cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("C"))
    scene.collection.objects.link(cam)
    pos = Vector(cam_pos)
    cam.location = pos
    cam.rotation_euler = (ctr - pos).to_track_quat("-Z", "Y").to_euler()
    scene.camera = cam
    scene.render.filepath = out_path
    bpy.ops.render.render(write_still=True)
    scene.collection.objects.unlink(cam)
render_shot(out_front, (0.0, rad, rad * 0.5))
render_shot(out_3q, (rad * 0.75, rad * 0.75, rad * 0.6))
render_shot(out_back, (0.0, -rad, rad * 0.5))
