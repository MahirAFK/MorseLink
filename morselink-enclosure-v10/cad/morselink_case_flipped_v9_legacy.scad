// =====================================================================
//  MorseLink wrist case v9-F  (LilyGO T-Beam V1.2) -- FLIPPED BOARD
//  Battery faces the lid, which is an arch over the cell.
// ---------------------------------------------------------------------
//  Axes, AS WORN with arm = "right" (the owner's build: case on the back
//  of the LEFT forearm):
//    X runs along the forearm: x = 0 is the ELBOW end, x = L the HAND end.
//    Z is up, away from the skin.
//    The pod and the USB are on the thumb side, the antenna on the pinky side.
//  (Notes before v8 called x = 0 the hand end.  That was the wrong way
//  round for this build, which is why v7 moved the USB the wrong way.)
//
//  v9 (18 Oct), what changed from v8:
//    * No bracket.  The SMA bulkhead goes through the pinky wall near the
//      elbow end (short cable run), held by its nut on a thickened patch
//      of wall.  The antenna's stub points straight out, the hinge is bent
//      90 deg and the element runs back along the arm toward the hand.
//    * One separate clip holds the element, bolted (M3) to a small tab on
//      the base.  The clip prints flat, so both of its holes are flat on
//      the bed; the tab's 45 deg underside prints without supports.
//    * Without LoRa: unscrew the antenna and take the clip off.
//  v8: symmetrical wings, USB 4.5 mm toward the elbow from v6, motor hole
//  in the elbow-end wall, pod 7 mm toward the hand and 3 mm lower.
//
//  Parts (pick one with `part`, F6, export STL):
//    "assembly"  everything in place, with ghost board/battery/antenna
//    "base"      case + wings + pod.  Prints upright, standing on the wing
//                tips: supports under the floor and under the pod (they
//                can't reach the pod from the build plate, so use
//                "everywhere" or paint them on)
//    "clip"      antenna clip, flat, no supports
//    "lid"       arched lid, dome up as worn: supports from the build plate
//    "lid_end"   the same lid standing on one end (tall: avoid)
//    "pod_lid"   button pod lid (prints top-down, no supports)
//    "cap"       button plunger (prints upside down, no supports)
//    "clip_test" three antenna clips, 1 notch = tightest .. 3 = loosest
//
//  Assembly order:
//    1. Button: wires from inside the case through the small hole into
//       the pod, soldered to two DIAGONALLY OPPOSITE legs of the switch
//       (legs bent out flat).  Switch between the four posts with a dab
//       of hot glue.  Drop the cap into the pod lid from underneath and
//       snap the pod lid on.
//    2. Motors and their wires out through the big hole in the elbow end.
//    3. Board, driver veroboard (taped to the pinky wall), main lid.
//    4. Antenna (lid off, before the board): SMA bulkhead through the hole
//       in the pinky wall from inside (its hex sits in the pocket), nut on
//       the outside, u.FL plugged into the board.  Screw the antenna on,
//       bend the hinge 90 deg so the element runs toward the hand, bolt the
//       clip to the tab (M3 x 12-16 + nut) and press the element down
//       into it.  Without the antenna the firmware must not transmit on LoRa.
//    5. Band and strap: up through the wing slots, round the bars.
//
//  All sizes in mm.  MEASURE = estimate, check against your parts.
// =====================================================================

part = "assembly";
arm  = "right";       // the owner's build (see Axes above)

/* ---------- T-Beam ---------- */
board_l     = 100.5;  // MEASURE  PCB length
board_w     = 33.0;   // MEASURE  PCB width
board_pcb_z = 19.5;   // battery side: PCB surface to the top of the cell (v6 lid sat 1.5 mm high)
under_clear = 2.5;    // MEASURE  pillar height under the board (v6 3.5)
holder_w    = 21.0;   // MEASURE  width of the 18650 holder
top_clr     = 0.0;    // gap between the cell and the lid: 0 = flush (raise to 0.3 if the lid won't close)
lid_R       = 38;     // inside radius of the lid arch
usb_drop    = 1.4;    // MEASURE  port centre below the PCB's component side

/* ---------- Locating pillars through the T-Beam's mounting holes ---------- */
hole_sx       = 95.1;    // measured: hole centre to centre along the board
hole_sy       = 27.8;    // measured: hole centre to centre across the board
mount_holes   = [for (i = [0, 1], j = [0, 1])
                   [(board_l - hole_sx)/2 + i*hole_sx, (board_w - hole_sy)/2 + j*hole_sy]];
mount_hole_d  = 2.0;
pillar_d      = 4.5;
pin_clr       = 0.5;
pin_above     = 1.5;

/* ---------- Driver veroboard, on edge against the pinky wall (taped in) ---------- */
drv_l       = 30;     // MEASURE
drv_h       = 11;     // MEASURE  height standing up (the console prints the most that fits)
drv_t       = 1.6;
drv_comp    = 5;      // MEASURE  tallest component, facing the battery
drv_x       = 0;      // shift along the arm

/* ---------- Shell ---------- */
wall      = 2.0;
floor_t   = 2.0;
lid_t     = 2.0;
clr       = 1.0;      // gap round the T-Beam (ends and thumb side)
thumb_gap = 3;        // extra channel on the thumb side for the antenna cable
pinky_gap = 1.6;      // board edge to the pinky wall (v6 3.6).  Under drv_t + 0.5
                      // the driver veroboard can't stand beside the board and sits above it
corner_r  = 3;

/* ---------- Snap-on lids ---------- */
lip_h    = 3;
lip_t    = 1.2;
lip_clr  = 0.25;
tab_w    = 8;
tab_len  = 7;
tab_t    = 1.2;
bump     = 0.6;       // hook size: +/-0.2 to make the lids looser/tighter
bump_h   = 2.0;
recess_d = 1.0;
tab_gap  = 1.0;
tabs_thumb = [0.25, 0.68];
tabs_pinky = [0.25, 0.80];

/* ---------- Band wings (part of the base, the same on both sides) ---------- */
wing_angle = 45;      // degrees below horizontal
slot_w     = 5.25;    // band + strap slot
bar_w      = 5;       // bar the band wraps round
wing_t     = 3;       // plate thickness
slot_in    = 4;       // slot starts this far out along the wing's underside
slot_end   = 4;       // solid wing at each end of the slot
root_fil   = 2;       // fillet between the wing and the case side
wing_x0    = 1;       // wings run from here to L - wing_x0

/* ---------- Antenna: SMA bulkhead through the pinky wall, hinge bent 90 deg ---------- */
ant_x      = 13;      // MEASURE  connector position from the elbow end (as near the u.FL as the cable needs)
ant_z      = 12.5;    // connector axis height: keeps its body inside under the lid's lip and above the board
ant_plate_r= 6.75;    // thickened patch of wall round the connector
plate_t    = 2.5;     // wall the bulkhead clamps (keep thin so the antenna still has thread)
sma_hole_d = 7.2;     // generous: horizontal hole printed upright (fit_test's flat one was 6.9)
sma_flat   = 0;       // distance from hole centre to the thread's flat, 0 = round
sma_hex_af = 8.4;     // MEASURE  across-flats of the connector's hex (inside); 0 = no pocket
sma_hex_d  = 1.5;     // depth of that hex pocket
sma_back   = 8;       // MEASURE  connector body inside the case, from the hex face to the cable
ant_d0     = 9.0;     // element diameter at the bend (measured)
ant_d1     = 8.0;     // element diameter at the tip (measured)
ant_nut    = 2;       // MEASURE  panel nut on the outside
ant_reach  = 30;      // measured: antenna base (against the panel nut) to the outer edge of the bend
ant_len    = 90;      // element, hinge to tip

/* ---------- Element clip, bolted to a tab on the base ---------- */
clip_x     = 76;      // where the clip meets the tab, from the elbow end
clip_t     = 4;       // clip thickness (along the arm)
clip_wall  = 2;
ctab_t     = 4;       // clip tab thickness (along the arm)
bolt_d     = 3;       // M3
bolt_clr_flat = 0.5;  // extra on the clip's hole (printed flat)
bolt_clr_base = 0.8;  // extra on the tab's hole (horizontal, printed upright)
bolt_u     = 4.5;     // bolt: out from the pinky wall (keeps the tab under the wall top)
bolt_boss_r= 4;       // material round the bolt
clip_foot  = 9;       // the clip's foot reaches this far below the bolt ...
foot_gap   = 0.3;     // ... and rests against the case wall: stops it tipping down

/* ---------- Motors and their wires ---------- */
tail_end  = "elbow";  // wall with the big hole: "elbow" (x = 0) or "hand" (x = L)
tail_hole = [14, 8];  // width x height, centred on the side-wall height,
                      // between the centre line and the thumb wall

/* ---------- Button pod (part of the base, thumb side) ---------- */
pod_angle   = 25;     // button face tilt from horizontal
pod_raise   = -4.5;   // pod top this far above the side-wall top (v7 -1.5)
pod_x       = 7;      // shift toward the hand (7 = as far as the straight wall goes)
pod_shift   = 1.0;    // pod moved out from the wall
pod_fl      = 1.2;    // pod floor
switch_w    = 6;      // 6x6 tactile switch
switch_h    = 5.0;    // MEASURE  switch bottom to the top of its plunger
sw_post_d   = 1.6;    // four corner posts that locate the switch
sw_post_h   = 1.5;
link_hole_d = 3.5;    // wires from the main box into the pod

/* ---------- Button cap ---------- */
cap_d     = 10;
cap_hole  = cap_d + 0.6;   // from fit_test
cap_proud = 1.5;
stem_d    = 7;
flange_d  = 12.0;     // stop collar inside the pod, wider than the hole
collar_t  = 0.8;
cap_gap   = 0.3;
cap_stem  = 0.3;      // stem under the collar

/* ---------- USB ---------- */
usb_x     = 44;       // measured on the board, battery side down
usb_shift = -4.5;     // negative = toward the elbow (x = 0): the v6 opening was 4.5 mm too far toward the hand
usb_w     = 15;
usb_h     = 11;
extra_cutouts = [];   // [side, pos, z, width, height]; side "hand" = x = L wall, "elbow" = x = 0 wall

$fn = 48;

// =====================================================================
//  Derived: main box
// =====================================================================
drv_above  = pinky_gap < drv_t + 0.5;          // no room beside the board: veroboard sits above it
inner_w    = board_w + clr + thumb_gap + pinky_gap;
W          = inner_w + 2*wall;
y_thumb_in = W/2 - wall;
y_pinky_in = -W/2 + wall;
board_y0   = y_thumb_in - clr - thumb_gap - board_w;
pcb_bot    = floor_t + under_clear;            // component side (down)
pcb_top    = pcb_bot + 1.6;                    // battery side (up)
cell_top   = pcb_top + board_pcb_z;
R_in       = lid_R;
R_out      = lid_R + lid_t;
arc_zc     = cell_top + top_clr - R_in;
H_side     = arc_zc + sqrt(R_out*R_out - (W/2)*(W/2));   // side-wall top
H_top      = arc_zc + R_out;                              // highest point
H          = H_side;
function arc_in(y) = arc_zc + sqrt(R_in*R_in - y*y);
board_yc   = board_y0 + board_w/2;
board_x0   = wall + clr;
board_x1   = board_x0 + board_l;
L          = board_x1 + clr + wall;
drv_xc     = (board_x0 + board_x1)/2 + drv_x;
drv_y      = y_pinky_in + (drv_above ? 0 : 0.5);
drv_z0     = drv_above ? pcb_top + 0.5 : floor_t;
drv_hmax   = H_side - lip_h - 0.3 - drv_z0;   // the lid's lip comes down over it

// ---------- wings.  Profile (u = out from the case side, v = up from the
// case bottom): the plate's underside starts at the case's bottom edge.
wa      = wing_angle; cw = cos(wa); sw = sin(wa); ta = tan(wa);
w_h     = wing_t/cw;                         // plate thickness measured upright
s_end   = slot_in + slot_w + bar_w;          // along the underside, case edge to the tip
w_drop  = s_end*sw;                          // flat foot this far below the case bottom
w_reach = (w_drop + w_h)/ta;                 // outer end of the foot
function wpt(s, t) = [s*cw + t*sw, -s*sw + t*cw];   // s along the underside, t across the plate
wing_x1 = L - wing_x0;

// ---------- antenna.  Bulkhead through the pinky wall at x = ant_x, stub
// straight out, hinge bent 90 deg, element back along the arm toward the hand.
plate_full = plate_t + (sma_hex_af > 0 ? sma_hex_d : 0);   // wall thickness at the connector
boss_t     = max(0, plate_full - wall);      // patch sticks out this far from the wall
ant_out    = boss_t + ant_nut + ant_reach - ant_d0/2;   // element axis, out from the pinky wall
ant_tip    = ant_x + ant_len;                // element tip, from the elbow end
hex_r      = sma_hex_af/2/cos(30);
ant_skin   = ant_out*sw + ant_z*cw - ant_d0/2; // element surface to the skin, taking the skin along the wings' underside
// ---------- clip and its tab
ant_d      = ant_d0 - (ant_d0 - ant_d1)*(clip_x + clip_t/2 - ant_x)/ant_len;   // element diameter at the clip
clip_r_in  = ant_d/2 - 0.2;                  // snap fit on the element
clip_r_out = clip_r_in + clip_wall;
tab_x0     = clip_x - ctab_t;
tab_zw     = w_h + root_fil + 0.3;           // tab's underside meets the wall just above the wing's root
bolt_z     = tab_zw + bolt_u + bolt_boss_r*sqrt(2) + 0.3;   // high enough for a 45 deg underside (no supports)

// ---------- motor hole
tail_yc = (W/2 - wall)/2;
tail_zc = H_side/2;

// =====================================================================
//  Derived: button pod.  Pod-local frame: X = down the face (outward),
//  Y = along the forearm (toward the hand), Z = out of the face.
// =====================================================================
cp = cos(pod_angle); sp = sin(pod_angle);
pin_w   = flange_d + 1;                                      // cavity across the face
pin_l   = flange_d + 2*(lip_clr + tab_t) + 1.5;              // cavity along the arm
cham    = (flange_d - cap_d)/2;                              // 45 deg collar top
pin_h   = switch_h + collar_t + cham + cap_gap + cap_stem;   // cavity depth
pd      = pin_h + pod_fl;
pw      = pin_w + 2*wall;
pl      = pin_l + 2*wall;
pod_xc  = L - wall - pl/2 - 8 + pod_x;
pod_y0  = W/2 - wall + sp*pd + pod_shift;
pod_z0  = H_side + pod_raise;
pod_M   = [[0, -1, 0, pod_xc], [cp, 0, sp, pod_y0], [-sp, 0, cp, pod_z0], [0, 0, 0, 1]];
pod_Minv= [[0, cp, -sp, -(cp*pod_y0 - sp*pod_z0)], [-1, 0, 0, pod_xc],
           [0, sp, cp, -(sp*pod_y0 + cp*pod_z0)], [0, 0, 0, 1]];      // world -> pod-local
function pod_pt(X, Y, Z) = [pod_xc - Y, pod_y0 + X*cp + Z*sp, pod_z0 - X*sp + Z*cp];
function pod_under_z(y) = pod_z0 - sp*(y - pod_y0 + pd*sp)/cp - pd*cp;   // pod's underside at this y
btn_X   = wall + pin_w/2;
sw_top  = -pin_h + switch_h;                                 // pod-local Z of the plunger top
cap_ft  = -cap_gap - sw_top;
cap_top = lid_t + cap_proud - sw_top;
stem_len= cap_ft - cham - collar_t;
// wires: horizontal hole that enters the cavity's inner wall 0.9 mm above its floor
link_z    = pod_z0 + (-pin_h + 0.9)*cp - sp*wall + link_hole_d/2;
link_Ztop = (sp*wall + (link_z - pod_z0) + link_hole_d/2)/cp;    // pod-local Z of the hole's top there
cav_corner= pod_pt(wall, 0, -pin_h);                         // cavity's lowest inner corner
slot_top  = wpt(slot_in, wing_t);                            // inner edge of the band slot, top side
pod_gap   = pod_under_z(W/2 + slot_top[0]) - slot_top[1];    // pod underside to the wing at the slot

usb_xw  = board_x0 + board_l - usb_x + usb_shift;   // board turned end-for-end
usb_z   = pcb_bot - usb_drop;
usb_top = usb_z + usb_h/2;
usb_k0  = usb_xw - usb_w/2 - 1;                     // wing cut away in front of the plug
usb_k1  = usb_xw + usb_w/2 + 1;
cutouts = extra_cutouts;

assert(R_out > W/2 + 2, "lid_R too small for the case width");
assert(drv_comp <= pinky_gap - drv_t - (drv_above ? 0 : 0.5) + (board_w - holder_w)/2 - 0.5,
       "driver components won't fit beside the battery holder");
assert(drv_h <= drv_hmax, str("driver veroboard too tall: ", drv_hmax, " mm fits under the lid's lip"));
assert(wing_angle >= 20 && wing_angle <= 60, "wing_angle outside 20..60");
assert(slot_top[0] > root_fil + 0.5, "band slot runs into the wing's root fillet: raise slot_in");
assert(link_Ztop < -(collar_t + cham + cap_gap) - 0.3, "pod wire hole reaches the cap's collar");
assert(link_z + link_hole_d/2 <= H_side - lip_h - 0.25, "pod wire hole hits the main lid's lip: lower pod_raise");
assert(cav_corner[1] > W/2 - wall + 0.8, "pod cavity breaks into the case");
assert(pod_gap > 5, "pod too low: the band can't get round the thumb bar under it");
assert(pod_xc + pl/2 <= L - corner_r, "pod runs past the straight part of the wall at the hand end: lower pod_x");
assert(usb_k1 < pod_xc - pl/2, "USB opening runs into the pod");
assert(stem_len >= 0, "cap stem negative");
assert(ant_z + hex_r <= H_side - lip_h - 0.3, "SMA connector hits the lid's lip inside: lower ant_z");
assert(ant_z - hex_r >= pcb_top + 0.5, "SMA connector sits on the board: raise ant_z");
assert(ant_x - ant_plate_r >= corner_r, "SMA patch runs into the elbow-end corner: raise ant_x");
assert(ant_tip <= L, "antenna tip runs past the hand end: lower ant_x");
assert(clip_x + clip_t < ant_tip - 3, "clip is past the antenna tip: lower clip_x");
assert(clip_x - ctab_t > ant_x + ant_plate_r + 5, "clip tab too close to the connector");
assert(bolt_z + bolt_boss_r <= H_side - 0.3, "clip tab sticks up past the wall top: the lid won't go on (lower bolt_u)");
assert(min([for (t = tabs_pinky) abs(wall + t*(L - 2*wall) - ant_x)]) > tab_w/2 + 0.5 + ant_plate_r,
       "SMA connector sits on a lid snap pocket: move ant_x");
assert(min([for (t = tabs_pinky) abs(wall + t*(L - 2*wall) - (clip_x - ctab_t/2))]) > tab_w/2 + 0.5 + ctab_t/2,
       "clip tab sits on a lid snap pocket: move clip_x");
assert(tail_yc + tail_hole[0]/2 <= W/2 - corner_r && tail_yc - tail_hole[0]/2 >= 0,
       "motor hole doesn't fit between the centre line and the thumb wall");

echo(str("Main box: ", L, " x ", W, "; ", H_side, " tall at the sides, ", H_top, " at the top of the arch"));
echo(str("Wings: reach ", w_reach, " mm out and ", w_drop, " mm below the case bottom.  Band slots ",
         L - 2*wing_x0 - 2*slot_end, " mm long, both sides"));
echo(str("Overall width ", W + max(w_reach, pod_pt(pw, 0, 0)[1] - W/2) + ant_out + clip_r_out,
         " mm with the antenna, ", W + max(w_reach, pod_pt(pw, 0, 0)[1] - W/2) + w_reach, " mm without"));
echo(str("Antenna: connector ", ant_x, " mm from the elbow end, ", ant_z, " mm up; element ", ant_out,
         " mm out from the pinky wall, about ", ant_skin, " mm from the skin; tip ", L - ant_tip, " mm short of the hand end"));
echo(str("Clip: at x = ", clip_x, " to ", clip_x + clip_t, "; bolt ", bolt_u, " mm out, ", bolt_z, " mm up; holes ",
         bolt_d + bolt_clr_flat, " (clip) and ", bolt_d + bolt_clr_base, " (tab)"));
echo(str("Pod: ", pw, " x ", pl, " x ", pd, " deep; top ", pod_z0, " mm up; sticks out ",
         pod_pt(pw, 0, -pd)[1] - W/2, " mm; ", pod_gap, " mm above the band slot"));
echo(str("Driver veroboard: ", drv_above ? "above the board, from " : "on the floor, from ", drv_z0,
         " mm up; max height ", drv_hmax, " mm"));
echo(str("USB opening: centre ", usb_xw, " mm from the elbow end, ", usb_z, " mm up, top at ", usb_top));
echo(str("Motor hole: ", tail_hole[0], " x ", tail_hole[1], " in the ", tail_end, "-end wall, centre ",
         tail_zc, " mm up, ", tail_yc, " mm from the centre line"));

// =====================================================================
//  Helpers
// =====================================================================
module arm_m() { if (arm == "right") mirror([0, 1, 0]) children(); else children(); }
module pinky() { mirror([0, 1, 0]) children(); }       // build in +y, put on the pinky side

module rr(x0, x1, y0, y1, r) {
  r2 = min(r, (x1-x0)/2 - 0.01, (y1-y0)/2 - 0.01);
  translate([x0 + r2, y0 + r2]) offset(r = r2) square([x1-x0-2*r2, y1-y0-2*r2]);
}
// 2D profile in (u = out from the case side, v = up), extruded along x, placed on the +y side
module along_x(x0, len) { translate([x0, W/2, 0]) rotate([90, 0, 90]) linear_extrude(len) children(); }

module cutouts_3d() {
  for (c = cutouts) {
    side = c[0]; p = c[1]; z = c[2]; w = c[3]; h = c[4];
    if (side == "elbow") translate([-1, p - w/2, z - h/2]) cube([wall + 2, w, h]);
    if (side == "hand")  translate([L - wall - 1, p - w/2, z - h/2]) cube([wall + 2, w, h]);
    if (side == "thumb") translate([p - w/2, W/2 - wall - 1, z - h/2]) cube([w, wall + 2, h]);
    if (side == "pinky") translate([p - w/2, -W/2 - 1, z - h/2]) cube([w, wall + 2, h]);
  }
}

function main_tabs() = concat([for (f = tabs_thumb) [wall + f*(L - 2*wall), 1]],
                              [for (f = tabs_pinky) [wall + f*(L - 2*wall), -1]]);
function pod_tabs()  = [for (s = [-1, 1]) [btn_X, s]];

// Snap lid for an inner rectangle [x0,x1]x[y0,y1]; plate from z = 0 up,
// tabs on the y faces at the given [x, side] positions.
module snap_lid(x0, x1, y0, y1, tabs) {
  lx0 = x0 + lip_clr; lx1 = x1 - lip_clr; ly0 = y0 + lip_clr; ly1 = y1 - lip_clr;
  difference() {
    union() {
      linear_extrude(lid_t) rr(x0 - wall, x1 + wall, y0 - wall, y1 + wall, corner_r);
      translate([0, 0, -lip_h]) linear_extrude(lip_h + 0.01) difference() {
        rr(lx0, lx1, ly0, ly1, 0.5);
        rr(lx0 + lip_t, lx1 - lip_t, ly0 + lip_t, ly1 - lip_t, 0.5);
      }
    }
    for (t = tabs) {
      yy = t[1] > 0 ? ly1 - lip_t - 0.5 : ly0 - 0.5;
      translate([t[0] - tab_w/2 - tab_gap, yy, -lip_h - 1]) cube([tab_w + 2*tab_gap, lip_t + 1, lip_h + 1]);
    }
  }
  snap_tabs_only(tabs, ly0, ly1);
}
module snap_tabs_only(tabs, ly0, ly1) {
  for (t = tabs) {
    s = t[1];
    yo = s > 0 ? ly1 : ly0;
    translate([t[0] - tab_w/2, 0, 0]) {
      translate([0, s > 0 ? yo - tab_t : yo, -tab_len]) cube([tab_w, tab_t, tab_len + 0.5]);
      translate([0, yo, -tab_len + 0.3]) rotate([90, 0, 90]) linear_extrude(tab_w)
        polygon(s > 0 ? [[0, 0], [bump, bump], [bump, bump_h - bump], [0, bump_h]]
                      : [[0, 0], [-bump, bump], [-bump, bump_h - bump], [0, bump_h]]);
    }
  }
}
module snap_pockets(y0, y1, z_top, tabs) {
  for (t = tabs) {
    yw = t[1] > 0 ? y1 - 0.01 : y0 - recess_d + 0.01;
    translate([t[0] - tab_w/2 - 0.5, yw, z_top - tab_len - 0.1]) cube([tab_w + 1, recess_d, bump_h + 0.8]);
  }
}

module sma_hole2d() {
  intersection() {
    circle(d = sma_hole_d);
    if (sma_flat > 0) translate([-sma_hole_d/2, -sma_flat]) square([sma_hole_d, sma_hole_d]);
    else circle(d = sma_hole_d);
  }
}

// =====================================================================
//  Band wings (part of the base)
// =====================================================================
module wing_solid2d() {     // plate from the case's bottom edge, flat foot at the tip, fillet at the root
  polygon([[-1.5, 0], [0, 0], [w_drop/ta, -w_drop], [w_reach, -w_drop],
           [root_fil, w_h - root_fil*ta], [0, w_h + root_fil], [-1.5, w_h + root_fil]]);
}
module wing2d() { offset(r = 0.8) offset(delta = -0.8) wing_solid2d(); }   // round the foot for the band
module wslot2d() {
  polygon([wpt(slot_in, -1), wpt(slot_in + slot_w, -1), wpt(slot_in + slot_w, wing_t + 1), wpt(slot_in, wing_t + 1)]);
}
// wing on the +y side, band slot from x = sx0 to sx1
module wing(sx0, sx1, usb) {
  difference() {
    along_x(wing_x0, wing_x1 - wing_x0) wing2d();
    along_x(sx0, sx1 - sx0) wslot2d();
    if (usb) translate([usb_k0, W/2 - 1.5, usb_z - usb_h/2]) cube([usb_k1 - usb_k0, w_reach + 5, 40]);
  }
}

// =====================================================================
//  Antenna (+y side; base() puts it on the pinky side).  The SMA bulkhead
//  goes through a thickened patch of wall; one clip holds the element,
//  bolted to a tab whose 45 deg underside prints without supports.
// =====================================================================
module sma_boss() {            // patch outside the wall round the connector
  if (boss_t > 0) translate([ant_x, W/2 - 0.5, ant_z]) rotate([-90, 0, 0])
    cylinder(r = ant_plate_r, h = boss_t + 0.5);
}
module sma_cuts() {
  translate([ant_x, W/2 - wall - 1, ant_z]) rotate([-90, 0, 0])
    linear_extrude(plate_full + 2) sma_hole2d();
  // hex pocket on the inside face: the connector's hex sits in it and can't turn (flat on top)
  if (sma_hex_af > 0) translate([ant_x, W/2 - wall - 0.01, ant_z]) rotate([-90, 0, 0])
    linear_extrude(sma_hex_d + 0.01) circle(r = hex_r, $fn = 6);
}
module clip_tab() {            // on the base
  difference() {
    along_x(tab_x0, ctab_t) hull() {
      translate([-1, tab_zw - 1]) square([1, bolt_z + bolt_boss_r - tab_zw + 1]);
      translate([bolt_u, bolt_z]) circle(r = bolt_boss_r);
    }
    translate([tab_x0 - 1, W/2 + bolt_u, bolt_z]) rotate([0, 90, 0])
      cylinder(d = bolt_d + bolt_clr_base, h = ctab_t + 2, $fn = 24);
  }
}
module clip_piece() {          // separate part, bolted to the tab's hand face
  along_x(clip_x, clip_t) difference() {
    union() {
      hull() {
        translate([bolt_u, bolt_z]) circle(r = bolt_boss_r);
        translate([ant_out, ant_z]) circle(r = clip_r_out);
      }
      hull() {                     // foot against the wall: the element's weight presses it in
        translate([bolt_u, bolt_z]) circle(r = bolt_boss_r);
        translate([foot_gap, bolt_z - clip_foot]) square([2.5, clip_foot]);
      }
    }
    translate([ant_out, ant_z]) circle(r = clip_r_in);
    translate([ant_out - 0.38*ant_d, ant_z]) square([0.76*ant_d, clip_r_out + 20]);
    translate([bolt_u, bolt_z]) circle(d = bolt_d + bolt_clr_flat, $fn = 24);
  }
}
// three clips: 1 notch = clip_r_in - 0.25, 2 = clip_r_in, 3 = clip_r_in + 0.25
module clip_test() {
  for (i = [0 : 2]) translate([i*16, 0, 0]) linear_extrude(clip_t) {      // flat, like the real clip
    r = clip_r_in + [-0.25, 0, 0.25][i];
    difference() {
      union() {
        translate([-7, 0]) square([14, 2.5]);
        hull() { translate([-4, 0]) square([8, 2.5]); translate([0, 9]) circle(r = r + clip_wall); }
      }
      translate([0, 9]) circle(r = r);
      translate([-0.38*(2*r + 0.4), 9]) square([0.76*(2*r + 0.4), 20]);
      for (n = [0 : i]) translate([-6 + n*1.8, 1.5]) square([0.9, 2]);
    }
  }
}

// =====================================================================
//  Button pod (part of the base, +y = thumb side)
// =====================================================================
module pod_cup() { multmatrix(pod_M) translate([0, -pl/2, -pd]) linear_extrude(pd) rr(0, pw, 0, pl, corner_r); }
module pod_solid() {        // cup, filled back to the case wall
  hull() {
    pod_cup();
    translate([pod_xc - pl/2, W/2 - 1, pod_under_z(W/2 - 1)]) cube([pl, 0.5, pod_z0 - pod_under_z(W/2 - 1)]);
  }
}
module pod_cuts() {
  multmatrix(pod_M) translate([wall, -pin_l/2, -pin_h]) cube([pin_w, pin_l, pin_h + 20]);    // cavity
  multmatrix(pod_M) snap_pockets(-pin_l/2, pin_l/2, 0, pod_tabs());
  multmatrix(pod_M) translate([pw - wall - 1, -3, -1.2]) cube([wall + 2, 6, 1.3]);         // pod lid pry notch
  translate([pod_xc, W/2 - wall - 1, link_z]) rotate([-90, 0, 0]) cylinder(d = link_hole_d, h = wall + 8);
}
module pod_posts() {        // locate the switch body; its legs come out between them
  q = switch_w/2 + 0.3 + sw_post_d/2;
  multmatrix(pod_M) for (dx = [-1, 1], dy = [-1, 1])
    translate([btn_X + dx*q, dy*q, -pin_h - 0.01]) cylinder(d = sw_post_d, h = sw_post_h + 0.01, $fn = 16);
}

// =====================================================================
//  Base
// =====================================================================
module arch_solid(r, x0, x1) {   // solid under the arch, along x
  translate([x0, 0, arc_zc]) rotate([0, 90, 0]) cylinder(r = r, h = x1 - x0, $fn = 160);
}
module body_outer() {
  intersection() {
    linear_extrude(H_top + 1) rr(0, L, -W/2, W/2, corner_r);
    union() {
      translate([L/2, 0, 0]) cube([L + 2, W + 2, 2*H_side], center = true);   // side walls up to H_side
      intersection() {                                         // end walls follow the arch
        union() { translate([0, -W, 0]) cube([wall, 2*W, H_top + 1]);
                  translate([L - wall, -W, 0]) cube([wall, 2*W, H_top + 1]); }
        arch_solid(R_out, -1, L + 1);
      }
    }
  }
}
module base() {
  difference() {
    union() {
      body_outer();
      wing(wing_x0 + slot_end, wing_x1 - slot_end, true);           // thumb side, cut away at the USB
      pinky() { wing(wing_x0 + slot_end, wing_x1 - slot_end, false); sma_boss(); clip_tab(); }
      pod_solid();
    }
    translate([0, 0, floor_t])
      linear_extrude(H_top + 2) rr(wall, L - wall, -W/2 + wall, W/2 - wall, max(corner_r - wall, 0.5));
    // USB: open-bottomed notch in the thumb wall
    translate([usb_xw - usb_w/2, W/2 - wall - 1, -1]) cube([usb_w, wall + 2, usb_top + 1]);
    pod_cuts();
    pinky() sma_cuts();                    // SMA bulkhead through the pinky wall
    // motors and their wires
    translate([tail_end == "hand" ? L - wall - 1 : -1, 0, 0]) rotate([90, 0, 90]) linear_extrude(wall + 2)
      rr(tail_yc - tail_hole[0]/2, tail_yc + tail_hole[0]/2, tail_zc - tail_hole[1]/2, tail_zc + tail_hole[1]/2, 1);
    snap_pockets(-W/2 + wall, W/2 - wall, H, main_tabs());
    translate([-1, -4, arc_zc + R_in - 1.5]) cube([wall + 2, 8, 10]);   // pry notch under the lid's elbow end
    cutouts_3d();
  }
  mount_pillars();
  pod_posts();
}

function hole_xy(h) = [board_x0 + board_l - h[0], board_y0 + board_w - h[1]];   // end-for-end
module mount_pillars() {        // pillar the board sits on, then a pin up through the hole
  for (h = mount_holes) translate([hole_xy(h)[0], hole_xy(h)[1], floor_t - 0.01]) {
    cylinder(d = pillar_d, h = pcb_bot - floor_t + 0.01);
    cylinder(d = mount_hole_d - pin_clr, h = pcb_top + pin_above - floor_t - 0.6);
    translate([0, 0, pcb_top + pin_above - floor_t - 0.6])
      cylinder(d1 = mount_hole_d - pin_clr, d2 = mount_hole_d - pin_clr - 0.6, h = 0.6);
  }
}

// =====================================================================
//  Lids
// =====================================================================
// Arched lid, in place.  It sits on the side walls between the arch-topped end walls.
module main_lid() {
  lx0 = wall + 0.2; lx1 = L - wall - 0.2;
  yi  = W/2 - wall - lip_clr;               // outer face of the lip / tabs
  difference() {
    union() {
      intersection() {
        difference() { arch_solid(R_out, lx0, lx1); arch_solid(R_in, lx0 - 1, lx1 + 1); }
        translate([lx0, -W/2, H_side]) cube([lx1 - lx0, W, 40]);
      }
      for (s = [-1, 1]) intersection() {    // side lips, up into the shell
        translate([lx0 + 1, s > 0 ? yi - lip_t : -yi, H_side - lip_h]) cube([lx1 - lx0 - 2, lip_t, lip_h + 2]);
        arch_solid(R_out, lx0, lx1);
      }
    }
    for (t = main_tabs()) {                 // free the lip beside each tab
      yy = t[1] > 0 ? yi - lip_t - 0.5 : -yi - 0.5;
      translate([t[0] - tab_w/2 - tab_gap, yy, H_side - lip_h - 1]) cube([tab_w + 2*tab_gap, lip_t + 1, lip_h + 1]);
    }
  }
  translate([0, 0, H_side]) snap_tabs_only(main_tabs(), -yi, yi);
}
// pod lid in pod-local coordinates (tabs on the faces across the forearm)
module pod_lid_local() {
  difference() {
    snap_lid(wall, wall + pin_w, -pin_l/2, pin_l/2, pod_tabs());
    translate([btn_X, 0, -lip_h - 1]) cylinder(d = cap_hole, h = lid_t + lip_h + 2);
    translate([btn_X, 0, -lip_h - 1]) cylinder(d = flange_d + 0.8, h = lip_h + 1);   // lip cut back round the collar
    translate([wall - 1, -(link_hole_d/2 + 1), -lip_h - 1])                            // lip cut back at the wire hole
      cube([lip_clr + lip_t + 1.5, link_hole_d + 2, lip_h + 1 - 0.01]);
  }
}

// =====================================================================
//  Cap (z = 0 touches the switch).  The collar is wider than the lid
//  hole, so the cap can't come out of the top; the switch's spring pushes
//  it back up against the lid.  Prints upside down, flat top on the bed.
// =====================================================================
module cap() {
  cylinder(d = stem_d, h = stem_len + 0.01);
  translate([0, 0, stem_len]) cylinder(d = flange_d, h = collar_t + 0.01);
  translate([0, 0, stem_len + collar_t]) cylinder(d1 = flange_d, d2 = cap_d, h = cham + 0.01);
  translate([0, 0, cap_ft - 0.01]) cylinder(d = cap_d, h = cap_top - cap_ft + 0.01);
}

// =====================================================================
//  Ghost parts
// =====================================================================
module ghosts() {
  color("green", 0.35) translate([0, 0, pcb_bot]) difference() {
    translate([board_x0, board_y0, 0]) cube([board_l, board_w, 1.6]);
    for (h = mount_holes) translate([hole_xy(h)[0], hole_xy(h)[1], -1]) cylinder(d = mount_hole_d, h = 4);
  }
  color("gray", 0.35) translate([board_x0 + 18, board_yc, cell_top - 9.25]) rotate([0, 90, 0]) cylinder(d = 18.5, h = 65);
  color("goldenrod", 0.5) translate([drv_xc - drv_l/2, drv_y, drv_z0]) cube([drv_l, drv_t, drv_h]);
  color("dimgray", 0.6) multmatrix(pod_M) translate([btn_X - switch_w/2, -switch_w/2, -pin_h]) cube([switch_w, switch_w, switch_h]);
  color("black", 0.6) pinky() {
    translate([ant_x, W/2 - wall, ant_z]) rotate([90, 0, 0]) cylinder(r = hex_r, h = sma_back);      // connector inside
    translate([ant_x, W/2 + boss_t, ant_z]) rotate([-90, 0, 0]) cylinder(d = 9, h = ant_nut + ant_reach - ant_d0);
    translate([ant_x, W/2 + ant_out, ant_z]) rotate([0, 90, 0]) cylinder(d1 = ant_d0, d2 = ant_d1, h = ant_len);
  }
}

// =====================================================================
//  Output
// =====================================================================
if (part == "assembly") arm_m() {
  color("slateblue") base();
  color("lightsteelblue", 0.85) main_lid();
  color("lightsteelblue", 0.85) multmatrix(pod_M) pod_lid_local();
  color("orange") multmatrix(pod_M) translate([btn_X, 0, sw_top]) cap();
  color("darkorange") pinky() clip_piece();
  %ghosts();
}
if (part == "base")      translate([0, 0, w_drop]) arm_m() base();                             // upright, on the wing tips
if (part == "clip")      rotate([0, -90, 0]) translate([-clip_x, 0, 0]) arm_m() pinky() clip_piece();   // flat, holes on the bed
if (part == "lid")       translate([0, 0, -(H_side - tab_len)]) arm_m() main_lid();            // dome up, as worn
if (part == "lid_end")   translate([0, 0, -(wall + 0.2)]) rotate([0, -90, 0]) arm_m() main_lid();
if (part == "pod_lid")   translate([0, 0, lid_t]) rotate([180, 0, 0]) arm_m() pod_lid_local();
if (part == "cap")       translate([0, 0, cap_top]) rotate([180, 0, 0]) cap();
if (part == "clip_test") clip_test();
