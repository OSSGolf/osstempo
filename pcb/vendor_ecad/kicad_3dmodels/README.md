# 3D models used by the osstempo LED carrier

Byte-identical copies from the KiCad 10.0.6 install
(`/Applications/KiCad/KiCad.app/Contents/SharedSupport/3dmodels/`), which ships the
official library <https://gitlab.com/kicad/libraries/kicad-packages3D> under
CC-BY-SA-4.0 with the KiCad libraries exception (same licence text as
`../kicad_symbols/LICENSE.md`). Only the STEP files the board references are kept — the
full library is hundreds of megabytes.

| Model | Used by |
|---|---|
| `Capacitor_SMD.3dshapes/C_0805_2012Metric.step` | C1, C2, C4, C5, C6 |
| `Capacitor_SMD.3dshapes/C_0603_1608Metric.step` | C3 |
| `Resistor_SMD.3dshapes/R_0805_2012Metric.step` | R1 |
| `Connector_PinSocket_2.54mm.3dshapes/PinSocket_1x15_P2.54mm_Vertical.step` | H1, H2 |
| `Connector_JST.3dshapes/JST_XH_S3B-XH-A_1x03_P2.50mm_Horizontal.step` | J1 |

Each component pack records the model's SHA-256 and the `${KIPRJMOD}/3dmodels/...` path the
generated project uses. The models are cosmetic (3D viewer and courtyard sanity); nothing in
the fabrication package depends on them.
