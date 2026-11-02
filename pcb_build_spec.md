# Putting Tempo Trainer - PCB Build Spec (KiCad + JLCPCB)

Carrier / "shield" PCB for the ESP32 putting tempo trainer. A 30-pin ESP32 dev
board drops into two female headers; JLC assembles the SMT passives; you
hand-solder the two sockets and the JST XH LED connector. The Dig2go's XH-to-SM cable plugs in.

## Assembly split

- **JLC assembles (SMT, single side, Basic parts only, two part numbers, no feeder fee):**
  470 ohm resistor and 6x 22 uF ceramic.
- **You hand-solder (THT):** 2x 15-pin female headers and the 3-pin JST XH LED connector.
  No pigtail to solder: the QuinLED Dig2go's XH-to-SM cable plugs straight in.

## Board summary

- **Type:** 2-layer, ordinary FR-4, 1.6 mm, HASL. No controlled impedance, no fine pitch.
- **v1.1 is built for hand soldering: no copper pours on either layer.** v1.0 had a solid GND
  plane on the back and a GND pour on the front, connected solid to the ground pins. Every net is
  now a plain trace (5V and DIN on the front, GND on the back), every through-hole pin touches at
  most one trace, power traces neck to 0.6 mm where they enter a pin, and the 26 unused socket
  pins touch no copper at all. See *Hand-soldering heat check* below.
- **Outline: 50 x 70 mm (portrait).** Not the 60 x 40 this spec originally guessed. The socket
  pin span is 35.56 mm and the rows sit 26.0 mm apart, so a 40 mm axis leaves no usable margin
  along the pin rows. Portrait also buys a ~15 mm exposed strip below the ESP32 body for the
  hand-soldered connectors, which would otherwise be unreachable under the dev board.
- **Power path:** ESP32 USB -> dev board 5V pin -> board `5V` net -> strip. One rail, no
  jumper, no injection header, no level shifter. Common ground.
- **Orientation:** the ESP32's **USB connector hangs off the top edge** (the MH1/MH2 end);
  the LED cable leaves the bottom edge. Socket pin 1 is at the USB end. A `▲ USB` mark on
  the front says which way the dev board goes in.
- **Assembly qty:** order >= 2 (JLC assembly minimum). Single-side SMT.

## Socket pin mapping (verified 2026-09-10)

Only three ESP32 pins are used - **VIN (5V), GND and D5 (GPIO5)** - the same three the
breadboard build uses. Positions were read from the DOIT DevKit V1 pinout for the board in
hand and cross-checked against the breadboard wiring.

**Convention.** Dev board component-side up, USB pointing away from you (toward the board's
`▲ USB` mark). The **left** column top to bottom is **H1.1 … H1.15**; the **right** column top
to bottom is **H2.1 … H2.15**. In that orientation the columns read:

| Pos | H1 (left) | H2 (right) |
|---|---|---|
| 1 | 3V3 | **VIN → `5V`** |
| 2 | **GND → `GND`** | **GND → `GND`** |
| 3 | D15 | D13 |
| 4 | D2 | D12 |
| 5 | D4 | D14 |
| 6 | RX2 | D27 |
| 7 | TX2 | D26 |
| 8 | **D5 → `ESP32_IO5`** | D25 |
| 9 | D18 | D33 |
| 10 | D19 | D32 |
| 11 | D21 | D35 |
| 12 | RX0 | D34 |
| 13 | TX0 | VN |
| 14 | D22 | VP |
| 15 | D23 | EN |

Both GND pins are tied together by a back-side GND trace (free, and a lower-impedance return). Everything else is
left floating. **GPIO5 is a strapping pin** (sampled at boot); driving a WS2812B DIN from it
is fine - DIN is a high-impedance input - and the breadboard confirms it boots with the strip
attached.

## Bill of materials

| Ref | Part | Package | Value | Mount | Notes |
|---|---|---|---|---|---|
| U1 | ESP32 dev board (30-pin) | 2x 15 header | - | socketed | NOT soldered; plugs into H1/H2 |
| H1, H2 | Samtec `SSW-115-01-T-S`, 15-way socket, 2.54 mm | THT | - | you solder | ESP32 socket. 1.0 mm drill / **2.0 mm pad** (v1.2, hand-solder ring). |
| R1 | UNI-ROYAL `0805W8F4700T5E` (**LCSC C17710**) | 0805 | 470 ohm 1% | SMT (JLC) | **Basic.** Data line series R. |
| C1-C6 | Samsung `CL21A226MAQNNNE` (**LCSC C45783**) | 0805 | 22 uF **25 V** X5R | SMT (JLC) | **Basic.** In parallel across 5V-GND at J1, ~90 uF effective. |
| J1 | JST `S3B-XH-A(LF)(SN)`, 3-pin XH 2.5 mm, side entry | THT | - | you solder | LED out. **Same connector as the Dig2go** - its XH-to-SM cable fits. Keyed: 1=GND 2=DIN 3=5V. |
| SILK1 | Back silkscreen: golf ball with script logo, `ossgolf.com · v1.2`, solder arrows + pin labels, `Designed in Austin, TX` | B.SilkS | - | artwork only | Baked to outlines; font-independent. |
| MARK1 | Silkscreen `▲ USB` orientation mark | F.SilkS | - | artwork only | Top edge, between the socket rows. |
| MH1-4 | Mounting hole, M3 | NPTH 3.2 mm | - | - | 4 corners, non-plated. |

### Part selection notes

- **C1-C6 - why ceramic instead of the electrolytic:** SMD aluminium electrolytics are
  frequently **Extended** on JLC, which adds a **~$3 feeder fee** on top of a higher unit
  price. 22 uF 0805 ceramics are staple **Basic** parts at fractions of a cent. Ceramics also
  remove the polarity-reversal risk and the can-height check.
- **Why no separate 100 nF (v1.1).** v1.0 had a 100 nF 0603 "HF" cap as C3. Next to five
  22 uF *ceramics* (not electrolytics) it adds nothing: an 0805 22 uF MLCC has HF impedance as low
  as a 100 nF part. C3 is now a sixth 22 uF, so the board has one capacitor part number.
- **Why six ceramics and no electrolytic.** The breadboard uses a ~1000 uF electrolytic
  (the generic NeoPixel-guide value). No SMD electrolytic is in JLCPCB's Basic library, so
  any would add a ~$3 feeder fee; more of the same 22 uF part costs nothing extra. The caps
  are rated **25 V** to limit DC-bias derating on the 5 V rail: six nominal 22 uF land around
  **90 uF effective**. Checked, not assumed: the `spice/` testbench drives a 250 mA comet-sized
  load step through a pessimistic 0.35 ohm USB path and holds the rail within tens of mV. The
  commercial Dig2go controller for the same strips carries one small electrolytic, not 1000 uF.
  This board is USB-powered only; bright or full-strip use is a v2, not something to push
  through the dev board.
- **The 470 ohm value was checked too:** the same suite measures a **31 ns** 10-90% edge into
  ~30 pF, comfortably inside the 350 ns WS2812B T0H pulse.
- **Verify the "Basic" flag in the cart before ordering** (Basic/Extended status can change).
  All three were confirmed Basic on 2026-09-09.

## Header spacing (the critical dimension)

**Row-to-row spacing is 26.0 mm center-to-center (measured, confirmed).**

- Row-to-row: **26.0 mm** center-to-center
- Within-row pitch: **2.54 mm**, 15 positions per row (row length 14 x 2.54 = 35.56 mm)
- **Both rows:** the stock KiCad `PinSocket_1x15_P2.54mm_Vertical` with its round pads enlarged
  from 1.7 to **2.0 mm on the same 1.0 mm drill** (v1.2, `OSSTempo:PinSocket_1x15_P2.54mm_Vertical_Pad2.0`,
  derived by `pcb/tools/make_socket_footprint.py` - pitch, drill, courtyard and 3D model untouched).
  The annular ring grows from 0.35 to 0.5 mm, 40% more copper for the tip and the solder, with
  0.54 mm still between neighbouring pads (JLC minimum 0.127). No slotted or oval holes.
- **Tolerance absorption comes from assembly, not geometry.** A 0.64 mm square header tail in
  a 1.0 mm round hole has ~0.18 mm of play on the flats (much less on the diagonal), and the
  female sockets add a little lateral compliance where the ESP32's pins seat. That is modest -
  which is exactly why the assembly order matters: **solder H1/H2 with the ESP32 plugged in**
  so both headers self-align to the real pin positions before anything is fixed. This
  technique is the tolerance absorber - do not skip it.
- If a future ESP32 board measures something other than 26.0 mm, that is a footprint respin,
  not a field fix. Re-measure before ordering a second batch.

## Netlist

```
ESP32.5V    -> NET 5V  -> J1.3 (5V), C1-C6
ESP32.GND   -> NET GND -> J1.1 (GND), C1-C6           (B.Cu trace, no plane)
ESP32.GPIO5 -> R1 -> NET DIN -> J1.2
```

Three nets: **5V, GND, DIN(after R1)**. Everything else on the ESP32 headers is unconnected
(leave the pads, route nothing). There is deliberately no external-5V injection path and no
isolating jumper: the strip is powered by the dev board's USB, full stop. If the build ever
needs more strip current than USB provides, that is a v2 with its own 5V input and a diode
or jumper to keep it off the dev board's rail - not a field modification.

## Placement & routing

- Place **J1 (LED out) centred on the bottom edge with its opening facing out**, opposite the
  USB end, so the LED cable and the USB cable leave from opposite ends. It is a side-entry
  connector, so the housing front sits flush with the edge (that is what side-entry is for);
  its pads are 10 mm inboard and 20 mm from either bottom mounting hole.
- **Pin order is fixed by the XH key.** On the Dig2go, with its opening facing away from you,
  5V is on the left and GND on the right. Our connector is the same part mounted the same way
  up, so with *its* opening facing away from you 5V is also on the left - which, viewed with
  the `▲ USB` mark at the top, puts **GND on the left (pin 1, the ▽ mark), DIN in the middle,
  5V on the right (pin 3)**. That is what makes the Dig2go cable plug in with the signals right.
- **Keep every other part >= 5 mm from J1's pads** - it is hand-soldered and the iron needs
  room. The caps stand on end in two rows to the upper right with the 5V bus running
  **between** the rows: every 5V pad faces the bus, every GND pad faces outward to its own
  via, so 5V copper is never within 2 mm of a ground pad. Numbered in reading order:
  C1 C2 C3 left to right on the top row (C1 nearest the connector), C4 C5 C6 below. The bus tees off the 5V drop 3.5 mm clear of the
  connector outline. 5V enters J1 pin 3 straight from above, necked to 0.6 mm; DIN enters pin 2
  from above; GND enters pin 1 from above on the back layer, necked to 0.6 mm.
- Place **R1 hard against the GPIO5 socket pad** - source-end damping is the point of a series
  resistor. In the built layout that puts it *under* the ESP32, which is fine: it is a 0.5 mm-tall
  0805 that JLC places, and the dev board rides ~8 mm up on the sockets.
- **GPIO5** is the DIN driver pin - find the pad labeled D5 / GPIO5 on the header and route it
  through R1 to J1.2.
- **Power traces, not planes (v1.1).** 5V and GND are 1.0-1.3 mm traces that neck to 0.6 mm for
  the last 2-4 mm into each through-hole pin. Under 0.5 A, 0.6 mm of 1 oz outer copper runs a few
  degrees warm at most. **No copper pours on either layer** - a plane connected to a hand-soldered
  pin turns it into a heat sink (v1.0's lesson).
- **As built** (copper authored in the manifest): 5V leaves the VIN socket pad along the pin-1
  row, drops straight down the empty middle corridor as a 1.3 mm trace, and feeds a short bus
  between the two cap rows. DIN is one straight 0.3 mm drop from R1 into J1.2. GND is on the back:
  a cross-tie between the two ESP32 GND pins, a trunk straight down to J1.1, and a bus that
  collects each cap's ground via. No trace or part is near a hole keepout, an edge, or a
  hand-soldered header row.
- **Silkscreen, as built (v1.2).** Front: just the `▲ USB` mark at the top edge (plus the JST
  footprint's own pin-1 triangle). No designators and no `LED OUT`: JLC places R1/C1-C6 from the
  CPL, and the hand-soldered parts are labelled on the back, the side you solder. Back, the side
  you solder:
  `ossgolf.com · v1.2` in the top band (no separate wordmark - the ball carries the brand); a 42 mm
  golf ball centred on the socket area (it runs past the pin columns; the pads punch clean rings
  through it), drawn as a lit sphere (each dimple is the shadow
  inside its cup, fading out toward the highlight at the upper left, with a shadow band and bright
  rim at the lower right) carrying the `OSSTempo` script (SignPainter) and a `1`, Titleist-style;
  arrows at every pin that gets solder (the four signal pins labelled `5V`, `GND`, `GND`, `D5`, four
  unlabelled stability pins, and `5V DIN GND` under J1, nothing else - no legend, the arrows are explained here
  and on the site); and `Designed in Austin, TX` in the bottom band. All of it is baked to
  outlines (`artwork/silk_back_v1_2.json` -> `Silk_Back_v1_2` via
  `pcb/tools/make_logo_footprint.py --spec ... --board <built .kicad_pcb>`, which also clips the
  art to the board outline and keeps it off every pad, via and hole in that board), so it survives
  the font-less fabrication container;
  the generator refuses to run if a font is missing rather than silently falling back to the
  KiCad stroke font, and a script checks every arrow against the pad positions in the board file.
- Keep component keepout ~6 mm around each M3 hole (screw head / standoff clearance).

## Mounting holes

- 4x **M3**, **3.2 mm** finished, **non-plated (NPTH)**, one near each corner, inset 4.5 mm.
- The generator emits `MountingHole:MountingHole_3.2mm` as an `np_thru_hole` pad marked
  `board_only` / `exclude_from_bom` / `exclude_from_pos_files` - verified NPTH, no copper.
- **Hole centres as built** (board spans x 75-125, y 65-135), for matching a 3D-printed bracket:

  | Ref | Centre |
  |---|---|
  | MH1 | (79.5, 69.5) |
  | MH2 | (120.5, 69.5) |
  | MH3 | (79.5, 130.5) |
  | MH4 | (120.5, 130.5) |

  Measured on the v1.1 layout: the nearest copper to any hole centre is **4.69 mm** (the B.Cu
  GND strap beside C6, near MH4) - 1.7 mm clear of an M3 pan head's 3 mm radius - and the
  others are 8.45 mm or more; no trace or via comes within **6.35 mm** of a board edge.

## JLCPCB order notes

### The three files to upload

Generate them from the promoted release - never hand-edit them, or a respin ships a stale BOM:

```
python3 pcb/tools/make_jlc_order.py      # -> pcb/build/jlcpcb_order/
```

| File | Where it goes on jlcpcb.com |
|---|---|
| `osstempo_led_carrier-v1.2-gerbers.zip` | **Add gerber file** (top of the quote page) |
| `osstempo_led_carrier-v1.2-bom.csv` | PCB Assembly step 2, **Add BOM file** |
| `osstempo_led_carrier-v1.2-cpl.csv` | PCB Assembly step 2, **Add CPL file** |

The script verifies every file against the release's `fabrication_manifest.json` hashes before
zipping, and the zip is byte-reproducible. It decides what JLC places from the board itself: a
footprint is an assembly part **iff it carries a `JLCPCB Part #` property**. H1/H2/J1 have none,
so they drop out of both CSVs automatically - that is what "leave them unpopulated" means
mechanically, and it is not a list maintained by hand.

**Do not upload the release's gerbers directly** - JLCPCB cannot parse them. See the gotcha
below; always go through the script.

### Gotcha: the release's gerbers carry an invalid CreationDate

The toolkit's reproducibility pass rewrites every `CreationDate` to the literal string
`<normalized>` so that two runs hash identically - but it does that to the **shipped artifacts**,
not just to the hash input. Every promoted release therefore contains

```
%TF.CreationDate,<normalized>*%          <- in all 9 gerbers
; #@! TF.CreationDate,<normalized>       <- and in the Excellon drill file
```

`<normalized>` is not a date. Per the Gerber spec `.CreationDate` must be an ISO 8601 timestamp,
and this is in the X2 header - the first thing a fab's parser reads. **Symptom: JLCPCB's upload
sticks at ~96%**, which is its server-side parse/render step, not the transfer (the zip is 45 kB;
the bytes are long gone by then).

`make_jlc_order.py` substitutes the schematic title-block date on the way into the zip
(20 placeholders: 2 per gerber + 2 in the drill) and asserts that no geometry line changed. The
date is taken from the design rather than the clock so the zip stays reproducible.

**Fixing the date alone was not enough** (observed 2026-09-17): the upload still stalled at the
same point until the **X2 attribute commands and the `.gbrjob` came out too**. So the default zip
is plain X1 with no job file, which JLCPCB accepts. `--x2` opts back into the attribute-carrying
form if some other fab ever wants it.

The strip applies to **gerbers only**. In the Excellon the same syntax carries

```
; #@! TA.AperFunction,NonPlated,NPTH,ComponentDrill
T4C3.200
```

which is the only thing marking the four M3 holes unplated - strip that and they come back
copper-lined. Both zips carry identical geometry, verified by hashing every file with comments
and attributes excluded.

### Quote page settings

Only two fields need changing from JLCPCB's defaults: **Dimensions** and the **PCB Assembly**
toggle. Everything else the page pre-selects is already right for this board.

| Field | Value | Why |
|---|---|---|
| Base Material / Layers | FR-4 / **2** | default |
| **Dimensions** | **50 x 70 mm** | **change from 100 x 100** - the outline is 50 x 70 |
| PCB Qty | 5 | JLC's PCB minimum |
| Delivery Format | Single PCB | no panel; the board is well over JLC's 10 x 10 mm floor |
| PCB Thickness | 1.6 mm | default, and what the spec calls for |
| Surface Finish | HASL (with lead) | cheapest, and the friendliest to hand-soldering H1/H2/J1 |
| Outer Copper Weight | 1 oz | default; the 5V bus is 1.3 mm wide, so 1 oz is ample |
| Via Covering | Tented | default; keeps the 6 cap ground vias out of the solder |
| **Min via hole size/diameter** | **0.3mm/(0.4/0.45mm)** | default, and the *loosest* tier - the board's vias are 0.3 mm drill / 0.6 mm pad, so no upcharge |
| Board Outline Tolerance | +/-0.2 mm (Regular) | outline only; it does not touch hole-position tolerance, and the 26.0 mm row spacing is absorbed at assembly anyway |
| Everything under High-spec Options | leave as-is | no gold fingers, castellations, edge plating, blind slots, UL mark or HIC |
| **PCB Assembly** | **toggle ON** | **change** - it is off by default |

### PCB Assembly settings

| Field | Value |
|---|---|
| PCBA Type | **Economic** - both parts are Basic 0805, nothing needs the Standard line |
| Assembly Side | **Top Side** |
| PCBA Qty | **2** (JLC's assembly minimum) |
| Tooling holes | Added by JLCPCB |
| Confirm Parts Placement | **Yes** - worth it on a first run; they send a placement render to approve |

**Two part numbers, seven placements** (6x C45783 + 1x C17710), single side.
**Rotation needs no fixup**: every placed part is a symmetric two-terminal chip passive, so
JLC's library orientation cannot disagree with ours the way it does for ICs and diodes.

- **Verify the "Basic" flag in the cart** before paying - Basic/Extended status can change, and
  an Extended part adds a ~$3 feeder fee. Both were confirmed Basic on 2026-09-09.
- Estimated overhead: ~$8 setup + ~$1.5 stencil, **$0 feeder if all parts are Basic**.

## Build steps (after boards arrive)

1. **Before seating the ESP32:** meter H2.1→J1.3 (5V), H2.2→J1.1 (GND), H1.8→R1→J1.2 (DIN),
   and confirm no 5V-GND short. J1.1 is the pad with the ▽ mark. Confirm R1 and C1-C6 are placed (no polarity - all ceramic; v1.2 prints no designators for them,
   they are the seven small parts by J1 and the left socket).
2. **How to make each joint** (standard practice; v1.1/v1.2 have >= 100 C of thermal margin on every pin):
   - Iron at **330-350 C** for leaded solder (370 C lead-free). v1.1 does not need more, and a
     cooler tip keeps the flux active longer.
   - A **2-3 mm chisel tip**, freshly tinned. Put a small blob of solder on the tip first - that
     blob is what carries heat into the joint.
   - Press the tip against **the pad and the pin at the same time**.
   - Feed solder where the tip meets the pad, not onto the tip. It should flow around the pin and
     go shiny in **1-2 seconds**. If a pad will not wet within ~3 s at 350 C, stop: that is a
     board heat-sinking problem (as on v1.0), not a technique problem.
3. Solder H1 and H2 - **only the pins with an arrow on the back** (8 of the 30; the board has no
   legend for the arrows, by choice - this step and the website explain them):
   - the four that carry signals, labelled `5V`, `GND`, `GND`, `D5`;
   - four more for mechanical stability, unlabelled: H1.1, H1.15 and H2.15 (the socket ends not
     already used - H2.1 is `5V`) and H2.8, the middle of the 5V side (H1's middle is `D5`).
   Corners plus a middle pin on each side hold the sockets square.
   The other 22 pins touch no copper and need no solder. With the ESP32 plugged in, **USB toward
   the `▲ USB` mark**, tack one corner arrow pin of each socket and check the ESP32 sits
   flat/level - that is the alignment step. Then pull the ESP32 out and solder the rest of the
   arrowed pins with clear access to each joint.
4. Hand-solder J1 (the JST XH). Plug the **Dig2go's XH-to-SM cable** into it (red = 5V, white =
   DIN, black = GND); the strip's SM connector goes on the other end. Keyed - no polarity to get
   wrong. Sanity check once: with `▲ USB` at the top, the red wire should sit on the right pin.
5. ESP32 in, cable off: confirm 5 V on J1.3 (right pin, `▲ USB` at the top). Then plug in.
6. Power via the ESP32 USB from a **>= 1 A supply** (BLE + ESP-NOW + strip can peak near 500 mA),
   flash the firmware **with the correct board flag** (below), pair over BLE.

## Firmware cross-reference

- **Build with `-DBOARD_DIY_ESP32`.** The sketch defaults to `BOARD_DIG2GO`
  (`tempo_trainer.ino:52`), which drives **`LED_PIN 16`**, not GPIO5. Flashing the default onto
  this board leaves the strip **dark**, and it looks exactly like a solder or level-shifter fault.
  Either pass the flag or uncomment `#define BOARD_DIY_ESP32` in the sketch.
- Data pin under that flag: **GPIO5** (`LED_PIN 5`, `tempo_trainer.ino:61`).
- Strip: WS2812B, 144 LEDs (`NUM_LEDS 144`).
- No LED power relay on this board: `BOARD_DIY_ESP32` defines no `LED_POWER_PIN`, and the strip
  5V is always live. That matches the firmware.
- **No level shifter, by decision.** 3.3 V data into a 5 V WS2812B is below the datasheet's
  V_IH (0.7 x VDD = 3.5 V), but the breadboard build - same ESP32, same strip, GPIO5 through
  470 ohm, no shifter - runs correctly, and this board is a copy of that circuit with a shorter,
  cleaner data path. If a future strip or a longer cable ever shows the classic first-pixel
  misbehaviour, the fix is an inline 74AHCT buffer off-board or a v2 board, not this one.

## Verification status (v1.1)

Full toolkit gate stack green and an **immutable fabrication release promoted** (built with the
`osstempo-patches` toolkit branch that also built v1.0, on KiCad 10.0.8):
ERC 0 items; DRC 0 items at all severities (toolkit, KiBot-in-container, and an independent
`kicad-cli` run on the saved release); the WS2812B output rule pack (six 22 uF at the connector,
series R, pinout) PASS; SPICE PASS; EMC/layout/asset/inventory evidence 9/9; pinned-KiBot
fabrication export PASS; two identical runs reproduced bit-for-bit.
Fabrication files: `pcb/build/kicad-agent/osstempo_led_carrier/current/fabrication/`.

**v1.0 hardware result:** one board was hand-soldered (with a heat gun) and powered up and ran
the strip correctly - the circuit, pin mapping and connector orientation are proven. v1.1 keeps
all of that and changes only the copper. The v1.0 order files are archived in
`pcb/build/v1.0-archive/`.

## Hand-soldering heat check

The toolkit's `tht_hand_solder_heat` check (kicad-agent-toolkit, `evidence/solder_heat.py`;
reviewed PASS by an independent EE/thermal review) simulates a 50 W station at 340 C on every
through-hole pin, using the real copper around it, and reports the lowest tip temperature that
gets the joint to wetting temperature (liquidus + 35 C, 218 C for 63/37) within 3 s.

| Pin | v1.0 needs | v1.1 needs |
|---|---|---|
| J1.1 LED GND | **345 C - fails at 340** (solid into planes, both layers) | 238 C |
| H1.2 / H2.2 ESP32 GND | 330 / 329 C - marginal (solid into planes) | 236 C |
| H2.1 / J1.3 5V | 247 / 246 C | 239 / 237 C |
| Data and the 26 unused socket pins | 232-240 C (model; field: unsolderable without heat gun) | 232-235 C |

Every v1.1 pin has >= 101 C of margin at a 340 C tip. Times are heating-only (the model assumes
solder already bridges tip, pin and pad), so the *required tip temperature* is the robust figure.
**v1.0 field result (fact, from the person who built it):** every pin, signal pins included,
could not be soldered with an iron at 350-420 C. Solder melted on the header pin but the pad never
got hot enough to wet, and joints never went shiny. The pads only wetted after the whole board was
pre-heated with a heat gun. The builder is an experienced solderer, the tip was clean and
tinned, and the same technique works on other boards. This was a heat-sinking failure of the
v1.0 board, not flux or technique.

**The model under-predicts v1.0.** As drawn, the check says only the three GND pins were
heat-sunk and the signal pins needed ~240 C. That contradicts the field result, so the model is
missing a heat path. Matching what was seen takes an extra ~0.1 W/K of heat drain on every pin
(with it, v1.0 signal pins need ~380-390 C and the GND pins are unsolderable). Likely sources,
not yet confirmed: the GND planes on *both* layers surrounded every pad at only 0.25-0.3 mm, so a
large tip resting on the pad also rests on plane copper under thin solder mask and dumps its
heat into the planes; and the plugged-in ESP32 plus socket contacts draining each pin. Neither
plane path exists on v1.1 (no pours at all), and the build steps pull the ESP32 out after
tacking. Until the model is calibrated against this, treat its v1.0 column as optimistic and the
field result as the authority.

The current board runs this check from the newer toolkit line against the built release; it is
not yet declared in `layout/osstempo_led_carrier_layout_quality.yaml` because the
`osstempo-patches` branch that builds the board predates it.

**Nothing is outstanding.** The LED connector is confirmed JST XH (the Dig2go cable is a
3-pin JST XH female to 3-pin JST SM male adapter, 20 AWG red/white/black = 5V/DIN/GND), and the
socket mapping is verified against the dev board's pinout and the working breadboard.

## Design toolchain

The board is authored with the [kicad-agent-toolkit](https://github.com/georgemclaughlin/kicad-agent-toolkit),
a **private** repo checked out at `../kicad-agent-toolkit` (a sibling of this repo). Nothing
from it is vendored into osstempo; `pcb/README.md` documents the boundary, the setup script,
and the `osstempo-patches` toolkit branch (four small fixes: zone fill with autorouting
off, `board.artwork` for the logo, `outline.corner_radius`, and `pcb_no_reference_label` dropping
the toolkit's own assembly designators - v1.2 prints none for H1/H2, R1 and C1-C6).

- Fab profile: **`jlcpcb_2layer_basic`** (ships with the toolkit; matches this order exactly).
- **Autorouting is disabled** - it does not work on macOS. The toolkit re-executes its own source
  under *KiCad's bundled Python*, which is **3.9** on macOS, while its parser uses PEP 604
  syntax (needs 3.10+). Symptom: `board validate` fails with `DSN_EXPORT_FAILED`. All copper
  is therefore authored in the manifest (`board.routes`, `board.vias`, `board.ground_stitching`,
  `board.zones`) and regenerates from YAML.
- `kicad-agent doctor` reports **BLOCKED** on three exact-version pins (`.ci/toolchain.json` wants
  kicad-cli 10.0.0, Python 3.11.15, uv 0.11.26). It does **not** gate `board build` or
  `board validate`; KiCad ERC runs fine on 10.0.6. Ignore it.
- Environment: `source pcb/env.sh` (KiCad's `MacOS/` dir on `PATH`, `KICAD_PYTHON` /
  `KICAD_PYTHONPATH` at the bundled 3.9, symlink-free `TMPDIR`, `KICAD_AGENT_PROJECT_ROOT=pcb/`).
- Fabrication export runs KiBot in the toolkit's pinned container under **Podman** with
  `--platform linux/amd64` (`pcb/bin/podman` shim). The Podman VM idles at 2 CPU cores + 4 GB, so
  **build with `sh pcb/build.sh`** (validate twice, reseed, order files): it starts the VM if
  needed and always stops it on exit, including on failure or Ctrl-C.
- Electrical footprints are stock official KiCad from the 10.0.6 install, copied into
  `pcb/vendor_ecad/kicad_footprints/` as `source_type: official-kicad`:
  `JST_XH_S3B-XH-A_1x03_P2.50mm_Horizontal`, `R_0805_2012Metric`, `C_0805_2012Metric` - and
  the socket, `PinSocket_1x15_P2.54mm_Vertical`, is used through a derived copy with 2.0 mm
  pads (`OSSTempo.pretty`, pack entry `datasheet_land_pattern`, SHA recorded). Mounting holes need no
  footprint - the generator emits its own NPTH pad from `board.mounting_holes`. The logo and
  marks are generated outline-polygon footprints (`OSSTempo.pretty`, `source_type: generated`).
