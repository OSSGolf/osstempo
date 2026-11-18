"""Put case_base.stl and case_lid.stl on one A1 mini plate as a Bambu Studio project, print settings embedded.
    python3 print/pcb_case/make_project.py
Run build.py first. The lid STL is already roof-down, so its logos print on the bed face."""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import make_3mf

BED, SPACE = 180.0, 8.0
# Applied over profiles/process.json (0.20mm Standard @BBL A1M).
SETTINGS = {
    "layer_height": "0.2",
    "wall_loops": "3",                    # the 1.6 mm lid wall and the latch tabs print as solid perimeter
    "sparse_infill_density": "15%",
    "elefant_foot_compensation": "0.15",  # keeps the lid's fit on the base rim and the pressed-in logos to size
    "outer_wall_speed": "150",            # cleaner logo edges and latch faces (stock: 200)
    "enable_support": "0",                # every overhang is 45 degrees or a short bridge
    "brim_type": "no_brim",
    "print_settings_id": "0.20mm Standard @BBL A1M",
}

objects, sizes = [], []
for name in ("case_base", "case_lid"):
    verts, tris = make_3mf.read_stl(os.path.join(HERE, name + ".stl"))
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]
    objects.append((name, verts, tris)); sizes.append((lo, hi))

width = sum(hi[0] - lo[0] for lo, hi in sizes) + SPACE
assert width <= BED - 10, f"parts are {width:.1f} mm side by side"
items, x = [], (BED - width) / 2
for k, (lo, hi) in enumerate(sizes):
    items.append((k, make_3mf.translate(x - lo[0], BED / 2 - (lo[1] + hi[1]) / 2, -lo[2])))
    x += hi[0] - lo[0] + SPACE

settings = make_3mf.bambu_settings(os.path.join(ROOT, "profiles", "bambu_project_settings.json"),
                                   os.path.join(ROOT, "profiles", "process.json"), SETTINGS)
out = os.path.join(HERE, "pcb_case_A1mini.3mf")
make_3mf.write_bambu_3mf(out, objects, items, settings)
for (name, _, tris), (lo, hi) in zip(objects, sizes):
    print(f"{name}: {len(tris)} triangles, {hi[0]-lo[0]:.1f} x {hi[1]-lo[1]:.1f} x {hi[2]-lo[2]:.1f} mm, bottom at z {lo[2]:.2f}")
print(f"{out}: {width:.1f} mm across the {BED:.0f} mm plate")
print("changed from stock:", settings["different_settings_to_system"][0])
