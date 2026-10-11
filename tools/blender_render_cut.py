# Render a gore_cuts.py OBJ (islands + cap_* + bone_* objects) in Blender.
# Usable from the Blender MCP (exec(open(path).read()) then render_cut(...))
# or headless:  "<blender>" -b -P tools/blender_render_cut.py -- in.obj out.png [vx vy vz] [focus_prefix]
# Skin is grey, caps dark red, bone stubs ivory; the ortho camera frames the
# objects whose names start with the focus prefixes (default: the caps).
import bpy, os, sys
from mathutils import Vector


def clear_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for m in list(bpy.data.meshes):
        if m.users == 0:
            bpy.data.meshes.remove(m)


def render_cut(path, out, view=(0.7, -0.7, 0.35), focus=("cap_", "bone_"), pad=14.0, res=900, wire=False):
    clear_scene()
    scene = bpy.context.scene
    before = set(bpy.data.objects)
    bpy.ops.wm.obj_import(filepath=path, forward_axis='Y', up_axis='Z')
    new = [o for o in bpy.data.objects if o not in before]

    def mat(name, col):
        m = bpy.data.materials.new(name)
        m.use_nodes = False
        m.diffuse_color = (col[0], col[1], col[2], 1)
        return m
    pts = []
    for o in new:
        if o.type != 'MESH':
            continue
        n = o.name.split('.')[0]
        if n.startswith('cap_'):
            c = (0.40, 0.04, 0.04)
        elif n.startswith('bone_'):
            c = (0.92, 0.88, 0.72)
        else:
            c = (0.72, 0.70, 0.66)
        o.data.materials.append(mat(n, c))
        o.show_wire = wire
        o.show_all_edges = wire
        if any(n.startswith(f) for f in focus):
            pts += [o.matrix_world @ v.co for v in o.data.vertices]
    bpy.context.view_layer.update()
    if not pts:
        pts = [o.matrix_world @ v.co for o in new if o.type == 'MESH' for v in o.data.vertices]
    mn = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    mx = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    ctr = (mn + mx) / 2
    ext = (mx - mn).length + pad
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.color_type = 'MATERIAL'
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.show_backface_culling = False
    scene.display.shading.show_cavity = True
    scene.display.shading.show_object_outline = wire
    scene.render.resolution_x = res
    scene.render.resolution_y = res
    scene.render.image_settings.file_format = 'PNG'
    cam = bpy.data.objects.new('Cam', bpy.data.cameras.new('C'))
    cam.data.type = 'ORTHO'
    cam.data.clip_end = 100000
    scene.collection.objects.link(cam)
    d = Vector(view).normalized()
    cam.location = ctr - d * 200
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    cam.data.ortho_scale = ext
    scene.camera = cam
    scene.render.filepath = out
    bpy.ops.render.render(write_still=True)
    return out


if __name__ == "__main__" and "--" in sys.argv:
    argv = sys.argv[sys.argv.index("--") + 1:]
    view = tuple(float(x) for x in argv[2:5]) if len(argv) >= 5 else (0.7, -0.7, 0.35)
    focus = (argv[5],) if len(argv) >= 6 else ("cap_", "bone_")
    print("wrote", render_cut(argv[0], argv[1], view, focus))
