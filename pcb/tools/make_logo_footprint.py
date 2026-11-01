"""Bake silkscreen text into a font-independent KiCad footprint.

One line of text, centred on the footprint origin:

    $KICAD_PYTHON pcb/tools/make_logo_footprint.py "OSSTempo v1.0" Menlo 2.2 \\
        pcb/vendor_ecad/kicad_footprints/OSSTempo.pretty/Logo_OSSTempo_v1_0.kicad_mod

Several labels placed at exact offsets, from a JSON spec:

    $KICAD_PYTHON pcb/tools/make_logo_footprint.py --spec labels.json \\
        pcb/vendor_ecad/kicad_footprints/OSSTempo.pretty/Silk_Back_Pin_Labels.kicad_mod \\
        [--board build/.../osstempo_led_carrier.kicad_pcb]

With `--board`, the spec's "origin" (board coordinates the footprint is placed at) is
used to clip the art to the board outline and to add a keepout around every pad, via
and hole read from that board, so the art can run off the board edge safely.

    {"face": "Menlo", "description": "...",
     "items": [{"text": "GND", "size": 1.2, "at": [14.4, -23.46], "anchor": "left"}, ...]}

Items may also be graphic shapes ("ball", "comet"; see shape_outlines).
`fit_width_mm` rescales a text item so its rendered width is exactly that.
`at` is in mm from the footprint origin, in the footprint's own (front-side) frame;
`anchor` is left, centre or right and sets which edge of the text sits on `at`
(vertically the text is always centred on `at`). A footprint placed on the back is
mirrored by KiCad, so author back-side labels as they read from the back.

Why not just put TTF text on the board: KiCad renders a TrueType face with whatever
is installed on the machine doing the export, and the fabrication gerbers are
produced inside a pinned container that has no fonts at all. The text would
silently fall back to the stroke font in the files that actually go to the fab.

So this does what KiCad's own Image Converter does for logos: render the text with
the font once, here, and emit the glyph *outlines* as filled polygons. Holes (the
counters of O, o, e, p, 0) are turned into keyhole slits by SHAPE_POLY_SET.Fracture,
which is how KiCad itself represents them in a footprint. The result is plain
geometry that every KiCad, KiBot, and gerber viewer renders identically.

Must run under KiCad's bundled Python (pcbnew). The font face is resolved by the
board parser, which is the only path the Python bindings expose for choosing one.
"""
from __future__ import annotations

import json
import math
import sys
import tempfile
import uuid
from pathlib import Path

import pcbnew

PROBE = """(kicad_pcb (version 20241229) (generator "osstempo_logo") (generator_version "10.0")
  (general (thickness 1.6) (legacy_teardrops no))
  (paper "A4")
  (layers (0 "F.Cu" signal) (2 "B.Cu" signal) (37 "F.SilkS" user "F.Silkscreen"))
  (gr_text {text} (at 0 0) (layer "F.SilkS") (uuid "{uid}")
    (effects (font (face {face}) (size {size} {size}) (thickness {thick}){bold}{italic}) (justify left top)))
)
"""

Outline = list[tuple[float, float]]


def q(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def text_outlines(text: str, face: str, size_mm: float, at: tuple[float, float] = (0.0, 0.0),
                  anchor: str = "centre", italic: bool = False, skew_deg: float = 0.0,
                  bold: bool = True, embolden_mm: float = 0.0) -> list[Outline]:
    """Glyph outlines of `text` in mm, positioned so `anchor` edge / vertical centre is `at`.

    `italic` asks the font for its italic face; `skew_deg` slants the baked outlines
    (positive leans right), for faces that have no italic of their own. `embolden_mm`
    grows every outline by that much, to lift script hairlines above the fab's minimum
    silkscreen line width. Raises if KiCad could not find `face` and silently substituted
    another one (it renders a nonsense face name too and compares) - a silent substitution
    is exactly what this tool exists to prevent.
    """
    polys = _render_text(text, face, size_mm, bold, italic)
    fallback = _render_text(text, "__no_such_face__", size_mm, bold, italic)
    if polys.OutlineCount() == fallback.OutlineCount() and polys.BBox() == fallback.BBox():
        raise SystemExit(f"font face {face!r} not found (KiCad silently substituted another face)")
    if embolden_mm > 0:
        polys.Inflate(pcbnew.FromMM(embolden_mm), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, pcbnew.FromMM(0.01))
        polys.Fracture()
    return _place(polys, at, anchor, skew_deg)


def _render_text(text: str, face: str, size_mm: float, bold: bool, italic: bool) -> "pcbnew.SHAPE_POLY_SET":
    thick = round(size_mm * 0.16, 3)
    with tempfile.TemporaryDirectory() as tmp:
        probe = Path(tmp) / "probe.kicad_pcb"
        probe.write_text(
            PROBE.format(text=q(text), face=q(face), size=size_mm, thick=thick, uid=uuid.uuid4(),
                         bold=" (bold yes)" if bold else "", italic=" (italic yes)" if italic else ""),
            encoding="utf-8",
        )
        board = pcbnew.LoadBoard(str(probe))

    txt = next(d for d in board.GetDrawings() if d.GetClass() == "PCB_TEXT")
    polys = pcbnew.SHAPE_POLY_SET()
    txt.TransformTextToPolySet(polys, 0, pcbnew.FromMM(0.005), pcbnew.ERROR_INSIDE)
    polys.Fracture()
    return polys


def _place(polys: "pcbnew.SHAPE_POLY_SET", at: tuple[float, float], anchor: str, skew_deg: float) -> list[Outline]:
    mm = pcbnew.ToMM
    bb = polys.BBox()
    x0, x1 = mm(bb.GetX()), mm(bb.GetX() + bb.GetWidth())
    ref_x = {"left": x0, "right": x1}.get(anchor, (x0 + x1) / 2)
    ref_y = mm(bb.GetY() + bb.GetHeight() / 2)
    shear = math.tan(math.radians(skew_deg))
    out: list[Outline] = []
    for i in range(polys.OutlineCount()):
        o = polys.Outline(i)
        pts = []
        for k in range(o.PointCount()):
            x, y = mm(o.CPoint(k).x) - ref_x, mm(o.CPoint(k).y) - ref_y
            pts.append((x - shear * y + at[0], y + at[1]))
        out.append(pts)
    return out


def _circle(polys: "pcbnew.SHAPE_POLY_SET", cx: float, cy: float, r: float, segs: int = 48) -> None:
    polys.NewOutline()
    for k in range(segs):
        a = 2 * math.pi * k / segs
        polys.Append(pcbnew.FromMM(cx + r * math.cos(a)), pcbnew.FromMM(cy + r * math.sin(a)))


def _to_outlines(polys: "pcbnew.SHAPE_POLY_SET") -> list[Outline]:
    polys.Fracture()
    mm = pcbnew.ToMM
    return [[(mm(o.CPoint(k).x), mm(o.CPoint(k).y)) for k in range(o.PointCount())]
            for o in (polys.Outline(i) for i in range(polys.OutlineCount()))]


def _from_outlines(outlines: list[Outline]) -> "pcbnew.SHAPE_POLY_SET":
    polys = pcbnew.SHAPE_POLY_SET()
    for o in outlines:
        polys.NewOutline()
        for x, y in o:
            polys.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
    polys.Simplify()
    return polys


def _xor(a: "pcbnew.SHAPE_POLY_SET", b: "pcbnew.SHAPE_POLY_SET") -> None:
    """a := a XOR b, for pcbnew builds without BooleanXor."""
    a_only = a.CloneDropTriangulation()
    a_only.BooleanSubtract(b)
    b_only = b.CloneDropTriangulation()
    b_only.BooleanSubtract(a)
    a.RemoveAllContours()
    a.BooleanAdd(a_only)
    a.BooleanAdd(b_only)


def shape_outlines(item: dict, blocker: "pcbnew.SHAPE_POLY_SET | None" = None) -> list[Outline]:
    """Graphic shapes, all in mm in the footprint frame.

    ball:  {"shape": "ball", "at": [x, y], "r": 4.6, "dimple_r": 0.4, "dimple_pitch": 1.3}
           a filled disc with a hex grid of round dimples cut out of it.
    arrow: {"shape": "arrow", "tip": [x, y], "dir": "left"|"right"|"up"|"down", "len": 1.4, "width": 1.2}
           a filled triangle whose point is at `tip`.
    ball3d: {"shape": "ball3d", "at": [x, y], "r": 19, "dimples": 700, "dimple_frac": 0.42,
             "highlight_r": 0.25, "light": [-0.45, -0.55]}
           a golf ball in perspective: dimples on a sphere, projected (see the code).
    disc:  {"shape": "disc", "at": [x, y], "r": 1.3}  (mostly used as a keepout)
    comet: {"shape": "comet", "center": [x, y], "radius": 7.5, "from_deg": -20, "to_deg": -265,
            "count": 12, "r_start": 0.2, "r_end": 0.8}
           dots along a circular arc (y grows downward), growing from r_start to r_end.
    """
    polys = pcbnew.SHAPE_POLY_SET()
    if item["shape"] == "ball":
        cx, cy = item["at"]
        r, dr, pitch = item["r"], item["dimple_r"], item["dimple_pitch"]
        _circle(polys, cx, cy, r, 96)
        holes = pcbnew.SHAPE_POLY_SET()
        rows = int(r / (pitch * 0.866)) + 1
        for j in range(-rows, rows + 1):
            y = j * pitch * 0.866
            off = (pitch / 2) if j % 2 else 0.0
            for i in range(-rows - 1, rows + 2):
                x = i * pitch + off
                d = math.hypot(x, y)
                # "shade": dimples shrink toward the rim, so the flat disc reads as a sphere
                rr = dr * (1 - item.get("shade", 0.0) * (1 - math.sqrt(max(0.0, 1 - (d / r) ** 2))))
                # a dimple is dropped whole (never trimmed) if it would touch a label or pad ring
                near = blocker is not None and blocker.OutlineCount() and blocker.Collide(
                    pcbnew.VECTOR2I(pcbnew.FromMM(cx + x), pcbnew.FromMM(cy + y)), pcbnew.FromMM(rr + 0.45))
                if d + rr <= r - item.get("rim", 0.35) and rr >= 0.12 and not near:
                    _circle(holes, cx + x, cy + y, rr, 20)
        polys.BooleanSubtract(holes)
    elif item["shape"] == "ball3d":
        # A sphere seen head-on: dimples spread evenly over the sphere (Fibonacci lattice),
        # then projected. A round dimple whose surface normal tilts away from the viewer
        # projects to an ellipse, full size along the rim direction and squashed by cos(tilt)
        # toward the centre - so dimples are round mid-ball and thin slivers at the edge,
        # which is what makes it read as a ball. Dimples too thin to print are dropped.
        cx, cy = item["at"]
        r = item["r"]
        n = int(item["dimples"])
        frac = item.get("dimple_frac", 0.42)
        spacing = r * math.sqrt(4 * math.pi / n)
        dr = frac * spacing
        lx, ly = item.get("light", [-0.45, -0.55])  # specular highlight centre, in units of r
        # light direction (unit 3D vector toward the light; screen y grows downward) and its
        # screen-plane direction, used for the crescent shadows and the form shadow
        lz = math.sqrt(max(0.0, 1 - lx * lx - ly * ly))
        L = (lx, ly, lz)
        sl = math.hypot(lx, ly) or 1.0
        sx, sy = lx / sl, ly / sl
        _circle(polys, cx, cy, r, 180)
        holes = pcbnew.SHAPE_POLY_SET()
        golden = math.pi * (3 - math.sqrt(5))
        for k in range(n):
            nz = 1 - 2 * (k + 0.5) / n
            ring = math.sqrt(max(0.0, 1 - nz * nz))
            nx, ny = ring * math.cos(golden * k), ring * math.sin(golden * k)
            if nz <= 0:
                continue
            minor = dr * nz
            if minor < item.get("min_feature", 0.18):
                continue
            if math.hypot(nx - lx, ny - ly) < item.get("highlight_r", 0.0):
                continue
            px, py = cx + r * nx, cy + r * ny
            if math.hypot(px - cx, py - cy) + dr > r - item.get("rim", 0.3):
                continue
            if blocker is not None and blocker.OutlineCount() and blocker.Collide(
                    pcbnew.VECTOR2I(pcbnew.FromMM(px), pcbnew.FromMM(py)), pcbnew.FromMM(dr + item.get("clearance", 0.3))):
                continue
            ang = math.atan2(ny, nx)

            def ellipse(ox: float, oy: float) -> "pcbnew.SHAPE_POLY_SET":
                e = pcbnew.SHAPE_POLY_SET()
                e.NewOutline()
                for j in range(28):
                    t = 2 * math.pi * j / 28
                    a_rad, a_tan = minor * math.cos(t), dr * math.sin(t)
                    e.Append(pcbnew.FromMM(px + ox + a_rad * math.cos(ang) - a_tan * math.sin(ang)),
                             pcbnew.FromMM(py + oy + a_rad * math.sin(ang) + a_tan * math.cos(ang)))
                return e

            if item.get("style") == "crescent":
                # A dimple is a cup: lit from the upper left, its rim shades the upper-left
                # inside wall. Print only that shadow - the ellipse minus itself shifted away
                # from the light - and make it thicker where the ball turns away from the light.
                lit = max(0.0, nx * L[0] + ny * L[1] + nz * L[2])
                a, b_, pw = item.get("shadow_curve", [0.35, 0.75, 1.0])
                w = dr * (a + b_ * (1 - lit) ** pw)
                if w < item.get("min_feature", 0.18):
                    if item.get("drop_thin", False):
                        continue  # lit side: the cup casts no printable shadow, leave it clean
                    w = item.get("min_feature", 0.18)
                cup = ellipse(0.0, 0.0)
                cup.BooleanSubtract(ellipse(-sx * w, -sy * w))
                holes.BooleanAdd(cup)
            else:
                holes.BooleanAdd(ellipse(0.0, 0.0))
        polys.BooleanSubtract(holes)
        if item.get("terminator", 0.0) > 0:
            # Form shadow on the side away from the light: a dark band inside the lower-right
            # edge, leaving a thin bright rim outside it (reflected light), so the disc reads
            # as a lit sphere rather than a flat circle.
            band = pcbnew.SHAPE_POLY_SET()
            _circle(band, cx, cy, r - item.get("rim_light", 0.45), 180)
            lit_part = pcbnew.SHAPE_POLY_SET()
            t = item["terminator"]
            _circle(lit_part, cx + sx * t, cy + sy * t, r, 180)
            band.BooleanSubtract(lit_part)
            if blocker is not None and blocker.OutlineCount():
                # keep a white halo around labels and pads that fall on the band, so text
                # knocked out of the ball stays dark-on-white there too
                halo = blocker.CloneDropTriangulation()
                halo.Inflate(pcbnew.FromMM(item.get("clearance", 0.3) + 0.2), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, pcbnew.FromMM(0.01))
                band.BooleanSubtract(halo)
            polys.BooleanSubtract(band)
    elif item["shape"] == "arrow":
        tx, ty = item["tip"]
        ln, w = item.get("len", 1.4), item.get("width", 1.2)
        ux, uy = {"left": (-1, 0), "right": (1, 0), "up": (0, -1), "down": (0, 1)}[item["dir"]]
        bx, by = tx - ux * ln, ty - uy * ln
        polys.NewOutline()
        for x, y in ((tx, ty), (bx - uy * w / 2, by + ux * w / 2), (bx + uy * w / 2, by - ux * w / 2)):
            polys.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
    elif item["shape"] == "disc":
        _circle(polys, item["at"][0], item["at"][1], item["r"], 32)
    elif item["shape"] == "comet":
        cx, cy = item["center"]
        n = int(item["count"])
        for k in range(n):
            t = k / (n - 1)
            a = math.radians(item["from_deg"] + t * (item["to_deg"] - item["from_deg"]))
            rr = item["r_start"] + t * (item["r_end"] - item["r_start"])
            _circle(polys, cx + item["radius"] * math.cos(a), cy + item["radius"] * math.sin(a), rr, 32)
        polys.Simplify()
    else:
        raise ValueError(f"unknown shape {item['shape']!r}")
    return _to_outlines(polys)


def write_footprint(out: Path, value: str, descr: str, outlines: list[Outline]) -> None:
    ys = [y for o in outlines for _, y in o]
    half_h = (max(ys) - min(ys)) / 2 if ys else 0.0
    lines = [
        f"(footprint {q(out.stem)}",
        '\t(version 20241229)',
        '\t(generator "osstempo_make_logo_footprint")',
        '\t(generator_version "10.0")',
        '\t(layer "F.Cu")',
        f"\t(descr {q(descr)})",
        '\t(attr board_only exclude_from_pos_files exclude_from_bom)',
        f'\t(property "Reference" "REF**" (at 0 {half_h + 1.0:.3f} 0) (layer "F.SilkS") hide',
        '\t\t(effects (font (size 1 1) (thickness 0.15))))',
        f'\t(property "Value" {q(value)} (at 0 {-(half_h + 1.0):.3f} 0) (layer "F.Fab") hide',
        '\t\t(effects (font (size 1 1) (thickness 0.15))))',
        '\t(property "Datasheet" "" (at 0 0 0) (layer "F.Fab") hide (effects (font (size 1 1) (thickness 0.15))))',
        '\t(property "Description" "" (at 0 0 0) (layer "F.Fab") hide (effects (font (size 1 1) (thickness 0.15))))',
    ]
    for i, outline in enumerate(outlines):
        pts = " ".join(f"(xy {x:.4f} {y:.4f})" for x, y in outline)
        lines += [
            "\t(fp_poly",
            f"\t\t(pts {pts})",
            "\t\t(stroke (width 0) (type solid))",
            "\t\t(fill yes)",
            '\t\t(layer "F.SilkS")',
            f'\t\t(uuid "{uuid.uuid5(uuid.NAMESPACE_URL, f"osstempo-logo/{out.stem}/{descr}/{i}")}")',
            "\t)",
        ]
    lines.append(")")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    xs = [x for o in outlines for x, _ in o]
    print(f"{out}: {len(outlines)} outlines, {max(xs) - min(xs):.2f} x {2 * half_h:.2f} mm")


def board_constraints(board_path: Path, origin: tuple[float, float], margin: float,
                      clearance: float) -> tuple["pcbnew.SHAPE_POLY_SET", "pcbnew.SHAPE_POLY_SET"]:
    """(clip, keepout) in the footprint's local frame, read from the real board.

    clip is the board outline deflated by `margin`; keepout is a disc around every pad,
    via and mounting hole, grown by `clearance`. The footprint goes on the back, where
    KiCad mirrors it, so local x = origin_x - board_x and local y = board_y - origin_y.
    """
    board = pcbnew.LoadBoard(str(board_path))
    mm = pcbnew.ToMM
    ox, oy = origin
    to_local = lambda v: (pcbnew.FromMM(ox - mm(v.x)), pcbnew.FromMM(mm(v.y) - oy))

    outline = pcbnew.SHAPE_POLY_SET()
    board.GetBoardPolygonOutlines(outline, True)
    clip = pcbnew.SHAPE_POLY_SET()
    for i in range(outline.OutlineCount()):
        o = outline.Outline(i)
        clip.NewOutline()
        for k in range(o.PointCount()):
            clip.Append(*to_local(o.CPoint(k)))
    clip.Inflate(-pcbnew.FromMM(margin), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, pcbnew.FromMM(0.01))

    keepout = pcbnew.SHAPE_POLY_SET()
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetAttribute() == pcbnew.PAD_ATTRIB_SMD and not pad.IsOnLayer(pcbnew.B_Cu):
                continue  # front-side SMD pads never meet the back silkscreen
            x, y = to_local(pad.GetPosition())
            sz = pad.GetSize()
            if pad.GetShape() == pcbnew.PAD_SHAPE_CIRCLE:
                reach = max(mm(sz.x), mm(sz.y)) / 2
            else:  # square / rect / oval pin-1 pads: the corner is the farthest copper
                reach = math.hypot(mm(sz.x), mm(sz.y)) / 2
            _circle(keepout, mm(x), mm(y), reach + clearance, 32)
    for t in board.GetTracks():
        if t.GetClass() == "PCB_VIA":
            x, y = to_local(t.GetPosition())
            _circle(keepout, mm(x), mm(y), mm(t.GetWidth(pcbnew.B_Cu)) / 2 + clearance, 24)
    keepout.Simplify()
    return clip, keepout


def main(argv: list[str]) -> int:
    if argv[1:2] == ["--spec"] and len(argv) in (4, 6):
        spec = json.loads(Path(argv[2]).read_text(encoding="utf-8"))
        board_path = Path(argv[5]) if len(argv) == 6 and argv[4] == "--board" else None
        face = spec.get("face", "Menlo")
        # Each item has a "role": "base" artwork (e.g. a big golf ball), "ink" (text,
        # arrows; the default) and "keepout" (pad clearances). The result is
        # (base XOR ink) - keepout: ink inside the base is knocked out of it (board colour
        # shows through), ink outside it prints normally, and nothing ever lands on a pad.
        # The base shapes drop any dimple near ink or a keepout, so knockouts stay legible.
        layers = {role: pcbnew.SHAPE_POLY_SET() for role in ("base", "ink", "keepout")}
        clip = None
        if board_path is not None:
            bc = spec.get("board_constraints", {})
            clip, auto_keepout = board_constraints(board_path, tuple(spec.get("origin", [0.0, 0.0])),
                                                   float(bc.get("edge_margin_mm", 0.6)), float(bc.get("pad_clearance_mm", 0.3)))
            layers["keepout"].BooleanAdd(auto_keepout)
        # Ink and keepouts first: the base art (the ball) needs them to place its dimples.
        ordered = sorted(spec["items"], key=lambda it: it.get("role", "ink") == "base")
        for item in ordered:
            if "shape" in item:
                blocker = None
                if item.get("role") == "base":
                    blocker = layers["ink"].CloneDropTriangulation()
                    blocker.BooleanAdd(layers["keepout"])
                outs = shape_outlines(item, blocker)
            else:
                size = float(item["size"])
                for _pass in range(2):
                    outs = text_outlines(item["text"], item.get("face", face), size,
                                         tuple(item["at"]), item.get("anchor", "centre"),
                                         bool(item.get("italic", False)), float(item.get("skew_deg", 0.0)),
                                         bool(item.get("bold", True)), float(item.get("embolden_mm", 0.0)))
                    xs = [x for o in outs for x, _ in o]; ys = [y for o in outs for _, y in o]
                    if "fit_width_mm" not in item:
                        break
                    size *= float(item["fit_width_mm"]) / (max(xs) - min(xs))  # second pass lands on the width
                print(f"  {item['text']!r:24s} {max(xs) - min(xs):5.2f} x {max(ys) - min(ys):4.2f} mm", file=sys.stderr)
            target = layers[item.get("role", "ink")]
            part = _from_outlines(outs)
            target.BooleanAdd(part)
        art = layers["base"]
        art.BooleanXor(layers["ink"]) if hasattr(art, "BooleanXor") else _xor(art, layers["ink"])
        art.BooleanSubtract(layers["keepout"])
        if clip is not None:
            art.BooleanIntersection(clip)
        # Emit the art as a grid of tiles. The fabricated result is identical (adjacent
        # filled regions merge in the gerber), but one polygon with thousands of dimple
        # holes trips KiCad's 3D renderer; small tiles preview faithfully.
        tile = float(spec.get("tile_mm", 8.0))
        bb = art.BBox()
        outlines = []
        y = pcbnew.ToMM(bb.GetY())
        while y < pcbnew.ToMM(bb.GetBottom()):
            x = pcbnew.ToMM(bb.GetX())
            while x < pcbnew.ToMM(bb.GetRight()):
                cell = pcbnew.SHAPE_POLY_SET()
                cell.NewOutline()
                for cx_, cy_ in ((x, y), (x + tile, y), (x + tile, y + tile), (x, y + tile)):
                    cell.Append(pcbnew.FromMM(cx_), pcbnew.FromMM(cy_))
                part = art.CloneDropTriangulation()
                part.BooleanIntersection(cell)
                outlines += _to_outlines(part)
                x += tile
            y += tile
        texts = " / ".join(item.get("text", item.get("shape", "")) for item in spec["items"])
        write_footprint(Path(argv[3]), texts, spec.get("description", f"Silkscreen labels: {texts}"), outlines)
        return 0
    if len(argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    text, face, size_mm, out = argv[1], argv[2], float(argv[3]), Path(argv[4])
    write_footprint(out, text, f"Silkscreen logo: {text!r} in {face} Bold {size_mm} mm, baked to outlines",
                    text_outlines(text, face, size_mm))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
