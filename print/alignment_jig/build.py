# Alignment jig for two tempo LED channels: one piece, tapered ends, printed on the bed diagonal.
#   blender -b -P print/alignment_jig/build.py
# Writes alignment_jig.stl (already turned 45 degrees for a 180 mm bed) and alignment_jig.png.
# Units are millimetres. X runs across the two strips, Y along them, Z up.
import bpy, bmesh, math, os

OUT = os.path.dirname(os.path.abspath(__file__))

STRIP_W  = 17.0         # LED channel width
STRIP_H  = 7.0          # LED channel height
CLEAR    = 0.4          # total side clearance in the cradle
CH_W     = STRIP_W + CLEAR
CH_H     = 5.0          # cradle wall height (the strip stands 2 mm proud)
WALL     = 2.4
FLOOR    = 2.0
LEAD     = 0.8          # lead-in chamfer on the channel-side top edge of each wall
SPACING  = 203.2        # 8 in, strip centre to strip centre
BED      = 180.0        # Bambu A1 mini
HY_IN    = 30.5         # half length of the inner (reference) walls; bed diagonal sets the limit
HY_OUT   = 11.0         # half length of the outer walls
XC       = SPACING / 2
X_IN     = XC - CH_W / 2 - WALL
X_OUT    = XC + CH_W / 2 + WALL
TOP      = FLOOR + CH_H


def mesh_obj(name, verts, faces):
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    bm = bmesh.new(); bm.from_mesh(me)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(me); bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(ob)
    return ob


def prism_z(name, poly, z0, z1):
    n = len(poly)
    verts = [(x, y, z0) for x, y in poly] + [(x, y, z1) for x, y in poly]
    faces = [tuple(range(n)), tuple(range(n, 2 * n))] + [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
    return mesh_obj(name, verts, faces)


def prism_y(name, profile, y0, y1):
    """Extrude an (x, z) profile along Y."""
    n = len(profile)
    verts = [(x, y0, z) for x, z in profile] + [(x, y1, z) for x, z in profile]
    faces = [tuple(range(n)), tuple(range(n, 2 * n))] + [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
    return mesh_obj(name, verts, faces)


def union(parts):
    base = parts[0]
    for p in parts[1:]:
        m = base.modifiers.new("b", "BOOLEAN")
        m.operation, m.object, m.solver = "UNION", p, "EXACT"
        bpy.context.view_layer.objects.active = base
        bpy.ops.object.modifier_apply(modifier=m.name)
        bpy.data.objects.remove(p, do_unlink=True)
    return base


def wall(x_ch, x_back, hy):
    """One cradle wall. x_ch is the face the strip touches, x_back the opposite face."""
    d = LEAD if x_back > x_ch else -LEAD
    z0 = FLOOR - 0.01
    return prism_y("wall", [(x_ch, z0), (x_back, z0), (x_back, TOP), (x_ch + d, TOP), (x_ch, TOP - LEAD)], -hy, hy)


def build():
    xt = X_IN + WALL        # the taper starts at the channel face, so the full inner wall sits on the plate
    outline = [(-X_OUT, -HY_OUT), (-xt, -HY_IN), (xt, -HY_IN), (X_OUT, -HY_OUT),
               (X_OUT, HY_OUT), (xt, HY_IN), (-xt, HY_IN), (-X_OUT, HY_OUT)]
    parts = [prism_z("jig", outline, 0, FLOOR)]
    for s in (1, -1):
        parts.append(wall(s * (X_IN + WALL), s * X_IN, HY_IN))        # inner, the reference wall
        parts.append(wall(s * (X_OUT - WALL), s * X_OUT, HY_OUT))     # outer
    return union(parts)


def check(ob):
    bm = bmesh.new(); bm.from_mesh(ob.data)
    open_edges = sum(1 for e in bm.edges if not e.is_manifold)
    vol = abs(bm.calc_volume()); bm.free()
    xs = [(ob.matrix_world @ v.co).x for v in ob.data.vertices]
    ys = [(ob.matrix_world @ v.co).y for v in ob.data.vertices]
    w, h = max(xs) - min(xs), max(ys) - min(ys)
    print(f"JIG bed footprint {w:.1f} x {h:.1f} mm on a {BED:.0f} bed, margin {(BED - max(w, h)) / 2:.1f} mm, "
          f"volume {vol / 1000:.1f} cm3, non-manifold edges {open_edges}")
    assert max(w, h) <= BED - 4 and open_edges == 0      # keep 2 mm clear of every bed edge


def mat(rgb, rough=0.5, metal=0.0):
    m = bpy.data.materials.new("m"); m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*rgb, 1)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metal
    return m


def paint(ob, m):
    ob.data.materials.clear(); ob.data.materials.append(m)
    for f in ob.data.polygons: f.material_index = 0


def render(jig):
    sc = bpy.context.scene
    sc.render.engine, sc.cycles.device, sc.cycles.samples = "CYCLES", "CPU", 64
    sc.render.resolution_x, sc.render.resolution_y = 1400, 900
    w = bpy.data.worlds.new("w"); sc.world = w; w.use_nodes = True
    w.node_tree.nodes["Background"].inputs[0].default_value = (0.9, 0.92, 0.95, 1)
    w.node_tree.nodes["Background"].inputs[1].default_value = 0.35
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 2.0; sun.rotation_euler = (math.radians(50), 0, math.radians(35))
    sc.collection.objects.link(sun)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.lens, cam.data.clip_end, cam.location = 50, 5000, (200, -300, 230)
    sc.collection.objects.link(cam); sc.camera = cam
    tgt = bpy.data.objects.new("tgt", None); sc.collection.objects.link(tgt)
    cam.constraints.new("TRACK_TO").target = tgt
    paint(jig, mat((0.95, 0.38, 0.05), 0.55))
    paint(prism_z("ground", [(-900, -900), (900, -900), (900, 900), (-900, 900)], -1, 0), mat((0.07, 0.25, 0.10), 0.95))
    alu = mat((0.75, 0.76, 0.78), 0.35, 0.9)
    # one strip in place, the other channel left empty so the cradle shows
    paint(prism_z("strip", [(-XC - STRIP_W / 2, -420), (-XC + STRIP_W / 2, -420), (-XC + STRIP_W / 2, 420), (-XC - STRIP_W / 2, 420)],
                  FLOOR, FLOOR + STRIP_H), alu)
    sc.render.filepath = os.path.join(OUT, "alignment_jig.png")
    bpy.ops.render.render(write_still=True)


def main():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    jig = build()
    jig.rotation_euler = (0, 0, math.radians(45))       # print orientation: corner to corner
    bpy.context.view_layer.update()
    check(jig)
    bpy.ops.object.select_all(action="DESELECT"); jig.select_set(True)
    bpy.ops.wm.stl_export(filepath=os.path.join(OUT, "alignment_jig.stl"), export_selected_objects=True)
    jig.rotation_euler = (0, 0, 0)
    render(jig)


main()
