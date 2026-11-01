# KiCad symbols used by the osstempo LED carrier

Only the three symbols the board needs, in KiCad 10's one-file-per-symbol library layout:

| File | Used by |
|---|---|
| `Device.kicad_symdir/C.kicad_sym` | C1–C6 |
| `Device.kicad_symdir/R.kicad_sym` | R1 |
| `power.kicad_symdir/PWR_FLAG.kicad_sym` | PF1, PF2 |

They are unmodified copies from the official KiCad symbol library,
<https://gitlab.com/kicad/libraries/kicad-symbols>, tag `10.0.0`
(commit `495444d353551ba5f610d47d22c979c50b0bd70c`). Per-file SHA-256 digests are in
`standard-symbols.acquisition.json`; `LICENSE.md` is that repository's licence file
(CC-BY-SA-4.0 with the KiCad libraries exception), copied from the same commit.

The pin-socket and JST symbols (H1, H2, J1) are declared inline in the board manifest
rather than imported, so nothing else is vendored here. To add a symbol, fetch it from the
same tag, record its digest in the acquisition file, and reference it from the manifest with
`source_type: official-kicad`.
