# MorseLink — Enclosure handover (case v9-F, 18 October 2026; v8, v7 and v6 notes below)

For the next chat. This covers the **wearable build**: the 3D-printed case,
the band, the motors, the button and the antenna mounting. It says where the
build stands, what has been decided and why, how the owner works, how the
OpenSCAD model is organised, and the pitfalls already hit. The firmware has
its own handover (`docs/HANDOVER.md` in the firmware zip); this file doesn't
repeat it.




## v9 (18 Oct) — overrides the antenna parts of v8

The pigtail was too short for the v8 bracket, so the antenna moved:
- SMA bulkhead through the pinky wall at `ant_x` = 13 from the elbow end
  (near the u.FL), axis 12.5 up (connector body inside must clear the lid
  lip above and the board below; asserts check both). Wall thickened to 4 mm
  outside the hole; hex pocket inside; hole 7.2 (generous, printed
  horizontal). Stub straight out, hinge bent 90 deg, element back toward
  the hand at 24 mm out; tip 3.5 mm short of the hand end.
- One separate clip (`part = "clip"`, prints flat so its clip and bolt holes
  lie on the bed) bolted with M3 x 12-16 + nut to a tab on the base at
  x = 72-76. Tab underside is 45 deg, no supports. Holes: 3.5 in the clip
  (flat), 3.8 in the tab (horizontal) — owner's rule: holes generous, the
  printer's narrow spots grip.
- Pitfall hit again: a new `tab_t` overwrote the snap tab's `tab_t`; renamed
  `ctab_t`. Watch the console for "was overwritten".
- Measured since: antenna base (against the panel nut) to the outer edge of
  the bend 30 mm (`ant_reach`), element 9 mm at the bend tapering to 8 mm
  (`ant_d0`, `ant_d1`) -> element axis 29.5 mm out; clip sized for 8.3 mm.
  Clip has a foot resting on the wall (0.3 gap) so it can't tip down more
  than ~2 deg; tab kept under the wall top (bolt_u 4.5) so the lid goes on
  (assert). clip_test now prints flat like the clip.
- Unconfirmed: `ant_x` (cable reach), `sma_back`, and whether the
  board's battery side is clear under the connector near the elbow end.
- Parts: base, clip, lid, lid_end, pod_lid, cap, clip_test.

## v8 (18 Oct) — read this first; it overrides the v7 and v6 notes below

**Ends:** as the owner wears it (arm = "right", case on the back of the LEFT
forearm), **x = 0 is the ELBOW end and x = L the HAND end**; pod and USB on
the thumb side, antenna on the pinky side. All notes before v8 called x = 0
the hand end, which is why v7 moved the USB the wrong way and put the motor
hole and the antenna direction at the wrong ends. In OpenSCAD's default view
x = 0 is on the left (the owner says "left" for that end).

What v8 changed:
- Wings symmetrical again (96.5 mm slot both sides). The owner needs this
  for the strap.
- **Detachable antenna bracket** (relay-only use without it; firmware must
  not transmit on LoRa then). Two vertical dovetail keys on the pinky wall
  (key_w 6, key_d 2.5, flare 0.8, key_clr 0.2 = fit_test slider 2, printed
  the same way up), grooves in the bracket closed at the top so it hangs on
  the keys, a 1.75 mm filament pin through a hole under each key locks it
  (~1 mm play). Bracket prints upright as worn, flat underside, no supports.
  Plate near the hand end (hex pocket on its hand face, connector body
  behind it), clip near the elbow end, antenna pointing to the elbow, tip
  ~18 mm past the elbow end. Coax hole just past the bracket's hand end.
- USB opening at 55 mm from the elbow end (v6 59.5 minus 4.5).
- Motor hole in the x = 0 (elbow) wall, thumb half.
- Pod: pod_x 7 (toward the hand, as far as the straight wall allows),
  pod_raise -4.5: ~7 mm left above the band slot (assert at 5).
- Lid, pod lid and cap unchanged from v7.
- Parts: base, bracket, lid, lid_end, pod_lid, cap, clip_test.

## v7 (17 Oct) — read this first; it overrides the v6 notes below

Owner feedback on v6 prints: 45° wings better than 25°; the wing's dovetail
groove printed unevenly (loose and tight in places); the bracket's element
clip was 1–1.5 mm too loose in radius; pillar positions perfect; USB opening
too far toward the hand and too small; the lid sat ~1.5 mm above the cell.
The printer is inaccurate: avoid sliding fits and small hooks.

What v7 changed (all in `cad/morselink_case_flipped.scad`, header lists it):
- **Wings, antenna mount and button pod are now part of the base.** No
  rails, pins, dovetails or bracket. The base prints upright standing on the
  wings' flat feet, with supports under the floor and under the pod (the pod
  sits over the thumb wing, so use "supports everywhere" or paint them on).
- **Antenna:** points to the elbow, hinge straight. Plate at x = 9..13 on
  the pinky side holds the SMA bulkhead (hex pocket on the hand face); C-clip
  (`ant_d` = 7.0, opening upward) at the elbow end. Both stand on the solid
  ends of the pinky wing, so the pinky band slot is 85.5 mm (thumb 96.5).
  Element axis 20 mm out, 14.5 mm up: ~21 mm from the skin (skin taken along
  the wings' underside); tip ~18.5 mm past the elbow end. `clip_test` prints
  three clips to pick `ant_d`.
- **Pod:** no veroboard; the switch sits on the floor between four posts,
  two wires to diagonal legs (pull-up resistor inside the main box). Cap
  collar 0.8 / flange 12 / stem 0.3, floor 1.2: pod 8.6 deep (v6 12.85).
  `pod_raise` = -1.5 (owner's).
- **Sizes:** pinky side 2 mm narrower (`pinky_gap` 1.6) → the driver
  veroboard no longer fits beside the board and sits above it, taped to the
  pinky wall, max ~11.5 mm tall (`drv_h` set to 11, unconfirmed).
  `under_clear` 2.5 (pillars 1 mm lower: "a bit", number unconfirmed).
  `board_pcb_z` 19.5 + `top_clr` 0 (lid flush on the cell). USB: `usb_shift`
  4.5 toward the elbow, `usb_w` 15, `usb_h` 11. Motor hole 14 × 8 in the
  elbow-end wall (`tail_end`, `tail_hole`), floor slot removed.
- Box 106.5 × 42.6, 21.5 at the sides, 27.6 at the arch; overall width ~87.
- Parts now: base, lid, lid_end, pod_lid, cap, clip_test.

**Read in this order:**
1. This file.
2. `cad/morselink_case_flipped.scad`: the header comment (layout, parts,
   assembly order) and the parameter block at the top.
3. The pictures in `png/` (`view_from_elbow.png` shows the cross-section as
   worn; `joint_section.png` shows how a wing locks onto its rail).

**Earlier chats** (search "MorseLink" or these titles if something is unclear):
- The chat that produced this handover (12–15 Oct): all of the case work.
- "Connecting a vibration motor to ESP": motor drivers, battery life, antenna
  options and range estimates (SMA extension cable, bigger antenna).
- "Adding Lily TTGO TBeam to KiCad schematic": the hardware schematic.
- Firmware: "MorseLink LoRa radio implementation", "Project handover and
  tasks", "Morselink testing suite expansion".

**First things to do in the new chat:**
1. Ask, in one message, what has been printed from v6 and how it went:
   - `wing_test`: does the wing slide on smoothly, does the filament pin go
     in, do the band and strap both fit through the slot?
   - `base`: does the T-Beam drop onto the four pins and sit flat? Is there
     room for the antenna cable on the thumb side, and for the driver
     veroboard on the pinky side?
   - `lid` (printed dome-up with supports): did it print cleanly, and does
     it snap on and off?
2. Ask for their current `.scad` if they've changed anything since v6. They
   edit it themselves (angles, fit values). Always work from their latest
   file, never from an older copy.
3. Don't change anything they haven't asked for.

---

## 1. The project and the owner

- **What:** a wearable Morse communicator. A LilyGO **T-Beam V1.2**
  (ESP32, AXP2101 power chip; this board's radio is an **SX1262, 868 MHz**).
  One button in; two coin vibration motors out (in the firmware a dot is the
  wrist motor and a dash sweeps from wrist to forearm). LoRa first, phone
  relay as a backup. The owner has two boards.
- **Deadline:** full project and report due **26 October 2026**.
- **Printer:** the campus **Prusa Mini** (small bed-slinger). The owner
  considers tall, thin prints risky on it. They export STLs themselves in
  OpenSCAD on their Windows PC (`part = "..."`, F6, F7; see §3.3).
- **Material:** PETG recommended (sweat, and the snap tabs flex). The owner
  hasn't said what they're using; worth asking once.

**How the owner works (this part of the project):**
- Short messages, sometimes with typos. Reports what went wrong after a
  print, often with a photo.
- Floats design ideas and asks "what do you think?". Give an honest view
  with the trade-offs, recommend one option, and wait for the go-ahead
  before big changes. Once agreed, they want the full set: the `.scad` plus
  every STL, checked.
- Often asks "which parameters do I change?" or says "don't make changes,
  just tell me". Answer with parameter names, values and where they are in
  the file.
- Tracks versions: once sent back an older file and asked for *only* the pod
  changes on top of it. Change only what's asked.
- Pragmatic about the prototype (tape or Blu-tack instead of printed guides).
- Prefers plain language and short explanations.

## 2. The wearable as decided

Worn on the **left forearm at the watch position**, case on the back of the
wrist (see §5 about the file's `arm` setting).

| Part | Decision | Why |
|---|---|---|
| Case | T-Beam turned over: component side down on 4 pillars whose pins go through its mounting holes, battery up under an **arched lid**. Main box 106.5 × 44.6 mm; side walls 23.3 mm, arch top 30.1 mm | lower side walls than the battery-down version; pins locate the board, the battery (0.5 mm under the lid) stops it lifting |
| Gaps round the board | 1.0 mm all round (`clr`), plus a 3 mm channel on the thumb side (`thumb_gap`) for the antenna cable | the v5 base was too tight to fit the board |
| Band | elastic sleeve cut open lengthwise into a strip (a cuff wrapped on from the side), passes under the case and up through the wing slots, wrapping the bars. Trim it to **≤ 94 mm wide** (the slot length) | putting the sleeve on over the hand would knock the motors |
| Strap | velcro, 8 cm wide, longer than the wrist, nearer the hand; also goes through the wing slots | owner's choice; slots made 50% wider for band + strap |
| Wings | separate clip-on parts, **40° down** to follow the wrist; slot 5.25 mm, bar 5 mm. Each slides onto a dovetail rail along the bottom of the case side from the elbow end, stops against the closed end of its groove, and is locked by a **1.75 mm filament pin** near the elbow end. They reach 13.6 mm out from each side and 10.9 mm below the case; overall width 71.7 mm | flat bars stuck out past the wrist. Separate wings print flat with no supports; the band's pull (outward and down) is blocked by the rail's flared top, the pin blocks the only free direction (along the case) |
| Motors | two coin ERMs on the **underside of the forearm**, under the strap, 5–6 cm apart along the arm. Wire extensions soldered, staggered joints, **heat shrink (not tape)**, a dab of glue as strain relief at the motor. Wires run round the arm under the band and into the case through a floor slot next to the driver veroboard (`tail_exit = "floor"`) | underside is the most sensitive skin; under 4 cm apart they blur into one |
| Driver veroboard | two NPN drivers. Stands on edge against the **pinky wall**, components facing the battery, **taped / Blu-tacked** (printed guides removed in v6). Must stay ≤ 17 mm tall (lid lip), components above the T-Beam's PCB line. Advised: a flyback diode across each motor and a base resistor on each transistor | owner's choice for the prototype |
| Button | 6×6 tactile switch on a 12×12 mm veroboard in a separate **pod**, thumb side, elbow half, face tilted 25°. Printed **cap** (plunger): stem rests on the switch, a collar under the pod lid stops it falling out. The pod slides down into a dovetail slot in the case wall; the main lid sits over the slot and locks it. Wires through a 3.5 mm hole | the pressing hand reaches the thumb side; separate part prints without supports |
| Antenna | stock 868 MHz whip (~11 cm, hinge at 2 cm, 9 cm element) on a u.FL → SMA **bulkhead** pigtail. Separate **bracket** on the **pinky side**, slides down onto two dovetail keys and clicks in. The 2.5 mm plate is clamped between the connector's fixed hex (sitting in a hex pocket so it can't turn) and its nut; the antenna screws on beyond. The element runs toward the elbow and snaps sideways into a C-clip on the rear strut. Element centre ~54 mm out from the pinky wall, ~2 mm below the case bottom | away from the hand pressing the button and from the board/battery; side mount keeps the case low |
| USB | micro-USB on the board's long side, 44 mm from the hand end (measured battery-down). Flipped, it sits low on the thumb wall: centre 59.5 mm from the hand end, 4.1 mm up. The wall notch is open at the bottom; the thumb rail and wing are cut away in front of it | charging likely off the wrist |
| Lid | snap-on, 4 tabs (`bump` 0.6), arch radius 38. Printed **dome-up with "support on build plate only"** | printing it on end made a 10 cm tower, too risky on the Prusa Mini |

**Rejected or replaced along the way** (don't re-propose without reason):
antenna lying behind the board (detunes it); antenna on top of the case
(height); antenna on the thumb side (the typing hand is right next to it);
the pod in line with the case beyond its elbow end (it belongs beside the
case); screws for lids; a separate tray
for the board ("separate middle", not needed once the guides went); flat
band bars (too wide); lid printed standing on end (risky on the printer).

## 3. The OpenSCAD model

### 3.1 Files
- `cad/morselink_case_flipped.scad` — **current** (v6-F). Built from the
  owner's own file, so it includes their changes (`arm = "right"`, both
  angles 25°, fit-test values).
- `cad/morselink_case_battery_down_legacy.scad` — the battery-down variant.
  Not maintained since 12 Oct (integrated pod and antenna rail, flat bars,
  veroboard guides). The owner went with the flipped design.
- `stl/`: v6 exports, already in print orientation. `png/`: renders.
- `tools/over.py`, `tools/vol.py`: the overhang and volume checks (§6).

### 3.2 Axes and frames
- x along the forearm (x = 0 is the **hand** end), z up from the case bottom.
  With `arm = "left"`, +y is the **thumb** side (view from the elbow end,
  palm down: thumb on the right). `arm = "right"` mirrors everything.
- `ant_side = -1` puts the antenna on the pinky side (`ant_m()` mirrors).
- Local frames: the pod uses `pod_M` / `pod_Minv` (pod-local X down the
  face, Z out of it); the bracket uses `arm_frame()` / `arm_inv()`; the wing
  is a 2D profile in (u = out from the case side, v = up) extruded along x
  by `along_x()`, with `wpt(s, t)` giving points along / across the plate.
- Mirrored parts must print as mirror images: `bracket_mirrored`, and the
  `wing_thumb` / `wing_pinky` outputs handle this already.

### 3.3 Parts, print orientation, export

| `part` | Prints | Supports |
|---|---|---|
| `base` | upright | none (only short bridges) |
| `lid` | dome up, as worn | build plate only (marks end up inside) |
| `lid_end` | standing on its hand end | none, but 10 cm tall: avoid on the Prusa Mini |
| `wing_thumb`, `wing_pinky` | flat on their underside | none |
| `wing_test` | 20 mm of rail + wing (with pin hole and a short slot) | none |
| `pod` | upright like a cup | none |
| `pod_lid` | top face down | none |
| `cap` | upside down | none |
| `bracket` | flat | none |
| `fit_test` | flat (cap holes, SMA hole, a dovetail key and three sliders) | none |

Export: set `part = "...";` at the top, **F6** (render), **F7** (export
STL). F5 is only a preview and can't export. Command line:
`openscad -D 'part="base"' -o flipped_base.stl morselink_case_flipped.scad`
(on Windows: `-D "part=\"base\""`).

### 3.4 Parameters: measured vs estimated

**Measured / set from tests:**
- mounting holes 95.1 × 27.8 mm centre to centre (`hole_sx`, `hole_sy`),
  **assumed centred on the board**;
- `usb_x = 44`; 6×6 switch;
- from `fit_test`: `cap_hole = cap_d + 0.6`, `sma_hole_d = 6.9`, slider 2 →
  `key_clr = 0.2` (vertical dovetails of the pod and bracket).

**Still estimates (`MEASURE` in the file):** `board_l` 100.5, `board_w` 33,
`board_pcb_z` 20.5, **`under_clear` 3.5** (tallest part on the component
side incl. the u.FL plug: see §4), `holder_w` 21, `usb_drop` 1.4, driver
board `drv_l/drv_h/drv_comp`, pod board `pv_l/pv_w`, `switch_h` 5.0,
`ant_d` 9.5, `ant_base` 22, `sma_hex_af` 8.2, and the band + strap
thickness behind `slot_w` (the owner's "50% wider" estimate).

**Owner-tuned:** `arm_angle` 25, `pod_angle` 25, `arm = "right"`. Other
knobs they've asked about: `arm_z0` (antenna arm height), `pod_raise` (pod
height), `wing_angle` (20–45 print without supports), `wing_clr` (wing
slide fit), `lid_R` (arch radius: must keep the driver board under the lid
lip; an assert says so), `bump` (snap tightness).

The console prints the key sizes (box, wings, USB, pod, antenna) and the
asserts stop with a plain message when something no longer fits.

## 4. Where it stands (15 Oct, evening)

**Printed and reported:**
- `fit_test` → the values in §3.4.
- v5 `base`: the board wouldn't go in. Only 0.6 mm clearance, the veroboard
  guides collided with the board, no room on the thumb side for the antenna
  cable, and the bar slots were too narrow for band + strap. The pins were
  also at guessed hole positions 0.6–0.8 mm out.

**v6 (sent 15 Oct) fixes all of that; none of it is printed yet.** Suggested
order: `wing_test` (about 15 min) → `base`, both wings, `bracket`, `lid` →
`pod_lid` (smaller hooks than v5; reprint only if needed). `pod` and `cap`
are unchanged from v5.

## 5. Open items and next steps

1. **Print and test v6.** Adjust `wing_clr` from the wing test before the
   full wings (a 10 cm dovetail binds more easily than a short one).
2. **If the board doesn't sit flat on the pillars:** the component side is
   taller than `under_clear` (3.5 mm). The GPS module and its patch antenna,
   any soldered header pins and the u.FL plug are the likely culprits.
   Measure and raise `under_clear` (the lid and side walls grow with it). If
   the pins miss the holes, check the "holes centred" assumption.
3. **`arm = "right"` in the file:** the owner set it after saying the thumb
   side looked wrong in v3/v4, even though they wear it on the left arm.
   Treat their setting as what they want physically. If in doubt, render
   from the elbow end and ask; don't change it unasked.
4. **Button wiring, unresolved:** the owner wants the pull-up resistor on the
   pod veroboard with only two wires to the main board. That needs three
   (3V3, GND, signal) unless the ESP32's internal pull-up is used instead
   (not available on GPIO 34–39).
5. Lid snap tabs print standing up in the dome-up orientation, so they're
   weaker: check they don't crack (PETG helps; `bump` already reduced).
6. Antenna check suggested, never done: compare `radio ping` signal strength
   with the antenna in free air, mounted, and worn. Keep the element at
   least 15–20 mm from the skin.
7. Comfort test on the wrist: wing angle, band and strap path, the two
   motors felt as two separate points, the case's hand end sitting behind
   the wrist bones so the wrist can bend.
8. The battery-down variant only if the owner asks for it again (it would
   need the v5/v6 changes ported).

## 6. Pitfalls already hit

- **OpenSCAD variables are global; the last assignment wins.** A cap
  parameter named `flange_t` silently overwrote the band flange's
  `flange_t` (2.5 → 1.0 mm) and the bars came out flimsy. OpenSCAD only
  warns: "flange_t was assigned on line N but was overwritten". Check the
  console for that warning after every edit and never reuse a name. (Claude
  first blamed a slot change; the owner found the real cause.)
- OpenSCAD has no `?:` for modules (`c ? a() : b();` is a syntax error):
  use `if / else`.
- The cap's collar used to hit the pod lid's lip, which would jam the
  button; fixed in v5 by cutting the lip back round the collar.
- At 25° the integrated pod and antenna rail needed supports, which is why
  both became separate dovetailed parts (v5).
- Left / right: see §5, item 3.

**Claude's sandbox checks** (do these before handing files over):
- `apt-get install -y openscad`; renders with
  `xvfb-run -a openscad -o out.png --imgsize=1200,800 --camera=... file.scad`.
- Overlaps: `intersection() { A(); B(); }` for every pair of parts
  (OpenSCAD says "Current top level object is empty" when they're clear).
  For sliding parts, also sweep the profile along the whole slide path.
- Printability: export each part in its print orientation and run
  `tools/over.py` (down-facing faces flatter than 45° above the bed) and
  `tools/vol.py` (volume and bounds).
- Cross-sections: `projection(cut = true) multmatrix(...)` of the parts.
- Confirm untouched parts really are unchanged (compare volumes) when the
  owner asks for a limited change.
