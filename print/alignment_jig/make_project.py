"""Wrap alignment_jig.stl in a Bambu Studio project for the A1 mini, print settings embedded.
    python3 print/alignment_jig/make_project.py
Run build.py first. tools/make_3mf.py and profiles/ are copied from the openagentdeck desk stand."""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import make_3mf

BED = 180.0
# Applied over profiles/process.json (0.20mm Standard @BBL A1M).
SETTINGS = {
    "layer_height": "0.2",
    "wall_loops": "3",                    # the 2.4 mm cradle walls print as solid perimeter
    "top_shell_layers": "5",
    "bottom_shell_layers": "4",           # with 5 top layers the 2 mm plate is solid
    "sparse_infill_density": "15%",
    "elefant_foot_compensation": "0.15",  # keeps the plate outline to size
    "outer_wall_speed": "150",            # truer channel faces (stock: 200)
    "enable_support": "0",
    "brim_type": "no_brim",               # the part sits 2.5 mm from the bed edge: no room for one
    "skirt_loops": "0",
    "print_settings_id": "0.20mm Standard @BBL A1M",
}

verts, tris = make_3mf.read_stl(os.path.join(HERE, "alignment_jig.stl"))
lo = [min(v[i] for v in verts) for i in range(3)]
hi = [max(v[i] for v in verts) for i in range(3)]
cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
item = make_3mf.translate(BED / 2 - cx, BED / 2 - cy, -lo[2])
margin = min(BED / 2 - (hi[0] - lo[0]) / 2, BED / 2 - (hi[1] - lo[1]) / 2)
assert margin >= 2.0, f"only {margin:.1f} mm to the bed edge"

settings = make_3mf.bambu_settings(os.path.join(ROOT, "profiles", "bambu_project_settings.json"),
                                   os.path.join(ROOT, "profiles", "process.json"), SETTINGS)
out = os.path.join(HERE, "alignment_jig_A1mini.3mf")
make_3mf.write_bambu_3mf(out, [("alignment_jig", verts, tris)], [(0, item)], settings)
print(f"{out}: {len(tris)} triangles, {hi[0]-lo[0]:.1f} x {hi[1]-lo[1]:.1f} x {hi[2]-lo[2]:.1f} mm, {margin:.1f} mm to the bed edge")
print("changed from stock:", settings["different_settings_to_system"][0])
