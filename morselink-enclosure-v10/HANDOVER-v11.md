# MorseLink enclosure: handover after v10 (19 Oct 2026)

**State:** v10 is built and checked: modular, bolt-together, every part
prints with no slicer supports. The owner has not printed it yet. The full
project and report are due **26 Oct 2026**.

**First thing to do in the next chat:** ask the owner for their latest
`.scad` (they edit it themselves) and how the v10 prints went. Work only
from their file.

---

## 1. Project and owner (unchanged)

- Wearable Morse communicator. **LilyGO T-Beam V1.2** (ESP32, SX1262 868 MHz
  LoRa). One button in, two coin vibration motors out. Worn on the **back of
  the LEFT forearm**.
- **Printer:** campus Prusa Mini, and it's **inaccurate**:
  - Holes never come out round. Make them generous and let the narrow spots
    grip.
  - Avoid sliding fits, small hooks and dovetails printed flat.
  - Avoid tall, thin prints.
  - Supports under big areas print badly.
- **Hardware the owner is happy to use:** M3 bolts and nuts, 1.75 mm
  filament as pins, tape and Blu-tack.
- **How the owner works:**
  - Short messages. Floats ideas and asks "what do you think".
  - Give an honest view and recommend one option, then build once they agree.
  - Wants the full set back, checked.
  - Often asks "which parameter do I change"; answer with names and values.
  - Edits angles and fits themselves.
  - Change only what's asked.
  - Plain language.

## 2. Conventions (important, got wrong before)

- File: `cad/morselink_case_flipped.scad` (v10). v9 is kept as
  `cad/morselink_case_flipped_v9_legacy.scad`.
- `arm = "right"` is the owner's build. **Keep it.**
- As worn: **x = 0 is the ELBOW end, x = L the HAND end**. z is up, away from
  the skin.
- With `arm = "right"`:
  - The pod and the USB are on the thumb side.
  - The antenna is on the pinky side.
  - Model code builds the thumb side at +y; `pinky()` mirrors to -y;
    `arm_m()` mirrors everything.
- In OpenSCAD's default view x = 0 is on the left. The owner says "left" for
  the elbow end.
- Board is flipped: component side down on 4 pillars; battery up under the
  lid. Pillar positions confirmed perfect.

## 3. What v10 is (numbers from the file)

**Box:** L 106.5 × W 42.6. Side walls 21.3 tall (v9 21.46), flat top 27.6
(flush on the cell). `pinky_gap` 1.6, `under_clear` 2.5. Driver veroboard
up to 11.9 tall.

**Bars** (`bar_thumb`, `bar_pinky`): separate, bolted.
- Same v9 45° profile (reach 14.3, drop 10.1). Profile length is now one
  parameter, `bar_len_s` 14.25.
- **Band slot is vertical** (straight up and down when the bar prints
  upside down): inner wall `slot_u` 3.9 out from the case side, `slot_w`
  5.25 across. Slot runs x 10.2 to 96.3 = **86.1 mm**, both sides the same.
- Root strip against the case wall (u 0.3..3, up to `bar_top` 7.5): that
  face and the end tabs sit on the bed when printing.
- Each end: a tab over a base lug, with an outer web. A pocket open toward
  the wall and underneath lets the bar slide sideways onto the lugs.
- Bolts: vertical M3x8 at x 4.5 and 102, 3.8 out (`lug_dx`, `lug_u`). Head
  on the bar tab; nut in a hex recess under the base lug (`lug_t` 4,
  flush with the floor). Holes 3.5.
- Thumb bar: everything inside the slot cut away at the USB (as v9).
- Pinky bar: root notched under the antenna clip tab.
- **Print ribs:** printed upside down, the outer bar's edge starts in mid-air
  (it would be an 86 mm bridge). `bar_ribs` adds seven 0.8 mm breakaway ribs
  in the slot (0.25 gap, ~11 mm bridges). They are only in the print STLs;
  snap them off and file the nubs. Set `bar_ribs = false` if using slicer
  supports instead.

**Lid:** flat top 30 wide (`lid_flat_w`) + two 45° flats
(`lid_side_ang`).
- Hinge at the elbow end: axis along y, 3.5 out (`hg_out`), 23.85 up.
  Knuckles r 3.75: lid knuckle in the middle (|y| ≤ 4.3), base knuckles
  4.7–13 each side.
- Pin: M3x30 + nut. Holes 3.8 (horizontal in both prints).
- The elbow-end wall stops at the axis height; the lid's end plate covers it
  above.
- Base knuckle undersides are 45°, meeting the wall at z 15.05, just above
  the motor hole (assert).
- Hand end: a 12 mm wide, 1.2 thick click flap goes over the outside of the
  end wall. A 0.6 bump (`fl_bump`) drops into a groove in the wall.
  - This replaces the "tongue inside": inside, the hinge arc pushes a
    tongue into the wall; outside, it pulls away.
  - Prying the flap's lower edge outward opens it.
- Side lips 2.5 deep (`lid_lip_h`, was 3) so the pod nuts inside clear them.
- Prints upside down, flat top on the bed.

**Pod** (`pod`): separate, prints on its flat back (the face against the
wall).
- `pod_angle` 45, `pod_raise` -4.5. **`pod_x` -1: pod moved 8 mm toward
  the elbow** (x 75.1–95.5, was 83–103.5) so the hand-end bolt lands on the
  straight wall.
- Flat back 0.3 off the thumb wall (`pod_wgap`).
- Two lugs, one at each end, flat against the wall. Horizontal M3x10
  through lug and wall, head outside, nut inside the case. Bolts at x 71.6
  and 99, 15.25 up.
- Holes: 3.5 in the pod (flat in print), 3.8 in the wall.
- Lugs sit above the bar bolt heads (assert).
- Band gap under the pod at the slot: 6.8 mm (assert > 5).
- Pry notches on the hand and elbow faces, over the lid's snap tabs.
  Outer-face notch removed.
- Wire hole now **3.0** (`link_hole_d`, was 3.5), starting 0.6 above the
  cavity floor (`link_up`). At 45° a 3.5 hole reached the cap's collar
  (assert).
- Switch posts, cap and pod lid unchanged.

**Antenna:**
- Connector `ant_x` **15** (was 13, +2 to clear the bar bolt head). Clip
  `clip_x` **78** (was 76).
- The patch's underside is cut at 45° above the pinky bar.
- Element tip now 1.5 short of the hand end.
- Clip geometry unchanged, so `clip_test` is unchanged.

**Unchanged:** USB (centre 55 from the elbow end), motor hole, pillars, cap,
pod lid, clip, clip_test.

**Parts:** base, bar_thumb, bar_pinky, pod, lid, pod_lid, cap, clip,
clip_test.

**Hardware:** 8 × M3 + 8 nuts:
- 4 × M3x8 (bars)
- 2 × M3x10 (pod)
- 1 × M3x30 (hinge)
- 1 × M3x12–16 (clip)

## 4. Checks done on v10

- All asserts pass; no "was overwritten" warnings.
- `intersection()` of every pair among: base, both bars, pod, lid, pod lid,
  cap, clip, board, cell, driver veroboard, antenna and SMA nut ghosts,
  switch, bar/pod/hinge bolts with heads and nuts. All clear (only faces
  touching), except:
  - The clip around the element: its snap fit, by design.
- Lid swept open 1–110° against base, bars, pod, clip, antenna, hinge bolt:
  clear. The only contact is the click bump riding over its groove edge in
  the first 2°, which is intended.
- Overhangs (`tools/over.py`, print orientation):
  - bars, pod, clip, pod lid: 0.
  - Lid: only the hinge hole top.
  - Base: only hole tops and short bridges: the hinge, SMA, clip-tab and
    pod bolt holes, the nut-recess ceilings, the motor and USB openings,
    and the click groove.

## 5. Open points to raise with the owner

1. Pod moved 8 mm toward the elbow (needed for the hand-end bolt). OK?
2. Breakaway ribs under the bars' outer edge: OK, or slicer supports?
3. Click flap outside the hand-end wall instead of a tongue inside: OK?
4. Wire hole 3.0: enough for their two wires?
5. Still unconfirmed:
   - `ant_x` (cable reach);
   - `sma_back`;
   - board clear under the connector;
   - `switch_h` (5.0);
   - `drv_h`.

## 6. How to build and check (same as before)

- **Install:** `apt-get update && apt-get install -y openscad` (2021.01).
- **Export:** `openscad -o x.stl -D 'part="base"' file.scad`.
- **Images:** `xvfb-run -a openscad -o x.png --camera=... file.scad`.
  `part = "assembly"` with CGAL (F6) fails on the coloured union; use preview
  PNGs or export parts.
- **Pitfalls:**
  - Variables are global and the last assignment wins. Read the console
    for "was overwritten" after every edit and never reuse a name: `pl`,
    `pw`, `pd` are pod sizes; pod lugs use `podlug_*`, bar lugs `lug_*`,
    the hinge `hg_*`, the flap `fl_*`.
  - OpenSCAD has no `? :` for modules; use `if / else`.
- **Overlaps:** include the file with `-D 'part="none"'` and intersect parts
  in place; `lid_open(a)` swings the lid. "Current top level object is
  empty" means clear.
- **Printability:** `tools/over.py` and `tools/vol.py` on the exported STLs.
