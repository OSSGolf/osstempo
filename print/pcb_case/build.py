# Snap-together case for the OSSTempo v1.2 carrier board with its ESP32 dev board plugged in.
#   blender -b -P print/pcb_case/build.py
# Writes case_base.stl and case_lid.stl (the lid already turned roof-down for printing) and renders.
# Units are millimetres. Origin is the board centre; +Y is the USB end, -Y the strip connector, Z up.
import bpy, bmesh, math, os

OUT = os.path.dirname(os.path.abspath(__file__))
WORDMARK_SVG = os.path.join(OUT, "art", "ossgolf_wordmark_paths.svg")
SCRIPT_FONT = "/System/Library/Fonts/Supplemental/SignPainter.ttc"
NUMBER_FONT = "/System/Library/Fonts/Supplemental/Times New Roman Italic.ttf"

# ---- the board (from pcb/routed/osstempo_led_carrier/osstempo_led_carrier.kicad_pcb)
BW, BL, BR = 50.0, 70.0, 3.0        # outline and corner radius
PCB_T      = 1.6
HOLES      = [(sx * 20.5, sy * 30.5) for sx in (1, -1) for sy in (1, -1)]   # 3.2 mm, 4.5 mm in from each edge
JST_X      = (-7.45, 2.45)          # strip connector body, opening on the -Y edge
JST_H      = 6.1                    # its height above the board
SOCKET_H   = 8.5                    # pin sockets H1/H2
# ---- the ESP32 dev board riding on the sockets (30-pin DevKit; not in the KiCad file, so these are nominal)
HEADER_H   = 2.54                   # plastic spacer on its pin headers
DEV_T      = 1.6
DEV_PARTS  = 3.2                    # tallest part on top: the shielded radio module
USB_MID    = 1.3                    # USB receptacle centre above the dev board's top face

# ---- the case
GAP     = 0.5                       # board edge to the base's inner wall
RIM_T   = 1.2                       # base inner wall (the rim the lid slides over)
FIT     = 0.25                      # rim to lid wall, per side
LID_T   = 1.6                       # lid wall
FLOOR   = 1.6
ROOF    = 1.8
STANDOFF = 2.6                      # under the board, for the solder tails
HEADROOM = 0.8                      # above the dev board's tallest part
RIM_TOP = 7.5
PEG_D, POST_D = 2.9, 6.4
COL_D, BORE_D = 7.0, 3.6            # lid hold-down columns; the bore swallows the peg tip
HOLD_GAP = 0.1                      # column foot to board, so the latches always close
BUMP    = 0.9                       # latch bump height
BUMP_HY, TAB_HY = 5.0, 9.0          # half widths: bump, flexing tab
SLIT_TOP = 20.0                     # the tab flexes from here down (lower on the open-frame lid)
DEBOSS  = 0.6

Z_BOT   = FLOOR + STANDOFF                      # board underside
Z_TOP   = Z_BOT + PCB_T                         # board top face
Z_DEV   = Z_TOP + SOCKET_H + HEADER_H + DEV_T   # dev board top face
VARIANT = os.environ.get("CASE_VARIANT", "a")   # a: closed, ball art; b: closed, dimpled skin; c: open frame
if VARIANT == "c":                              # the roof is a frame level with the radio module, ESP32 showing through
    Z_LID  = Z_DEV + DEV_PARTS + 0.2
    Z_CEIL = Z_LID - ROOF
else:
    Z_CEIL = Z_DEV + DEV_PARTS + HEADROOM       # lid underside
    Z_LID  = Z_CEIL + ROOF                      # overall height
SLIT_TOP = min(SLIT_TOP, Z_CEIL - 2.5)
X_RIM, Y_RIM = BW / 2 + GAP + RIM_T, BL / 2 + GAP + RIM_T     # rim outer faces
X_IN,  Y_IN  = X_RIM + FIT, Y_RIM + FIT                       # lid inner faces
X_OUT, Y_OUT = X_IN + LID_T, Y_IN + LID_T                     # outside
R_OUT = BR + GAP + RIM_T + FIT + LID_T


# ---------------------------------------------------------------- mesh helpers
def mesh_obj(name, verts, faces):
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    bm = bmesh.new(); bm.from_mesh(me)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(me); bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(ob)
    return ob


def loft(name, rings):
    """Solid through a stack of (outline, z) rings that all have the same vertex count."""
    n = len(rings[0][0])
    verts = [(x, y, z) for poly, z in rings for x, y in poly]
    faces = [tuple(range(n)), tuple(range((len(rings) - 1) * n, len(rings) * n))]
    for k in range(len(rings) - 1):
        a, b = k * n, (k + 1) * n
        faces += [(a + i, a + (i + 1) % n, b + (i + 1) % n, b + i) for i in range(n)]
    return mesh_obj(name, verts, faces)


def prism(name, poly, z0, z1):
    return loft(name, [(poly, z0), (poly, z1)])


def prism_y(name, profile, y0, y1):
    """Extrude an (x, z) profile along Y."""
    n = len(profile)
    verts = [(x, y0, z) for x, z in profile] + [(x, y1, z) for x, z in profile]
    faces = [tuple(range(n)), tuple(range(n, 2 * n))] + [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
    return mesh_obj(name, verts, faces)


def rrect(w, h, r, seg=10):
    pts = []
    for cx, cy, a0 in ((w / 2 - r, h / 2 - r, 0), (-w / 2 + r, h / 2 - r, 90), (-w / 2 + r, -h / 2 + r, 180), (w / 2 - r, -h / 2 + r, 270)):
        for i in range(seg + 1):
            a = math.radians(a0 + 90 * i / seg)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def circle(cx, cy, r, n=48):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def box(name, x0, x1, y0, y1, z0, z1):
    return prism(name, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], z0, z1)


def boolean(base, parts, op):
    for p in parts:
        m = base.modifiers.new("b", "BOOLEAN")
        m.operation, m.object, m.solver = op, p, "EXACT"
        m.use_self = True
        bpy.context.view_layer.objects.active = base
        bpy.ops.object.modifier_apply(modifier=m.name)
        bpy.data.objects.remove(p, do_unlink=True)
    return base


def join(parts, name):
    bpy.ops.object.select_all(action="DESELECT")
    for p in parts: p.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    parts[0].name = name
    return parts[0]


def curve_to_solid(ob, depth):
    """Flat filled curve or text object -> closed mesh, `depth` thick, centred on its own Z."""
    ob.data.dimensions, ob.data.fill_mode, ob.data.extrude = "2D", "BOTH", depth / 2
    bpy.ops.object.select_all(action="DESELECT"); ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.convert(target="MESH")
    bm = bmesh.new(); bm.from_mesh(ob.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-4)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(ob.data); bm.free()
    return ob


def fit(ob, width, cx, cy, z):
    """Scale a mesh to `width`, centre its bounding box on (cx, cy), put its mid-plane at z."""
    xs = [v.co.x for v in ob.data.vertices]; ys = [v.co.y for v in ob.data.vertices]
    s = width / (max(xs) - min(xs))
    mx, my = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
    for v in ob.data.vertices:
        v.co.x, v.co.y, v.co.z = (v.co.x - mx) * s + cx, (v.co.y - my) * s + cy, v.co.z + z
    return (max(ys) - min(ys)) * s


def text_solid(body, font_path, width, cx, cy, z, depth):
    cu = bpy.data.curves.new("txt", "FONT")
    cu.body, cu.font, cu.resolution_u = body, bpy.data.fonts.load(font_path), 6
    ob = bpy.data.objects.new("txt", cu); bpy.context.collection.objects.link(ob)
    curve_to_solid(ob, depth)
    fit(ob, width, cx, cy, z)
    return ob


def svg_loops(path):
    """Closed outlines of every <path> in an SVG that uses only absolute M L H V Q Z, curves flattened."""
    import re
    loops = []
    for d in re.findall(r'<path[^>]* d="([^"]*)"', open(path).read()):
        tok = re.findall(r"[MLHVQZ]|-?\d+\.?\d*", d)
        i, cmd, cur, pts = 0, None, (0.0, 0.0), []
        def num():
            nonlocal i
            i += 1; return float(tok[i - 1])
        while i < len(tok):
            if tok[i].isalpha():
                cmd = tok[i]; i += 1
                if cmd == "Z":
                    if len(pts) > 2: loops.append(pts)
                    pts = []
                continue
            if cmd == "M": cur = (num(), num()); pts = [cur]; cmd = "L"; continue
            if cmd == "L": cur = (num(), num())
            elif cmd == "H": cur = (num(), cur[1])
            elif cmd == "V": cur = (cur[0], num())
            elif cmd == "Q":
                c, e = (num(), num()), (num(), num())
                for k in range(1, 4):
                    t = k / 4
                    pts.append(((1 - t) ** 2 * cur[0] + 2 * t * (1 - t) * c[0] + t * t * e[0],
                                (1 - t) ** 2 * cur[1] + 2 * t * (1 - t) * c[1] + t * t * e[1]))
                cur = e
            else: raise ValueError(f"unsupported SVG path command {cmd}")
            if pts[-1] != cur: pts.append(cur)
    return [lp[:-1] if lp[0] == lp[-1] else lp for lp in loops]


def inside(p, poly):
    x, y = p; hit = False
    for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0): hit = not hit
    return hit


# ---------------------------------------------------------------- the two parts
def build_base():
    plate = loft("base", [(rrect(2 * X_OUT - 0.8, 2 * Y_OUT - 0.8, R_OUT - 0.4), 0),      # 0.4 chamfer on the bed edge
                          (rrect(2 * X_OUT, 2 * Y_OUT, R_OUT), 0.4),
                          (rrect(2 * X_OUT, 2 * Y_OUT, R_OUT), FLOOR)])
    r_rim = BR + GAP + RIM_T
    rim = loft("rim", [(rrect(2 * X_RIM, 2 * Y_RIM, r_rim), FLOOR - 0.01),
                       (rrect(2 * X_RIM, 2 * Y_RIM, r_rim), RIM_TOP - 0.5),
                       (rrect(2 * X_RIM - 0.8, 2 * Y_RIM - 0.8, r_rim - 0.4), RIM_TOP)])   # lead-in for the lid
    boolean(rim, [prism("c", rrect(BW + 2 * GAP, BL + 2 * GAP, BR + GAP), FLOOR - 0.02, RIM_TOP + 1),
                  box("jst", JST_X[0] - 0.9, JST_X[1] + 0.9, -Y_RIM - 1, -BL / 2, Z_TOP - 0.2, RIM_TOP + 1)], "DIFFERENCE")
    parts = [rim]
    for x, y in HOLES:
        parts.append(prism("post", circle(x, y, POST_D / 2), FLOOR - 0.01, Z_BOT))
        tip = Z_TOP + 2.4
        parts.append(loft("peg", [(circle(x, y, PEG_D / 2), Z_BOT - 0.01), (circle(x, y, PEG_D / 2), tip - 0.6),
                                  (circle(x, y, PEG_D / 2 - 0.5), tip)]))
    for s in (1, -1):       # latch bumps: 45 degree catch face underneath (prints clean, releases on a firm pull), long ramp on top
        x0 = s * X_RIM
        parts.append(prism_y("bump", [(x0 - s * 0.3, 3.6), (x0, 3.6), (x0 + s * BUMP, 3.6 + BUMP), (x0 + s * BUMP, 3.9 + BUMP),
                                      (x0, RIM_TOP - 0.3), (x0 - s * 0.3, RIM_TOP - 0.3)], -BUMP_HY, BUMP_HY))
    return boolean(plate, parts, "UNION")


def wordmark(y_mid, zc, width=46.0):
    """oss/golf: prompt chevron and letters from the logo's own SVG paths, then the flag cursor box."""
    u = width / (4660 - 81)                       # SVG units -> mm across the prompt row (chevron to cursor box)
    x_left = -width / 2
    def svg(x, y): return x_left + (x - 81) * u, y_mid + (560 - y) * u      # row centre is y = 560 in SVG units
    loops = [[svg(x, y) for x, y in lp] for lp in svg_loops(WORDMARK_SVG)]
    holes = [lp for lp in loops if any(lp is not o and inside(lp[0], o) for o in loops)]   # counters of o and g
    cut = [prism("glyph", lp, zc - DEBOSS, zc + 1) for lp in loops if lp not in holes]
    keep = [prism("counter", lp, zc - DEBOSS - 0.01, zc) for lp in holes]
    seen = {}
    for lp in loops:                              # where pixel corners meet at a point, open a hairline so the mesh stays simple
        for p in lp:
            k = (round(p[0], 3), round(p[1], 3)); seen[k] = seen.get(k, 0) + 1
    cut += [prism("pinch", circle(x, y, 0.12, 4), zc - DEBOSS, zc + 1) for (x, y), n in seen.items() if n > 1]
    def rect(x0, y0, x1, y1, z0, z1):
        (ax, ay), (bx, by) = svg(x0, y1), svg(x1, y0)
        return box("r", ax, bx, ay, by, z0, z1)
    cut.append(boolean(rect(4120, 100, 4660, 880, zc - DEBOSS, zc + 1),          # pole and flag stay standing in the box
                       [rect(4240, 220, 4300, 760, zc - 1, zc + 2), rect(4300, 220, 4420, 460, zc - 1, zc + 2),
                        rect(4420, 280, 4540, 400, zc - 1, zc + 2)], "DIFFERENCE"))
    return cut, keep


def dimple(x, y, zc):
    return prism("dot", circle(x, y, 1.05, 20), zc - DEBOSS, zc + 1)


def build_art():
    """Everything pressed into the lid's top face: (cutters, islands to put back)."""
    zc, d = Z_LID, 2 * DEBOSS                     # text solids are centred on the top face: DEBOSS below it, the rest in air
    if VARIANT == "c":                            # open frame: only the strip below the window carries art
        cut, keep = wordmark(-33.2, zc, 40.0)
        return cut + [text_solid("OSSTempo", SCRIPT_FONT, 26.0, 0, -25.6, zc, d)], keep
    if VARIANT == "b":                            # golf-ball skin: dimples all over, the two names in a clear band
        cut, keep = wordmark(-9.5, zc)
        cut.append(text_solid("OSSTempo", SCRIPT_FONT, 44.0, 0, 6.0, zc, d))
        pitch = 3.6
        for j in range(-12, 13):
            for i in range(-9, 10):
                x, y = (i + 0.5 * (j % 2)) * pitch, 0.5 + j * pitch * 0.866
                if abs(x) > X_OUT - 3.2 or abs(y) > Y_OUT - 3.2 or -16 < y < 15: continue
                if abs(x) > X_OUT - 6 and abs(y) > Y_OUT - 6: continue            # the rounded corners
                cut.append(dimple(x, y, zc))
        return cut, keep
    bx, by, br = 0.0, 9.5, 22.5                   # A: the golf ball from the board's silkscreen
    cut, keep = wordmark(-27.0, zc)
    cut.append(boolean(prism("ring", circle(bx, by, br, 96), zc - DEBOSS, zc + 1),
                       [prism("r", circle(bx, by, br - 1.0, 96), zc - 1, zc + 2)], "DIFFERENCE"))
    cut += [text_solid("OSSTempo", SCRIPT_FONT, 31.0, bx, by + 3.0, zc, d), text_solid("1", NUMBER_FONT, 2.6, bx, by - 6.5, zc, d)]
    pitch, lx, ly, lr = 3.5, bx + 6.5, by + 6.5, 20.5      # dimples: a crescent on the lower left
    for j in range(-8, 9):
        for i in range(-8, 9):
            x, y = bx + (i + 0.5 * (j % 2)) * pitch, by + j * pitch * 0.866
            if math.hypot(x - bx, y - by) > br - 2.6 or math.hypot(x - lx, y - ly) < lr: continue
            if abs(x - bx) < 17 and by - 1 < y < by + 8: continue          # keep clear of the script
            if abs(x - bx) < 3 and abs(y - (by - 6.5)) < 4: continue       # and of the 1
            cut.append(dimple(x, y, zc))
    return cut, keep


def build_lid(art=True):
    lid = loft("lid", [(rrect(2 * X_OUT, 2 * Y_OUT, R_OUT), FLOOR),
                       (rrect(2 * X_OUT, 2 * Y_OUT, R_OUT), Z_LID - 1.0),
                       (rrect(2 * X_OUT - 2.0, 2 * Y_OUT - 2.0, R_OUT - 1.0), Z_LID)])     # 1 mm chamfer round the top
    boolean(lid, [prism("cav", rrect(2 * X_IN, 2 * Y_IN, R_OUT - LID_T), FLOOR - 1, Z_CEIL)], "DIFFERENCE")
    add = []
    for x, y in HOLES:      # hold-down columns: press the board onto its posts, around each peg
        add.append(boolean(prism("col", circle(x, y, COL_D / 2), Z_TOP + HOLD_GAP, Z_CEIL + 0.01),
                           [prism("bore", circle(x, y, BORE_D / 2), Z_TOP - 1, Z_TOP + 4.5)], "DIFFERENCE"))
    for s in (1, -1):       # finger lip at the foot of each latch tab
        x0 = s * X_OUT
        add.append(prism_y("lip", [(x0 - s * 0.3, FLOOR), (x0 + s * 1.2, FLOOR), (x0 + s * 1.2, FLOOR + 0.4),
                                   (x0, FLOOR + 1.6), (x0 - s * 0.3, FLOOR + 1.6)], -TAB_HY + 0.5, TAB_HY - 0.5))
    boolean(lid, add, "UNION")
    cut = []
    for s in (1, -1):
        xa, xb = sorted((s * (X_IN - 1), s * (X_OUT + 2)))
        for y in (TAB_HY, -TAB_HY - 1.0):                                        # slits that free the tab
            cut.append(box("slit", xa, xb, y, y + 1.0, FLOOR - 1, SLIT_TOP))
        xa, xb = sorted((s * (X_IN - 1), s * (X_OUT + 0.01)))
        cut.append(box("win", xa, xb, -BUMP_HY - 0.6, BUMP_HY + 0.6, 3.5, 3.5 + 3.3))   # the bump shows through here
    cut.append(box("jst", JST_X[0] - 0.9, JST_X[1] + 0.9, -Y_OUT - 1, -Y_IN + 1, FLOOR - 1, Z_TOP + JST_H + 0.7))
    usb_z = Z_DEV + USB_MID
    cut.append(box("usb", -7.0, 7.0, Y_IN - 1, Y_OUT + 1, usb_z - 3.9, Z_LID + 1 if VARIANT == "c" else Z_LID - 1.0))
    if VARIANT == "c":                            # window over the whole dev board, running out to the USB notch
        cut.append(prism("window", [(x, y + 6.9) for x, y in rrect(29.6, 53.0, 2.0)], Z_CEIL - 1, Z_LID + 1))
    boolean(lid, cut, "DIFFERENCE")
    if art:
        cut, keep = build_art()
        boolean(lid, cut, "DIFFERENCE")
        boolean(lid, keep, "UNION")           # put the letter counters back as islands
    return lid


def check(ob, label):
    bm = bmesh.new(); bm.from_mesh(ob.data)
    bad = sum(1 for e in bm.edges if not e.is_manifold)
    for e in bm.edges:
        if not e.is_manifold: print("CASE   open edge", [tuple(round(c, 2) for c in v.co) for v in e.verts], len(e.link_faces))
    vol = abs(bm.calc_volume()); bm.free()
    co = [ob.matrix_world @ v.co for v in ob.data.vertices]
    dim = [max(c[i] for c in co) - min(c[i] for c in co) for i in range(3)]
    print(f"CASE {label}: {dim[0]:.1f} x {dim[1]:.1f} x {dim[2]:.1f} mm, {vol / 1000:.1f} cm3, non-manifold edges {bad}")
    return bad


def export(ob, name, flip=False):
    if flip:
        ob.rotation_euler = (math.pi, 0, 0); ob.location.z = Z_LID       # roof down on the bed
    bpy.context.view_layer.update()
    bpy.ops.object.select_all(action="DESELECT"); ob.select_set(True)
    bpy.ops.wm.stl_export(filepath=os.path.join(OUT, name), export_selected_objects=True)
    ob.rotation_euler = (0, 0, 0); ob.location.z = 0
    bpy.context.view_layer.update()


# ---------------------------------------------------------------- renders
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
    return ob


def stack():
    """Stand-in for the populated board, so the renders show what the case holds."""
    g, k = mat((0.05, 0.28, 0.12), 0.6), mat((0.04, 0.04, 0.05), 0.5)
    obs = [paint(boolean(prism("pcb", rrect(BW, BL, BR), Z_BOT, Z_TOP),
                         [prism("h", circle(x, y, 1.6), Z_BOT - 1, Z_TOP + 1) for x, y in HOLES], "DIFFERENCE"), g)]
    for x in (13, -13):
        obs.append(paint(box("sock", x - 1.27, x + 1.27, -10.83, 27.27, Z_TOP, Z_TOP + SOCKET_H), k))
    obs.append(paint(box("jst", JST_X[0], JST_X[1], -34.2, -22.7, Z_TOP, Z_TOP + JST_H), mat((0.9, 0.9, 0.86), 0.5)))
    z = Z_DEV - DEV_T
    obs.append(paint(box("dev", -14.1, 14.1, -19.0, 32.5, z, Z_DEV), k))
    obs.append(paint(box("module", -9, 9, -19, 6.5, Z_DEV, Z_DEV + DEV_PARTS), mat((0.7, 0.71, 0.73), 0.3, 0.9)))
    obs.append(paint(box("usb", -3.8, 3.8, 27.5, 33.3, Z_DEV, Z_DEV + 2.6), mat((0.75, 0.76, 0.78), 0.3, 0.9)))
    return obs


def scene_setup():
    sc = bpy.context.scene
    sc.render.engine, sc.cycles.device, sc.cycles.samples = "CYCLES", "CPU", 96
    sc.render.resolution_x, sc.render.resolution_y = 1500, 1100
    w = bpy.data.worlds.new("w"); sc.world = w; w.use_nodes = True
    w.node_tree.nodes["Background"].inputs[0].default_value = (0.9, 0.92, 0.95, 1)
    w.node_tree.nodes["Background"].inputs[1].default_value = 0.4
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy, sun.data.angle = 2.2, math.radians(8)
    sun.rotation_euler = (math.radians(55), 0, math.radians(-40))
    sc.collection.objects.link(sun)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.lens, cam.data.clip_end = 70, 5000
    sc.collection.objects.link(cam); sc.camera = cam
    tgt = bpy.data.objects.new("tgt", None); sc.collection.objects.link(tgt)
    cam.constraints.new("TRACK_TO").target = tgt
    paint(box("ground", -900, 900, -900, 900, -1, 0), mat((0.07, 0.25, 0.10), 0.95))
    return sc, cam, tgt


def shot(sc, cam, tgt, name, loc, look):
    cam.location, tgt.location = loc, look
    sc.render.filepath = name
    bpy.ops.render.render(write_still=True)


def main():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    base, lid = build_base(), build_lid(art=not os.environ.get("CASE_NO_ART"))
    bad = check(base, "base") + check(lid, "lid")
    print(f"CASE closed: {2 * X_OUT:.1f} x {2 * Y_OUT:.1f} x {Z_LID:.1f} mm; board top at {Z_TOP:.1f}, dev board top at {Z_DEV:.1f}, "
          f"ceiling at {Z_CEIL:.1f}, USB window centre {(Z_DEV + USB_MID - 3.9 + Z_LID - 1.0) / 2:.1f}")
    if bad: print(f"CASE WARNING: {bad} non-manifold edges")
    opts = os.environ.get("CASE_OPTIONS_DIR")      # comparison renders only: no STLs
    if not opts:
        export(base, "case_base.stl"); export(lid, "case_lid.stl", flip=True)
    if os.environ.get("CASE_NO_RENDER"): return
    out = opts or OUT
    sc, cam, tgt = scene_setup()
    orange, cream = mat((0.95, 0.38, 0.05), 0.55), mat((0.92, 0.9, 0.84), 0.55)
    paint(base, cream); paint(lid, orange)
    inner = stack()
    shot(sc, cam, tgt, os.path.join(out, f"{VARIANT}_closed.png"), (150, -190, 150), (0, 0, 10))
    shot(sc, cam, tgt, os.path.join(out, f"{VARIANT}_top.png"), (0, -1, 330), (0, 0, 10))
    if not opts:
        shot(sc, cam, tgt, os.path.join(out, f"{VARIANT}_usb_end.png"), (-120, 210, 110), (0, 0, 10))
    lid.rotation_euler = (math.pi, 0, 0); lid.location = (75, 0, Z_LID)      # opened: board in the base, lid beside it
    shot(sc, cam, tgt, os.path.join(out, f"{VARIANT}_open.png"), (190, -230, 200), (35, 0, 8))

main()
