# osstempo_led_carrier

Carrier / "shield" PCB for the osstempo putting tempo trainer. A 30-pin ESP32 dev board
drops into two 15-way sockets; JLCPCB assembles four SMT passives; you hand-solder the
three connectors.

**[`../../../pcb_build_spec.md`](../../../pcb_build_spec.md) is the authority on what this
board is.** This directory is only how it gets generated. Renders of both sides, and the
v1.0-vs-v1.2 comparison, are in [`../../README.md`](../../README.md).

## Build it

```sh
sh pcb/setup-toolkit.sh                            # once: venv, patches, ngspice, podman, KiBot image
sh pcb/build.sh                                    # the build (validate x2, reseed, order files); stops the podman VM after
source pcb/env.sh
$KICAD_AGENT board validate osstempo_led_carrier   # full gate stack; run twice to promote a release
sh pcb/reseed-routed.sh osstempo_led_carrier       # copy the accepted release to pcb/routed/
open -a KiCad pcb/routed/osstempo_led_carrier/osstempo_led_carrier.kicad_pro
```

`$KICAD_AGENT board checkout osstempo_led_carrier` also works once a release exists — it
is the toolkit's official writable copy. Fabrication files for JLCPCB are in
`pcb/build/kicad-agent/osstempo_led_carrier/current/fabrication/` (gerbers, Excellon
drill, positions CSV, assembly drawings, IPC-2581, IPC-D-356).

## Socket pin mapping — verified

Three ESP32 pins are used, the same three as the breadboard: **VIN, GND, D5**. Positions come
from the DOIT DevKit V1 pinout for the board in hand. Dev board component-side up, USB toward
the `▲ USB` mark: left column top→bottom is H1.1…H1.15, right column is H2.1…H2.15.

| Net | Pin |
|---|---|
| `5V` | H2.1 (VIN) |
| `GND` | H2.2 and H1.2 (both dev-board GNDs) |
| `ESP32_IO5` | H1.8 (D5) |

Full column listing is in the spec. GPIO5 is a strapping pin; the breadboard confirms it boots
with the strip attached.

## Current state: full toolkit gate stack green, release promoted

| Gate | Result |
|---|---|
| ERC | **PASS**, 0 items |
| DRC (toolkit, `--severity-all`, zones refilled) | **PASS**, 0 items |
| DRC (independent `kicad-cli` on the saved release) | **0 violations, 0 unconnected** |
| Schematic intent (WS2812B + injection rule packs) | **PASS** |
| SPICE (ngspice-47) | **PASS**, both testbenches |
| EMC / layout / assets / inventory evidence | **9 / 9 PASS** |
| Fabrication export (pinned KiBot, KiCad 10.0.4 in container) | **PASS** |
| Reproducibility | two identical runs → **immutable release promoted** |

### How the copper is authored

The toolkit's autorouter does not run on macOS, so every track and via is declared in the
manifest's `board.routes` / `board.vias`, with
trace ends anchored to pads (`{ref, pad}`) so a placement nudge or pin-mapping fix moves
the copper with it. That makes the toolkit's *own* validate see a routed board, which is
what lets its DRC, KiBot export and release gate pass rather than being bypassed.

Topology:

- **No copper pours on either layer (v1.1).** v1.0 had a solid GND plane on B.Cu and a GND
  pour on F.Cu, connected solid to the GND pins, and its through-hole joints were miserable to
  hand-solder. Every net is now a trace, every through-hole pin touches at most one trace
  (necked to 0.6 mm at the pad), and the unused socket pins touch nothing. GND runs on B.Cu: a
  cross-tie between the two ESP32 GND pins, a trunk down x = 95 into J1.1, and a bus that
  collects one via per cap ground pad.
- **5V leaves the socket at the USB end** (top-right) and drops down the middle corridor as a
  1.3 mm trace into a bus between the two cap rows, then J1. One rail — no jumper, no injection
  header.
- **Signals are 0.3 mm, minimum length, no vias, 45° corners only**.
  `ESP32_IO5` is 2.6 mm (socket pad → R1). `DIN` takes one diagonal under the ESP32 to J1.

### Things the toolkit needed patching for

See [`../../README.md`](../../README.md) for the `osstempo-patches-v2` branch. The zone-fill
patch mattered for v1.0's ground plane (v1.1 and later have no zones). `board.artwork` is how the
golf ball gets on the board, `pcb_no_reference_label` is why the branded back and the bare front
print no `Cn`/`Rn`/`Hn`, and `tht_hand_solder_heat` is the hand-soldering check in
`layout/osstempo_led_carrier_layout_quality.yaml`. `setup-toolkit.sh` checks the branch out.

### Scoped out, and why

- `reference_plane_coverage` and the v1.0 plane checks are retired: v1.1 has no plane, by
  design (hand soldering). At an 800 kHz strip data rate it bought nothing worth a heat gun.

## Why the board is 50 × 70 mm, not the spec's 60 × 40

The socket pin span is 35.56 mm (14 × 2.54) and pads run along **+Y** from the footprint
origin, with the two rows 26.0 mm apart in X. A 40 mm axis therefore leaves no usable margin
on the pin-row direction. Portrait also buys a ~15 mm exposed strip below the ESP32 body,
which is where the hand-soldered connectors live — otherwise they end up trapped under the
dev board where you cannot reach them with an iron.

## Placement decisions worth keeping

- **J1 is centred on the bottom edge with its opening facing out** — a side-entry connector
  belongs at an edge — 20 mm from either bottom mounting hole, pads 10 mm inboard. Everything
  else stays ≥ 7.9 mm from a hole centre and well clear of the routed edge; two earlier
  revisions put 5V within 0.75 mm of a hole and 0.5 mm of the edge — DRC-legal, and wrong.
- **5V has the empty middle corridor to itself** — on J1 pin 3's own column, entering the
  pad from directly above the way DIN enters pin 2; 8.5 mm from either socket row, never
  pressed against a hand-soldered header pin.
- **R1 sits at (93, 91.78), 5 mm from the H1.8 socket pad** with 3.7 mm of clear board
  between its pad and the header row — still at the source end (5 mm is nothing at 800 kHz),
  but out of the way of the iron when the sockets are hand-soldered.
- **J1 is the same JST XH side-entry connector the QuinLED Dig2go uses**, so the Dig2go's
  XH-to-SM cable plugs straight in — keyed, latched, no pigtail. Identified from a scaled
  photo of the Dig2go (2.57 mm pitch → XH, not PH; housing ≈ 10 mm). Mounted the same way up
  with its opening toward our bottom edge, the Dig2go's 5V-left/GND-right becomes
  **pin 1 = GND (left, ▽), pin 2 = DIN, pin 3 = 5V (right)**. The rule pack asserts that.
- **Nothing sits within 5 mm of a J1 pad.** J1 is hand-soldered and the iron needs room.
  The six caps stand on end in two rows at a 4 mm pitch with the 5V bus between them: 5V
  pads face the bus, GND pads face outward to their own vias, so 5V copper is ≥ 2 mm from
  every ground pad (a flat-lying arrangement had it at 0.47 mm — DRC-legal, review-reject).
  They are numbered C1-C3 left to right on the top row and C4-C6 below; C1 is nearest the connector. The bus tees off the 5V drop at x = 106.5, 3.5 mm clear
  of the connector outline; the nearest copper to any mounting hole is 4.69 mm from its centre.
- **No silkscreen on the mounting holes.** Four labelled holes on a 50 × 70 board is noise.
- **No 5V injection header and no isolating jumper.** An earlier revision had both. Removed as
  unneeded: the strip is USB-powered, full stop.
- **No dedicated test pads.** Every net is on a through-hole pin you can clip a probe onto:
  `5V`/`DIN`/`GND` on J1, and every socket pin on H1/H2.

## Gotchas hit while authoring this (all cost a build cycle)

- **Schematic symbol positions come from `schematic_layout.sections`**, not from each
  component's `at`. Two hard limits follow: at most **two columns** per section, and keep
  everything in **grid row 0** — a 15-pin socket makes row 0 ~230 mm tall, so anything in
  row 1 lands off the bottom of A3.
- **Component-pack ratings use the field name `id`**, not `rating_id`.
- **A component-level `footprint:` override trips `footprint_symbol_mismatch`.** Declare a
  second symbol instead (v1.0 had `Capacitor_0603` beside `Capacitor` for this reason).
- **A ref in `non_physical_refs` needs `in_bom: false` on its symbol too**, or the footprint
  and symbol disagree about BOM exclusion. (Bit the solder jumper an earlier revision had.)
- **SPICE evidence reports `NOT_AVAILABLE` with zero testbenches when ngspice is missing** —
  it does not even enumerate them. `brew install ngspice`.
- **The fabrication export runs KiBot in a Podman container, and demands Podman by name**
  (`shutil.which("podman")`; Docker is not accepted). The pinned image is linux/amd64 only;
  it runs fine under emulation on Apple Silicon, but Podman's platform *warning* lands in
  the captured output and gets parsed as the tool's version. `pcb/bin/podman` is a shim that
  adds `--platform linux/amd64` — the only thing that silences it.
- **macOS's `TMPDIR` is under `/var`, a symlink** the toolkit refuses in its KiBot path.
  `env.sh` resolves it. It must stay outside the project root — a temp dir inside the root
  trips the toolkit's source-mutation guard.
- **`documentation_legends` positions symbols from the grid, and a legend's box is exactly
  `card_width` wide** — any line wider than that is a `silk_overlap`. Keep lines ≤ ~24
  chars at 0.8 mm in a 22 mm card.
- **`pcb_rotation: 90` marches pads in +X**, not −X. Getting this backwards put J1 pin 3 on
  top of mounting hole MH4 and produced 13 cascading DRC errors from one root cause.

## Parts

Both assembled part numbers were confirmed as JLCPCB **Basic** (so no feeder fee):

| Ref | MPN | LCSC | Notes |
|---|---|---|---|
| R1 | UNI-ROYAL `0805W8F4700T5E` | C17710 | 470 Ω 1% 0805 |
| C1–C6 | Samsung `CL21A226MAQNNNE` | C45783 | 22 µF **25 V** X5R 0805, in parallel — ~90 µF effective |
| H1, H2 | Samtec `SSW-115-01-T-S` | — | hand-soldered, unpopulated in the assembly BOM |
| J1 | JST `S3B-XH-A(LF)(SN)` | — | hand-soldered; the Dig2go's XH-to-SM cable fits |
| SILK1, MARK1 | — | — | silkscreen artwork (back: golf ball + script logo, URL, solder arrows, pin labels, `Designed in Austin, TX`; front: `▲ USB`), not parts |

The 25 V rating is deliberate: it limits DC-bias derating on the 5 V rail. Six nominal 22 µF
land around 90 µF effective. No SMD electrolytic is in JLCPCB's Basic library (any would add a
feeder fee), and more of the same 22 µF costs nothing extra — so that is how the breadboard's
~1000 µF was answered. The SPICE testbench holds the rail within tens of mV on a 250 mA step.
