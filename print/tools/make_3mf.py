"""Write a core-spec 3MF that Bambu Studio opens with the parts already
placed on the plate. Used by stand.py; also a CLI for a single STL:
    python3 tools/make_3mf.py part.stl out.3mf --copies 2
"""
import json, struct, sys, zipfile
from xml.sax.saxutils import quoteattr

def read_stl(path):
    """Binary STL -> (vertices, triangles), vertices welded at 1e-5 mm."""
    data = open(path, "rb").read()
    n = struct.unpack("<I", data[80:84])[0]
    verts, index, tris = [], {}, []
    for i in range(n):
        f = struct.unpack("<12f", data[84 + i * 50: 84 + i * 50 + 48])
        tri = []
        for k in range(3):
            v = tuple(round(c, 5) for c in f[3 + 3 * k: 6 + 3 * k])
            if v not in index:
                index[v] = len(verts); verts.append(v)
            tri.append(index[v])
        if len(set(tri)) == 3:
            tris.append(tri)
    return verts, tris

def write_3mf(path, objects, items):
    """objects: [(name, verts, tris)]; items: [(object_index, 3x4 matrix as
    12 floats in 3MF row-vector order m00 m01 m02 m10 .. m22 tx ty tz)].

    Each item becomes its own object with its placement baked into the
    vertices, and every build transform is identity. Slicers (Bambu Studio
    included) turn two items that share one object into instances and may
    drop or merge their transforms, which stacks the parts on top of each
    other. Baked coordinates leave nothing to reinterpret."""
    res, build = [], []
    for k, (o, m) in enumerate(items):
        name, v, t = objects[o]
        w = [(p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9],
              p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10],
              p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11]) for p in v]
        if det3(m) < 0:  # a mirror would turn the mesh inside out
            t = [(a, c, b) for a, b, c in t]
        vx = "".join(f'<vertex x="{p[0]:.5f}" y="{p[1]:.5f}" z="{p[2]:.5f}"/>' for p in w)
        tx = "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"/>' for a, b, c in t)
        n = sum(1 for oo, _ in items[:k] if oo == o)
        res.append(f'<object id="{k + 1}" name="{name}{"_" + str(n + 1) if n else ""}" type="model"><mesh>'
                   f'<vertices>{vx}</vertices><triangles>{tx}</triangles></mesh></object>')
        build.append(f'<item objectid="{k + 1}"/>')
    build = "".join(build)
    model = ('<?xml version="1.0" encoding="UTF-8"?>'
             '<model unit="millimeter" xml:lang="en-US" '
             'xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">'
             f'<resources>{"".join(res)}</resources><build>{build}</build></model>')
    ct = ('<?xml version="1.0" encoding="UTF-8"?>'
          '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
          '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>'
          '</Types>')
    rels = ('<?xml version="1.0" encoding="UTF-8"?>'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Target="/3D/3dmodel.model" Id="rel0" '
            'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ct)
        z.writestr("_rels/.rels", rels)
        z.writestr("3D/3dmodel.model", model)

# ------------------------------------------------------- Bambu Studio project
# Profile keys that name or gate a preset; never copied into a project.
_PROFILE_META = {"name", "inherits", "from", "setting_id", "instantiation", "type",
                 "description", "compatible_printers", "compatible_printers_condition"}

def bambu_settings(template_path, process_path, overrides):
    """Project settings: the template, then the process profile's keys, then
    overrides. Each value takes the shape the template has for that key
    (string vs one-element list). Keys the template lacks are skipped, since
    this Bambu Studio version doesn't know them."""
    base = json.load(open(template_path))
    def shaped(key, val):
        if isinstance(val, list) and not isinstance(base[key], list):
            val = val[0]
        if isinstance(base[key], list) and not isinstance(val, list):
            val = [val]
        return [str(x) for x in val] if isinstance(val, list) else str(val)
    process = json.load(open(process_path))
    for src in (process, overrides):
        for k, v in src.items():
            if k in base and k not in _PROFILE_META:
                base[k] = shaped(k, v)
    # Bambu Studio re-applies the named system preset on load and keeps only
    # the keys listed here: [process, one entry per filament, printer], each
    # a sorted ';'-joined list. Unlisted changes are silently reset.
    changed = sorted(k for k, v in overrides.items()
                     if k in base and k not in _PROFILE_META and k != "print_settings_id"
                     and (k not in process or shaped(k, process[k]) != base[k]))
    n_fil = len(base.get("filament_settings_id", [""]))
    base["different_settings_to_system"] = [";".join(changed)] + [""] * n_fil + [""]
    return base

def _world(v, t, m):
    """Apply a 12-float row-vector transform; a mirror flips the winding."""
    w = [(p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9],
          p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10],
          p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11]) for p in v]
    return w, ([(a, c, b) for a, b, c in t] if det3(m) < 0 else list(t))

def write_bambu_3mf(path, objects, items, settings, thumbnail_png=None):
    """Bambu Studio project 3MF: one object per item, each mesh centred on its
    own bounding box in 3D/Objects/object_N.model, the item transform a pure
    translation to that centre, and `settings` (flat dict) as
    Metadata/project_settings.config. thumbnail_png is the plate image bytes."""
    H = '<?xml version="1.0" encoding="UTF-8"?>\n'
    NS = ('unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" '
          'xmlns:BambuStudio="http://schemas.bambulab.com/package/2021" '
          'xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" requiredextensions="p"')
    files, wrappers, build, rels, cfg, plate_objs, boxes = {}, [], [], [], [], [], []
    seen = {}
    for k, (o, m) in enumerate(items):
        name, v, t = objects[o]
        n = seen.get(o, 0); seen[o] = n + 1
        if n: name += f"_{n + 1}"
        w, t = _world(v, t, m)
        lo = [min(p[i] for p in w) for i in range(3)]
        hi = [max(p[i] for p in w) for i in range(3)]
        c = [(a + b) / 2 for a, b in zip(lo, hi)]
        boxes.append((name, lo, hi))
        vx = "\n".join(f'     <vertex x="{p[0] - c[0]:.7g}" y="{p[1] - c[1]:.7g}" z="{p[2] - c[2]:.7g}"/>' for p in w)
        tx = "\n".join(f'     <triangle v1="{a}" v2="{b}" v3="{d}"/>' for a, b, d in t)
        mid, wid, num = 2 * k + 1, 2 * k + 2, k + 1
        files[f"3D/Objects/object_{num}.model"] = (
            f'{H}<model {NS}>\n <metadata name="BambuStudio:3mfVersion">1</metadata>\n <resources>\n'
            f'  <object id="{mid}" p:UUID="{num:04x}0000-81cb-4c03-9d28-80fed5dfa1dc" type="model">\n'
            f'   <mesh>\n    <vertices>\n{vx}\n    </vertices>\n    <triangles>\n{tx}\n    </triangles>\n'
            f'   </mesh>\n  </object>\n </resources>\n <build/>\n</model>\n')
        wrappers.append(
            f'  <object id="{wid}" p:UUID="{num:08x}-61cb-4c03-9d28-80fed5dfa1dc" type="model">\n   <components>\n'
            f'    <component p:path="/3D/Objects/object_{num}.model" objectid="{mid}" '
            f'p:UUID="{num:04x}0000-b206-40ff-9872-83e8017abed1" transform="1 0 0 0 1 0 0 0 1 0 0 0"/>\n'
            f'   </components>\n  </object>\n')
        tr = f"1 0 0 0 1 0 0 0 1 {c[0]:.7g} {c[1]:.7g} {c[2]:.7g}"
        build.append(f'  <item objectid="{wid}" p:UUID="{num + 1:08x}-b1ec-4553-aec9-835e5b724bb4" '
                     f'transform="{tr}" printable="1"/>\n')
        rels.append(f' <Relationship Target="/3D/Objects/object_{num}.model" Id="rel-{num}" '
                    f'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>\n')
        nm = quoteattr(name)
        nf = len(t)
        cfg.append(
            f'  <object id="{wid}">\n    <metadata key="name" value={nm}/>\n    <metadata key="extruder" value="1"/>\n'
            f'    <metadata face_count="{nf}"/>\n    <part id="1" subtype="normal_part">\n'
            f'      <metadata key="name" value={nm}/>\n'
            f'      <metadata key="matrix" value="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"/>\n'
            f'      <mesh_stat face_count="{nf}" edges_fixed="0" degenerate_facets="0" facets_removed="0" '
            f'facets_reversed="0" backwards_edges="0"/>\n    </part>\n  </object>\n')
        plate_objs.append(
            f'    <model_instance>\n      <metadata key="object_id" value="{wid}"/>\n'
            f'      <metadata key="instance_id" value="0"/>\n'
            f'      <metadata key="identify_id" value="{4000 + num}"/>\n    </model_instance>\n')
        cfg.append(("assemble", f'   <assemble_item object_id="{wid}" instance_id="0" transform="{tr}" offset="0 0 0" />\n'))
    objs_xml = "".join(x for x in cfg if isinstance(x, str))
    asm_xml = "".join(x[1] for x in cfg if isinstance(x, tuple))
    thumb = ""
    if thumbnail_png:
        thumb = '    <metadata key="thumbnail_file" value="Metadata/plate_1.png"/>\n'
    model_settings = (
        f'{H}<config>\n{objs_xml}  <plate>\n    <metadata key="plater_id" value="1"/>\n'
        f'    <metadata key="plater_name" value=""/>\n    <metadata key="locked" value="false"/>\n'
        f'    <metadata key="filament_map_mode" value="Auto For Flush"/>\n'
        f'    <metadata key="filament_maps" value="1"/>\n{thumb}{"".join(plate_objs)}  </plate>\n'
        f'  <assemble>\n{asm_xml}  </assemble>\n</config>\n')
    meta_thumb = ""
    if thumbnail_png:
        meta_thumb = (' <metadata name="Thumbnail_Middle">/Metadata/plate_1.png</metadata>\n'
                      ' <metadata name="Thumbnail_Small">/Metadata/plate_1_small.png</metadata>\n')
    model = (f'{H}<model {NS}>\n <metadata name="Application">BambuStudio-02.02.00.85</metadata>\n'
             ' <metadata name="BambuStudio:3mfVersion">1</metadata>\n <metadata name="Copyright"></metadata>\n'
             ' <metadata name="CreationDate">2026-01-01</metadata>\n <metadata name="Description"></metadata>\n'
             ' <metadata name="Designer"></metadata>\n <metadata name="DesignerCover"></metadata>\n'
             ' <metadata name="DesignerUserId"></metadata>\n <metadata name="License"></metadata>\n'
             f' <metadata name="ModificationDate">2026-01-01</metadata>\n <metadata name="Origin"></metadata>\n'
             f'{meta_thumb} <metadata name="Title"></metadata>\n <resources>\n{"".join(wrappers)} </resources>\n'
             f' <build p:UUID="2c7c17d8-22b5-4d84-8835-1976022ea369">\n{"".join(build)} </build>\n</model>\n')
    allbox = (min(b[1][0] for b in boxes), min(b[1][1] for b in boxes),
              max(b[2][0] for b in boxes), max(b[2][1] for b in boxes))
    plate_json = {"bbox_all": list(allbox),
                  "bbox_objects": [{"area": (hi[0] - lo[0]) * (hi[1] - lo[1]),
                                    "bbox": [lo[0], lo[1], hi[0], hi[1]], "id": 4001 + i,
                                    "layer_height": float(settings.get("layer_height", "0.2")),
                                    "name": nm} for i, (nm, lo, hi) in enumerate(boxes)],
                  "bed_type": "textured_plate", "filament_colors": [], "filament_ids": [],
                  "first_extruder": 0, "is_seq_print": False, "nozzle_diameter": 0.4, "version": 2}
    ct = (f'{H}<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">\n'
          ' <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>\n'
          ' <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>\n'
          ' <Default Extension="png" ContentType="image/png"/>\n'
          ' <Default Extension="gcode" ContentType="text/x.gcode"/>\n</Types>')
    top = [' <Relationship Target="/3D/3dmodel.model" Id="rel-1" '
           'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>\n']
    if thumbnail_png:
        top += [' <Relationship Target="/Metadata/plate_1.png" Id="rel-2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail"/>\n',
                ' <Relationship Target="/Metadata/plate_1.png" Id="rel-4" Type="http://schemas.bambulab.com/package/2021/cover-thumbnail-middle"/>\n',
                ' <Relationship Target="/Metadata/plate_1_small.png" Id="rel-5" Type="http://schemas.bambulab.com/package/2021/cover-thumbnail-small"/>\n']
    R = f'{H}<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">\n'
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ct)
        z.writestr("_rels/.rels", R + "".join(top) + "</Relationships>")
        z.writestr("3D/_rels/3dmodel.model.rels", R + "".join(rels) + "</Relationships>")
        z.writestr("3D/3dmodel.model", model)
        for fn, body in files.items():
            z.writestr(fn, body)
        z.writestr("Metadata/model_settings.config", model_settings)
        z.writestr("Metadata/project_settings.config", json.dumps(settings, indent=4))
        z.writestr("Metadata/plate_1.json", json.dumps(plate_json, separators=(",", ":")))
        z.writestr("Metadata/slice_info.config",
                   f'{H}<config>\n  <header>\n    <header_item key="X-BBL-Client-Type" value="slicer"/>\n'
                   '    <header_item key="X-BBL-Client-Version" value="02.02.00.85"/>\n  </header>\n</config>\n')
        z.writestr("Metadata/cut_information.xml",
                   f'{H}<objects>\n' + "".join(
                       f' <object id="{2 * k + 2}">\n  <cut_id id="0" check_sum="1" connectors_cnt="0"/>\n </object>\n'
                       for k in range(len(items))) + '</objects>')
        if thumbnail_png:
            z.writestr("Metadata/plate_1.png", thumbnail_png)
            z.writestr("Metadata/plate_1_small.png", thumbnail_png)

def det3(m):
    return (m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6])
            + m[2] * (m[3] * m[7] - m[4] * m[6]))

def translate(x, y, z=0.0):
    return (1, 0, 0, 0, 1, 0, 0, 0, 1, x, y, z)

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("stl"); ap.add_argument("out")
    ap.add_argument("--copies", type=int, default=1)
    ap.add_argument("--gap", type=float, default=8.0)
    ap.add_argument("--bed", type=float, default=180.0)
    a = ap.parse_args()
    v, t = read_stl(a.stl)
    w = max(p[0] for p in v) - min(p[0] for p in v)
    total = a.copies * w + (a.copies - 1) * a.gap
    if total > a.bed - 10:
        sys.exit(f"row of {total:.0f} mm does not fit the {a.bed:.0f} mm bed")
    items = [(0, translate(a.bed / 2 - total / 2 + w / 2 + i * (w + a.gap), a.bed / 2))
             for i in range(a.copies)]
    write_3mf(a.out, [("part", v, t)], items)
    print(a.out)
