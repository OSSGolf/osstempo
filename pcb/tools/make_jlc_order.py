#!/usr/bin/env python3
"""Build the JLCPCB upload set from a promoted kicad-agent fabrication release.

The release's `fabrication/` directory has the gerbers, the drill file and a
KiCad-format positions CSV. JLCPCB additionally wants

  * one zip of gerbers + drill,
  * a BOM CSV keyed by LCSC part number,
  * a CPL (pick-and-place) CSV in its own column/units convention,

and it wants the hand-soldered parts left out of both CSVs. This script derives
all three from the release, so a respin can never ship a hand-edited stale BOM.

Which parts JLC places is not a list kept here: a footprint is an assembly part
iff the board gives it a `JLCPCB Part #` property. H1/H2/J1 deliberately have
none (see pcb_build_spec.md, "Assembly split").

It also repairs one defect the toolkit bakes into every release. The
reproducibility pass rewrites each `CreationDate` to the literal string
`<normalized>` so two runs hash the same -- but it does that to the shipped
artifacts, not just to the hash input, and `<normalized>` is not a date. That
leaves `%TF.CreationDate,<normalized>*%` in the X2 header of every gerber and in
the Excellon, which is invalid per the Gerber spec and is the first thing a fab's
parser reads. We substitute the design revision's date on the way into the zip
and prove that nothing but those comment/attribute lines changed.

That alone was not enough for JLCPCB: its parser still stalled at ~96% until the
X2 attribute commands and the `.gbrjob` came out too, so stripping them is the
default and `--x2` opts back in. Gerbers only -- in the Excellon that same syntax
carries the NPTH marking for the M3 holes.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BOARD = "osstempo_led_carrier"
PLACEMENT_PROPERTY = "JLCPCB Part #"
# A deterministic zip needs a fixed mtime; the DOS epoch is what zipfile floors to.
ZIP_EPOCH = (1980, 1, 1, 0, 0, 0)
# What the toolkit's reproducibility pass leaves behind where a date belongs.
NORMALIZED_DATE = "<normalized>"
PLACEHOLDER_LABEL = NORMALIZED_DATE


def die(msg: str) -> None:
    sys.exit(f"error: {msg}")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_release(fab: Path, manifest: dict) -> None:
    """Refuse to ship a release whose files drifted from its own manifest."""
    for entry in manifest["files"]:
        path = fab / entry["path"]
        if not path.is_file():
            die(f"release is missing {entry['path']}")
        if (got := sha256(path)) != entry["sha256"]:
            die(f"{entry['path']} does not match the manifest ({got[:12]}...)")


def read_placements(board_file: Path) -> dict[str, dict[str, str]]:
    """Map reference -> footprint properties, for footprints JLC places."""
    text = board_file.read_text()
    placements: dict[str, dict[str, str]] = {}
    # Footprint blocks start at a top-level `(footprint "<lib:name>"`; properties
    # are flat `(property "k" "v")` pairs inside. No full s-expression parse needed.
    for block in text.split('\n\t(footprint ')[1:]:
        props = dict(re.findall(r'\(property "([^"]+)" "([^"]*)"', block))
        if not props.get(PLACEMENT_PROPERTY):
            continue
        lib_id = re.match(r'"([^"]+)"', block).group(1)
        props["_footprint"] = lib_id.split(":", 1)[-1]
        placements[props["Reference"]] = props
    if not placements:
        die(f"no footprint in {board_file.name} carries a '{PLACEMENT_PROPERTY}' property")
    return placements


def read_positions(csv_file: Path) -> dict[str, dict[str, str]]:
    with csv_file.open(newline="") as fh:
        return {row["Ref"]: row for row in csv.DictReader(fh)}


def write_bom(out: Path, placements: dict[str, dict[str, str]]) -> list[tuple]:
    """One row per LCSC part number, designators collapsed into one cell."""
    lines: dict[str, dict] = {}
    for ref, props in sorted(placements.items()):
        line = lines.setdefault(
            props[PLACEMENT_PROPERTY],
            {
                "comment": props["Value"],
                "footprint": props["_footprint"],
                "manufacturer": props.get("Manufacturer", ""),
                "mpn": props.get("MPN", ""),
                "refs": [],
            },
        )
        line["refs"].append(ref)

    rows = []
    with out.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for lcsc, line in sorted(lines.items(), key=lambda kv: kv[1]["refs"][0]):
            refs = ",".join(natural_sort(line["refs"]))
            writer.writerow([line["comment"], refs, line["footprint"], lcsc])
            rows.append((lcsc, line["comment"], refs, line["manufacturer"], line["mpn"]))
    return rows


def write_cpl(out: Path, placements: dict[str, dict[str, str]], positions: dict[str, dict[str, str]]) -> int:
    """JLC's column names and units. Y stays negated, matching the gerbers."""
    with out.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        written = 0
        for ref in natural_sort(placements):
            if (pos := positions.get(ref)) is None:
                die(f"{ref} is an assembly part but has no row in the positions file")
            writer.writerow([
                ref,
                f"{float(pos['PosX']):.4f}",
                f"{float(pos['PosY']):.4f}",
                pos["Side"],
                f"{float(pos['Rot']) % 360:.0f}",  # JLC wants [0,360), KiCad emits +/-180
            ])
            written += 1
    return written


def natural_sort(refs) -> list[str]:
    return sorted(refs, key=lambda r: (re.sub(r"\d+$", "", r), int(m.group() if (m := re.search(r"\d+$", r)) else 0)))


def revision_date(schematic: Path) -> str:
    """An ISO 8601 CreationDate, taken from the design's own title block.

    Deterministic on purpose: a wall-clock `now()` here would make the zip differ
    between runs, which is the property the reproducibility pass was protecting.
    """
    if m := re.search(r'\(date "(\d{4}-\d{2}-\d{2})"\)', schematic.read_text()):
        return f"{m.group(1)}T00:00:00+00:00"
    die(f"no title-block date in {schematic.name} to use as the gerber CreationDate")


def significant(data: bytes) -> list[bytes]:
    """The lines that carry geometry -- everything a comment or X2 attribute isn't."""
    return [ln for ln in data.splitlines()
            if not ln.startswith((b"G04", b";", b"%TF", b"%TA", b"%TO", b"%TD"))]


def patch_date(member: str, data: bytes, iso_date: str) -> tuple[bytes, int]:
    """Put a real date where the reproducibility pass left `<normalized>`."""
    placeholder = NORMALIZED_DATE.encode()
    if (hits := data.count(placeholder)) == 0:
        return data, 0
    patched = data.replace(placeholder, iso_date.encode())
    # The placeholder only ever sits on a comment or attribute line. If a
    # replacement ever touched a geometry line, that is a corrupted fab file.
    if significant(patched) != significant(data):
        die(f"patching the date in {member} changed a geometry line - refusing to ship it")
    return patched, hits


def strip_x2(member: str, data: bytes) -> bytes:
    """Reduce a gerber to plain X1 by dropping its attribute commands.

    Geometry-preserving: X2 attributes are metadata. Gerbers only -- in the
    Excellon the same syntax carries `TA.AperFunction,NonPlated,NPTH`, which is
    the *only* thing marking the four M3 holes as unplated. Strip that and they
    come back copper-lined.
    """
    kept = [ln for ln in data.split(b"\n")
            if not ln.startswith((b"%TF", b"%TA", b"%TO", b"%TD"))]
    stripped = b"\n".join(kept)
    if significant(stripped) != significant(data):
        die(f"stripping X2 from {member} changed a geometry line - refusing to ship it")
    return stripped


def write_zip(out: Path, fab: Path, manifest: dict, iso_date: str,
              strip: bool = True) -> tuple[list[str], int]:
    """Gerbers + drill, flat at the zip root, byte-reproducible."""
    members = sorted(
        e["path"] for e in manifest["files"]
        if e["path"].startswith((manifest["gerber_dir"] + "/", manifest["drill_dir"] + "/"))
        and not e["path"].endswith(".rpt")  # a human report, not a fab input
        and not (strip and e["path"].endswith(".gbrjob"))  # optional; JLC trips on it
    )
    repaired = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        for member in members:
            data, hits = patch_date(member, (fab / member).read_bytes(), iso_date)
            repaired += hits
            if strip and member.endswith(".gbr"):
                data = strip_x2(member, data)
            info = zipfile.ZipInfo(Path(member).name, date_time=ZIP_EPOCH)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zf.writestr(info, data)
    return [Path(m).name for m in members], repaired


def board_extents(edge_cuts: Path) -> tuple[float, float]:
    """Outline bounding box in mm, from the 4.6-format Edge.Cuts gerber."""
    coords = re.findall(r"X(-?\d{5,})Y(-?\d{5,})", edge_cuts.read_text())
    if not coords:
        die("could not read any coordinates from the Edge.Cuts gerber")
    xs = [int(x) / 1e6 for x, _ in coords]
    ys = [int(y) / 1e6 for _, y in coords]
    return max(xs) - min(xs), max(ys) - min(ys)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--release", type=Path, default=REPO / "pcb/build/kicad-agent" / BOARD / "current",
                    help="promoted release directory (default: .../current)")
    ap.add_argument("--out", type=Path, default=REPO / "pcb/build/jlcpcb_order",
                    help="output directory (default: pcb/build/jlcpcb_order)")
    ap.add_argument("--x2", action="store_true",
                    help="keep the X2 attribute commands and the .gbrjob. JLCPCB's parser "
                         "stalled on that form (2026-09-17); the default strips both, which it "
                         "accepts. Geometry is identical either way.")
    args = ap.parse_args()

    fab = args.release / "fabrication"
    manifest_file = fab / "fabrication_manifest.json"
    if not manifest_file.is_file():
        die(f"no fabrication manifest at {manifest_file}\n"
            "       run `board validate` twice to promote a release (see pcb/README.md)")
    manifest = json.loads(manifest_file.read_text())
    verify_release(fab, manifest)

    revision = manifest["revision"]
    stem = f"{BOARD}-{revision}"
    args.out.mkdir(parents=True, exist_ok=True)

    placements = read_placements(args.release / "project" / f"{BOARD}.kicad_pcb")
    positions = read_positions(fab / "executor/positions" / f"{BOARD}-positions.csv")

    zip_path = args.out / f"{stem}-gerbers{'-x2' if args.x2 else ''}.zip"
    bom_path = args.out / f"{stem}-bom.csv"
    cpl_path = args.out / f"{stem}-cpl.csv"

    iso_date = revision_date(args.release / "project" / f"{BOARD}.kicad_sch")
    members, repaired = write_zip(zip_path, fab, manifest, iso_date, strip=not args.x2)
    bom_rows = write_bom(bom_path, placements)
    placed = write_cpl(cpl_path, placements, positions)

    width, height = board_extents(fab / manifest["gerber_dir"] / f"{BOARD}-Edge_Cuts.gbr")
    skipped = natural_sort(set(positions) - set(placements))

    print(f"release   {args.release.resolve()}")
    print(f"revision  {revision}  (board {manifest['routed_board_sha256'][:12]}, manifest hashes verified)")
    print(f"outline   {width:.1f} x {height:.1f} mm, {manifest['copper_layer_count']} layer, "
          f"{manifest['board_thickness_mm']} mm\n")
    print(f"{zip_path.name}  ({len(members)} files: {', '.join(m.rsplit('-', 1)[-1] for m in members)})")
    print(f"    CreationDate: {repaired} '{PLACEHOLDER_LABEL}' placeholders -> {iso_date} "
          f"(geometry verified unchanged)")
    print("    X2 attributes: " + ("KEPT, .gbrjob included - JLCPCB stalls on this form"
                                     if args.x2 else
                                     "stripped from the gerbers, .gbrjob omitted, drill intact"))
    print(f"{bom_path.name}  ({len(bom_rows)} part numbers, {placed} placements)")
    for lcsc, comment, refs, mfr, mpn in bom_rows:
        print(f"    {lcsc:8} {comment:7} {refs:20} {mfr} {mpn}")
    print(f"{cpl_path.name}  ({placed} placements, top side)")
    print(f"\nleft unpopulated (hand-soldered, no '{PLACEMENT_PROPERTY}'): {', '.join(skipped) or 'none'}")


if __name__ == "__main__":
    main()
