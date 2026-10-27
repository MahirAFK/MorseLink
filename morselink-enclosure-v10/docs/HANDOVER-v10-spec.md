# MorseLink enclosure: handover for v10 (19 Oct 2026)

**Task for this chat:** turn the v9 case into **v10**, a modular, bolt-together
design (spec in §4). Deliver the `.scad`, every STL, renders and an updated
handover, all checked (§6). Full project and report due **26 Oct 2026**.

**First thing to do:** ask the owner for the latest v9 zip or `.scad` (they
edit it themselves), plus the two open answers in §5. Work only from their
file.

---

## 1. Project and owner

- Wearable Morse communicator. **LilyGO T-Beam V1.2** (ESP32, SX1262 868 MHz
  LoRa). One button in, two coin vibration motors out. Worn on the **back of
  the LEFT forearm**.
- **Printer:** campus Prusa Mini, and it's **inaccurate**:
  - Holes never come out round. Make them generous and let the narrow spots
    grip.
  - Avoid sliding fits, small hooks and dovetails printed flat. These have
    failed.
  - Avoid tall, thin prints.
  - Supports under big areas (the v9 base floor) printed badly. That's the
    reason for v10.
- **Hardware the owner is happy to use:** M3 bolts and nuts (the clip uses
  them), 1.75 mm filament as pins, tape and Blu-tack.
- **How the owner works:**
  - Short messages. Floats ideas and asks "what do you think".
  - Give an honest view and recommend one option, then build once they agree.
  - Wants the full set back, checked.
  - Often asks "which parameter do I change"; answer with names and values.
  - Edits angles and fits themselves.
  - Change only what's asked.
  - Plain language.

## 2. Conventions (important, got wrong before)

- File: `cad/morselink_case_flipped.scad`.
- `arm = "right"` is the owner's build. **Keep it.**
- As worn: **x = 0 is the ELBOW end, x = L the HAND end**. z is up, away from
  the skin.
- With `arm = "right"`:
  - The pod and the USB are on the thumb side.
  - The antenna is on the pinky side.
  - Model code builds the thumb side at +y; `pinky()` mirrors to -y; `arm_m()`
    mirrors everything.
- In OpenSCAD's default view x = 0 is on the left. The owner says "left" for
  the elbow end.
- Comments before v8 called x = 0 the hand end. That's why v7 put the USB and
  the motor hole at the wrong ends.
- Board is flipped: component side down on 4 pillars whose pins go through
  its mounting holes; battery up under the lid. The pillar positions are
  confirmed perfect.

## 3. Where v9 stands (numbers from the file)

- **Box:** L 106.5 × W 42.6 mm.
  - Side walls 21.46 tall; top 27.6.
  - Lid flush on the cell (`top_clr` 0, `board_pcb_z` 19.5).
  - `under_clear` 2.5 (pillars).
  - `pinky_gap` 1.6 (board edge to the pinky wall).
  - Cell top at z 25.6; PCB from 4.5 to 6.1.
- **Wings:** part of the base, 45°.
  - Slot 5.25, bar 5, plate 3.
  - Reach 14.3, drop 10.1.
  - Slot 96.5 long, the same on both sides (must stay symmetrical: the
    strap needs it).
  - Thumb wing cut away in front of the USB.
- **USB:** opening centre 55 mm from the elbow end, z 3.1, 15 wide × 11 tall,
  open at the bottom.
- **Motor hole:** 14 × 8 in the elbow-end wall (x = 0), thumb half, centred
  on H_side/2. Motors (14 × 8) pass through it.
- **Pod:** part of the base, thumb side, near the hand end (x ≈ 83–103.5).
  - `pod_angle` 25, `pod_raise` -4.5, `pod_x` 7.
  - 17 × 20.4 × 8.6 deep.
  - No veroboard: 6×6 switch on the floor between 4 posts, two wires to
    diagonal legs.
  - Snap-on pod lid with tabs on the pod's hand and elbow faces; one pry
    notch on the outer face.
  - Printed cap (plunger) with collar 0.8, flange 12, stem 0.3.
  - Wire hole into the case.
- **Antenna:**
  - SMA bulkhead through the pinky wall at `ant_x` 13 from the elbow end,
    `ant_z` 12.5.
  - 2 mm thickened patch outside the wall (`sma_boss`, r 6.75); hex pocket
    inside; hole 7.2.
  - Antenna hinge bent 90° so the element runs toward the hand.
  - Owner measured: 30 mm from the antenna base to the outer edge of the
    bend; element 9 mm at the bend, tapering to 8 mm. That puts the element
    29.5 mm out; tip 3.5 mm short of the hand end.
- **Clip (separate, prints flat):** bolted with M3 to a tab on the base at
  x 72–76.
  - Bolt 4.5 mm out from the wall, 17 up.
  - Holes 3.5 in the clip, 3.8 in the tab.
  - The tab's 45° underside needs no supports.
  - A foot resting on the wall (0.3 gap) stops the clip tipping down more
    than ~2°.
  - An assert keeps the tab under the wall top so the lid goes on.
- **Driver veroboard:** against the pinky wall, above the board, max ~11.5 tall.
- **Lid:** arched (`lid_R` 38), 4 snap tabs, printed dome-up with supports.
  `lid_end` is the same lid on end (tall, unused).
- **Parts:** base, clip, lid, lid_end, pod_lid, cap, clip_test (3 flat clips).
- **Unconfirmed:**
  - `ant_x`, which depends on cable reach.
  - `sma_back` (connector length inside).
  - Whether the board is clear under the connector near the elbow end.
  - `switch_h` (5.0).
  - `drv_h`.

## 4. v10 changes (agreed with the owner)

Goal: every part prints flat with as few supports as possible. Bolts instead
of fits. **The base should print upright on its floor with no supports.**

### 4.1 Bars (wings) become separate, bolted parts
- Same profile as v9 (45°, slot 5.25, bar 5, plate 3), identical on both
  sides (`pinky()` mirror). Keep the USB cutaway on the thumb bar.
- The slot gets shorter so each end of the bar has room for an M3 bolt.
  **Keep the slot ≥ 84 mm** (8 cm velcro strap + band). If needed, let the
  bars run a few mm past the case ends.
- **Recommended joint:** vertical bolts, so every hole prints flat.
  - **Base side:** a flat lug at each corner, flush with the floor (z 0 to
    ~3), sticking out from the side wall, with a vertical hole. Add a hex nut
    recess on its underside so nothing sticks out below the floor.
  - **Bar side:** a flat root tab at each end with a vertical hole, resting
    on the base lug. The bar's root block bears against the case side wall
    along its length.
  - **Print the bar upside down:** root tabs flat on the bed, plate rising at
    45°, no supports.
- **Holes:** bolt hole 3 + 0.5 when printed flat. That's the owner's rule:
  generous, the narrow spots grip.
- **Watch:** the SMA patch on the pinky wall (x 6–20, z down to 5.75) sits
  where a bar root would go. Notch the pinky bar's root there, or raise the
  patch.
- **Watch:** the clip's foot (z ~8–17 at x 76–80) and the band path. Keep
  ≥ 5 mm over the slot for band + strap. The v9 assert `pod_gap > 5` shows
  the idea.

### 4.2 Lid: three flat sides plus a hinge
- **Cross-section:** a flat top about **30 mm wide** in the centre, plus two
  angled flats down to the side walls (half-hexagon), replacing the arch.
  - The flat top stays flush on the cell (inner z = cell top 25.6), so the
    peak height doesn't change.
  - The side flats come out at about 45° if `H_side` stays near 21.5.
  - `H_side` can't drop much: the SMA connector inside needs
    `ant_z` + hex radius ≤ `H_side` − lip − 0.3, so `H_side` ≥ ~20.7.
    Driver veroboard height also follows `H_side`.
- The end walls follow the new profile.
- **Lid print:** upside down, flat top on the bed, sides rising at 45°, no
  supports.
- **Hinge** at the **elbow end** (x = 0) on the flat top: knuckles on the lid
  and on the base end wall, and a horizontal M3 bolt across (along y) as the
  pin. Hinge holes are horizontal on the base, so make them generous. Check
  the hinge clears the motor hole below it.
- **Front (hand end):** a simple tongue on the lid that slots into the base
  to hold it shut. It needs a retainer. Recommend a small click bump; fall
  back to a filament pin or a second bolt across. Check the tongue's path on
  the hinge arc.
- **Remove:** the 4 snap tabs, the snap pockets in the base walls, the lid
  pry notch, and `lid_end`.

### 4.3 Pod: separate, bolted, steeper
- Separate part again, bolted to the thumb wall.
  - Read "two bolts on each side" as **one M3 at the pod's hand end and one at
    its elbow end** (confirm, §5).
  - **Recommended:** vertical bolts through flat lugs on the pod into shelf
    tabs on the base wall with 45° undersides (like the clip tab), so both
    holes print flat.
  - Pod prints upright like a cup.
- **Increase `pod_angle`** above 25°. The owner didn't give a number:
  **40°** is a placeholder (confirm).
- Keep:
  - the wire hole lined up between the case and the pod;
  - the switch-on-floor layout and the cap;
  - the band clearance under the pod.
- **Pod lid removal:** remove the existing pry notch (outer face). Add **two**
  notches, one on the pod's **hand-side face** and one on its **elbow-side
  face**. These are the faces with the lid's snap tabs, so prying there
  releases them. Keep them clear of the new bolt lugs.

### 4.4 Unchanged
- Antenna connector and clip.
- USB.
- Motor hole.
- Pillars and board position.
- Driver veroboard space.
- `pinky_gap`.
- Cap.
- `clip_test`.

## 5. Ask the owner first (one message)
1. Pod: one bolt at each end (2 total), or 2 per end (4)? And what angle (40°)?
2. Anything else they changed in the file since v9, and how the v9 prints
   went (clip and its bolt, the connector in the wall, `clip_test`).

## 6. How to build and check (do all before sending)

- **Install and render:** `apt-get install -y openscad` (2021.01).
  - Export a part: `openscad -o x.stl -D 'part="base"' file.scad`.
  - Images: `xvfb-run -a openscad -o x.png --camera=... file.scad`.
- **Pitfall (hit twice):** OpenSCAD variables are global and the last
  assignment wins.
  - v9: a new `tab_t` silently overwrote the snap tab's `tab_t`.
  - Earlier: `flange_t` did the same.
  - After every edit, read the console for "was assigned on line N but was
    overwritten". Never reuse a name.
- **Pitfall:** OpenSCAD has no `? :` for modules; use `if / else`.
- **Overlaps:**
  - `intersection() { A(); B(); }` for every pair, in place: base, bars,
    pod, pod lid, cap, lid, clip, antenna and connector ghosts, board,
    battery, driver veroboard, bolts and nut/head discs.
  - "Current top level object is empty" means clear. Zero-volume results are
    faces touching, which is fine.
  - Sweep moving parts along their path: the lid swinging on its hinge (step
    the angle), the tongue entering, parts sliding on.
- **Printability:** export each part in print orientation. Run
  `tools/over.py` (down-facing faces flatter than 45°, off the bed) and
  `tools/vol.py` (volume, bounds). Target: base, bars, pod, lid, clip all
  with no supports apart from short bridges.
- Keep asserts with plain messages for every clearance (the v9 file has many
  to copy), and echo key sizes.
- **Deliverables:**
  - zip with `cad/` (v10 + legacy), `stl/` (every part in print
    orientation), `png/` (assembly from both sides, elbow-end view, each new
    part in print orientation);
  - updated handover;
  - a short reply listing changes, parameter names, what to reprint, and the
    hardware count (~8–9 M3 bolts + nuts).
