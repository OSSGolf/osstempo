"""Derive the hand-solder socket footprint from the official KiCad one.

    python3 pcb/tools/make_socket_footprint.py

Reads vendor_ecad/kicad_footprints/Connector_PinSocket_2.54mm.pretty/
PinSocket_1x15_P2.54mm_Vertical.kicad_mod (official, 1.7 mm pads on a 1.0 mm drill) and
writes OSSTempo.pretty/PinSocket_1x15_P2.54mm_Vertical_Pad2.0.kicad_mod: the same
footprint with 2.0 mm pads. Nothing else changes - pitch, drill, courtyard, silkscreen
and the 3D model reference are untouched, so the socket still fits and still renders.

Why: the ESP32 socket is hand-soldered (v1.0 was painful). A 2.0 mm pad has a 0.5 mm
annular ring instead of 0.35 mm - 40% more copper to rest the tip on and for the
solder to wet - while leaving 0.54 mm between neighbouring pads on the 2.54 mm pitch,
four times JLCPCB's minimum. The derived file is regenerated, never hand-edited; its
SHA-256 is recorded in component_packs/samtec-ssw-115-01-t-s.yaml.
"""
from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

PAD_MM = 2.0
ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "vendor_ecad/kicad_footprints/Connector_PinSocket_2.54mm.pretty/PinSocket_1x15_P2.54mm_Vertical.kicad_mod"
NAME = f"PinSocket_1x15_P2.54mm_Vertical_Pad{PAD_MM:.1f}"
OUT = ROOT / "vendor_ecad/kicad_footprints/OSSTempo.pretty" / f"{NAME}.kicad_mod"


def main() -> int:
    text = SRC.read_text(encoding="utf-8")
    pads = re.findall(r'\(pad "\d+" thru_hole \w+\s*\(at [^)]*\)\s*\(size 1\.7 1\.7\)\s*\(drill 1\)', text)
    if len(pads) != 15:
        print(f"expected 15 stock pads (size 1.7, drill 1) in {SRC.name}, found {len(pads)}", file=sys.stderr)
        return 1
    out = text.replace("(size 1.7 1.7)", f"(size {PAD_MM:g} {PAD_MM:g})")
    out = out.replace('(footprint "PinSocket_1x15_P2.54mm_Vertical"', f'(footprint "{NAME}"', 1)
    out = re.sub(r'\(descr "([^"]*)"', lambda m: f'(descr "{m.group(1)} - {PAD_MM:g} mm pads for hand soldering (derived by pcb/tools/make_socket_footprint.py)"', out, count=1)
    OUT.write_text(out, encoding="utf-8")
    digest = hashlib.sha256(OUT.read_bytes()).hexdigest()
    print(f"{OUT.relative_to(ROOT)}: {len(pads)} pads -> {PAD_MM:g} mm, sha256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
