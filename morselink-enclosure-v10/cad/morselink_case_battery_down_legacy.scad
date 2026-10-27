// =====================================================================
//  MorseLink wrist case v4  (LilyGO T-Beam V1.2)
// ---------------------------------------------------------------------
//  Axes:  X runs along the forearm (x = 0 is the HAND end, +x toward
//         the elbow).  Z is up, away from the skin (case on the back of
//         the forearm, motors underneath).  With arm = "left", +Y is the
//         THUMB side: look along the model from the elbow end toward the
//         hand and the thumb side is on your right, exactly as when you
//         hold your left arm out in front of you, palm down.
//
//  Layout:
//    * Main box: T-Beam lying flat, battery down; motor-driver veroboard
//      standing on edge along the pinky wall.  USB through the thumb wall.
//      Motor wires leave through the elbow-end wall, low down, so they can
//      be slid under the band.
//    * Pinky side: antenna arm, away from the hand that presses the
//      button.  A separate bracket pushes the SMA bulkhead out and down
//      the side of the arm; the element runs toward the elbow and is held
//      by a clip on a second strut.
//    * Thumb side, elbow half: button pod, face angled up and out, with
//      its own small veroboard (switch + pull-up).
//    * Snap-on lids, no screws.
//
//  Parts (pick one with `part`, F6, export STL):
//    "assembly"  everything in place, with ghost board/battery/antenna
//    "base"      main box + rail + button pod (one print, no supports)
//    "lid"       main lid (prints top-down)
//    "pod_lid"   button pod lid (prints top-down)
//    "cap"       button plunger for a 6x6 tactile switch (brim)
//    "bracket"   antenna arm (prints flat)
//    "fit_test"  cap holes, SMA hole and a peg socket to check fits
//
//  All sizes in mm.  MEASURE = estimate, check against your parts.
// =====================================================================

part = "assembly";
arm  = "left";        // "right" mirrors everything for a right-arm build
ant_side = -1;        // -1 = antenna on the pinky side, +1 = thumb side

/* ---------- T-Beam ---------- */
board_l     = 100.5;  // MEASURE  PCB length
board_w     = 33.0;   // MEASURE  PCB width
stack_h     = 24.5;   // MEASURE  holder bottom to tallest part on top, battery fitted
board_pcb_z = 20.5;   // MEASURE  PCB underside above the holder bottom
holder_w    = 21.0;   // MEASURE  width of the 18650 holder under the PCB
headroom    = 2.5;

/* ---------- Locating pillars through the T-Beam's mounting holes ----------
   Each hole: [distance from the board's hand-end edge, distance from its
   thumb-side edge], to the hole centre.  MEASURE: the defaults are a guess
   (one hole 3 mm in from each corner); delete entries for holes you don't have. */
mount_holes   = [[3, 3], [3, 30], [97.5, 3], [97.5, 30]];
mount_hole_d  = 2.0;     // hole diameter (2 mm on the T-Beam)
pillar_d      = 4.5;     // pillar under the board
pin_clr       = 0.3;     // pin = hole minus this
pin_above     = 1.5;     // how far the pin pokes up through the board
lid_hold      = true;    // short posts on the lid press the board down around each pin

/* ---------- Driver veroboard, on edge along the pinky wall ---------- */
drv_l       = 30;     // MEASURE
drv_h       = 18;     // MEASURE  height standing up
drv_t       = 1.6;
drv_comp    = 5;      // MEASURE  tallest component (bend transistors flat)
drv_face_in = true;   // components face the board, under its overhang
drv_x       = 0;      // shift along the arm

/* ---------- Shell ---------- */
wall     = 2.0;
floor_t  = 2.0;
lid_t    = 2.0;
clr      = 0.6;
corner_r = 3;

/* ---------- Snap-on lids ---------- */
lip_h    = 3;
lip_t    = 1.2;
lip_clr  = 0.25;
tab_w    = 8;
tab_len  = 7;
tab_t    = 1.2;
bump     = 0.8;       // hook size: +/-0.2 to make the lids looser/tighter
bump_h   = 2.0;
recess_d = 1.0;
tab_gap  = 1.0;
tabs_main= [0.3, 0.65];

/* ---------- Band slots ---------- */
band_t   = 2.5;       // MEASURE  thickness of the cut sleeve fabric
slot_w   = band_t + 1;
bar_w    = 4;
bar_h    = 4;         // set to flange_t (2.5) to remove the raised ridge on the bar
flange_t = 2.5;
slot_end = 4;

/* ---------- Antenna arm (bracket) ---------- */
arm_angle  = 25;      // degrees below horizontal.  Away from 45 the rail's underside needs print supports.
arm_z0     = 8.5;     // height of the arm's root (bottom of the rail) above the case bottom
arm_len    = 25;      // strut length to the SMA plate (longer than the antenna's 2 cm base)
ant_base   = 22;      // MEASURE  plate's outer face to the element centreline, antenna fitted
ant_d      = 9.5;     // MEASURE  element diameter near the clip
ant_len    = 90;
arm_x      = 10;      // front strut (SMA) position along the arm
clip_frac  = 0.7;     // rear strut position along the element (0 = elbow joint, 1 = tip)
sma_hole_d = 6.6;
sma_flat   = 0;       // distance from hole centre to the thread's flat, 0 = round
plate_t    = 2.5;     // plate the bulkhead clamps (keep thin so the antenna still has thread)
sma_hex_af = 8.2;     // MEASURE  across-flats of the connector's hex (inner side); 0 = no pocket
sma_hex_d  = 1.5;     // depth of that hex pocket
axis_h     = 10;       // SMA axis height above the bracket's base plane
strut_w    = 6;
strut_t    = 5;
spine_w    = 6;
spine_h    = 6;
peg_len    = 6;
peg_w      = 6;
peg_h      = 4;
clip_t     = 6;
coax_hole_d= 4.5;

/* ---------- Motor wires ----------
   tail_exit = "floor": slot through the case back, beside the driver board;
               the wires run out under the band and around the arm.
   tail_exit = "elbow": hole low in the elbow-end wall by the pinky corner.  */
tail_exit   = "floor";
tail_hole_d = 5;
tail_hole_z = 5;      // elbow option: centre height above the case bottom
tail_slot   = [8, 4]; // floor option: slot length (along the arm) x width

/* ---------- Button pod ---------- */
pod_angle   = 25;     // button face tilt from horizontal (below 45 the pod's underside needs supports)
pv_l        = 12;     // MEASURE  button veroboard size (switch + resistor fit on ~5x5 holes)
pv_w        = 12;
pv_standoff = 1.5;    // just enough for the solder joints under the veroboard
switch_h    = 5.0;    // MEASURE  veroboard to top of switch stem
pod_x       = 0;      // shift along the arm (0 = default spot in the elbow half)
link_hole_d = 4;

/* ---------- Button cap ---------- */
cap_d     = 10;
cap_hole  = cap_d + 0.3;   // set from fit_test
cap_proud = 1.5;
stem_d    = 7;
flange_d  = 12.5;     // stop collar inside the pod, wider than the hole
collar_t  = 1.0;     // thickness of the cap's stop collar
cap_gap   = 0.3;

/* ---------- USB (on the long edge of the board) ----------
   usb_x is measured from the board's hand-end edge.  If your board has to
   be turned end-for-end to get the port on the thumb side, use
   board_l - 44 (= 56.5) instead.                                         */
usb_side = "thumb";
usb_x    = 44;
usb_w    = 12;        // big enough for the cable's moulded plug body
usb_h    = 8;
extra_cutouts = [];   // [side, pos, z, width, height], see v2 notes

$fn = 48;

// =====================================================================
//  Derived: main box
// =====================================================================
drv_space  = drv_face_in ? drv_t + 1.0 : drv_t + drv_comp + 1.0;
tab_room   = lip_clr + tab_t + 0.4;
inner_w    = board_w + tab_room + clr + drv_space;
inner_h    = stack_h + headroom;
W          = inner_w + 2*wall;
H          = floor_t + inner_h;
y_thumb_in = W/2 - wall;
y_pinky_in = -W/2 + wall;
board_y0   = y_thumb_in - tab_room - board_w;
board_yc   = board_y0 + board_w/2;
board_x0   = wall + clr;
board_x1   = board_x0 + board_l;
L          = board_x1 + clr + wall;
drv_xc     = (board_x0 + board_x1)/2 + drv_x;
drv_y      = y_pinky_in + (drv_face_in ? 0.5 : drv_comp + 0.5);
fl_ext     = slot_w + bar_w;

// =====================================================================
//  Derived: antenna arm.  Bracket-local frame: X = along the forearm,
//  Y = along the struts (out and down), Z = out of the bracket plane.
// =====================================================================
ca = cos(arm_angle); sa = sin(arm_angle);
plate_back = sma_hex_af > 0 ? sma_hex_d : 0;
arm_y0   = W/2 + peg_len*ca + 1.5;           // root of the struts
rear_x   = arm_x + ant_len*clip_frac;
ant_Y    = arm_len + ant_base;               // element axis, bracket Y
rail_x0  = arm_x - 8;
rail_x1  = rear_x + 7;
module arm_frame() { translate([0, arm_y0, arm_z0]) rotate([-arm_angle, 0, 0]) children(); }
function arm_pt(X, Y, Z) = [X, arm_y0 + Y*ca + Z*sa, arm_z0 - Y*sa + Z*ca];
// height of the rail's top surface where it meets the wall
rail_top_wall = arm_pt(0, (W/2 - arm_y0 - spine_h*sa)/ca, spine_h)[2];
coax_z   = min(rail_top_wall + 1, H - lip_h - coax_hole_d/2 - 0.5);

// =====================================================================
//  Derived: button pod.  Pod-local frame: X = down the face (outward),
//  Y = along the forearm (toward the hand), Z = out of the face.
// =====================================================================
cp = cos(pod_angle); sp = sin(pod_angle);
pin_w   = max(pv_w + 2*clr, flange_d + 1);                 // across the face
pin_l   = max(pv_l + 2*clr, flange_d + 2*(lip_clr + tab_t) + 1.5);
cham    = (flange_d - cap_d)/2;                              // 45 deg collar top
pin_h   = pv_standoff + drv_t + switch_h + collar_t + cham + cap_gap + 0.6;
pod_fl  = 1.6;
pd      = pin_h + pod_fl;
pw      = pin_w + 2*wall;
pl      = pin_l + 2*wall;
pod_xc  = L - wall - pl/2 - 8 + pod_x;
pod_y0  = W/2 - wall + sp*pd;
pod_z0  = H;
pod_M   = [[0, -1, 0, pod_xc], [cp, 0, sp, pod_y0], [-sp, 0, cp, pod_z0], [0, 0, 0, 1]];
function pod_pt(X, Y, Z) = [pod_xc - Y, pod_y0 + X*cp + Z*sp, pod_z0 - X*sp + Z*cp];
btn_X   = wall + pin_w/2;
sw_top  = -pin_h + pv_standoff + drv_t + switch_h;          // pod-local Z
cap_ft  = -cap_gap - sw_top;                               // top of the collar (lid underside - gap)
cap_top = lid_t + cap_proud - sw_top;
stem_len= cap_ft - cham - collar_t;
link_pt = pod_pt(wall, 0, -pin_h);

usb_z   = floor_t + board_pcb_z + 1.6 + 1.6;
cutouts = concat([[usb_side, board_x0 + usb_x, usb_z, usb_w, usb_h]], extra_cutouts);

assert(!drv_face_in || drv_comp <= (board_w - holder_w)/2 - 0.5,
       "driver components won't fit under the PCB overhang: set drv_face_in = false");
assert(!drv_face_in || drv_h <= board_pcb_z - 1, "driver board too tall to sit under the PCB");
assert(stem_len >= 0, "cap stem negative");
assert(ant_side < 0 || rail_x1 < pod_xc - pl/2 - 2, "antenna rail runs into the button pod");
assert(rail_x1 < L - 1, "antenna rail runs past the elbow end: lower clip_frac");
assert(pod_xc + pl/2 <= L, "pod hangs past the elbow end");
assert(link_pt[2] + 3 + link_hole_d/2 < H - lip_h, "pod link hole hits the lid lip");

ant_c = arm_pt(0, ant_Y, axis_h);
assert(arm_z0 >= bar_h + 1, "arm root sits on the band bar: raise arm_z0");
echo(str("Gap between the band bar and the bottom of the antenna rail: ", arm_z0 - bar_h, " mm"));
echo(str("Main box: ", L, " x ", W, " x ", H + lid_t, " (", W + 2*fl_ext, " wide over the band bars)"));
echo(str("Element centre: ", ant_c[1] - W/2, " mm out from the ", ant_side < 0 ? "pinky" : "thumb", " wall, ",
         -ant_c[2], " mm below the case bottom"));
echo(str("Pod sticks out ", pod_pt(pw, 0, 0)[1] - W/2, " mm; button face centre ",
         pod_pt(btn_X, 0, 0)[1] - W/2, " mm out, ", pod_pt(btn_X, 0, 0)[2], " mm up"));

// =====================================================================
//  Helpers
// =====================================================================
module ant_m() { if (ant_side < 0) mirror([0, 1, 0]) children(); else children(); }
module arm_m() { if (arm == "right") mirror([0, 1, 0]) children(); else children(); }

module rr(x0, x1, y0, y1, r) {
  r2 = min(r, (x1-x0)/2 - 0.01, (y1-y0)/2 - 0.01);
  translate([x0 + r2, y0 + r2]) offset(r = r2) square([x1-x0-2*r2, y1-y0-2*r2]);
}

module cutouts_3d() {
  for (c = cutouts) {
    side = c[0]; p = c[1]; z = c[2]; w = c[3]; h = c[4];
    if (side == "hand")  translate([-1, p - w/2, z - h/2]) cube([wall + 2, w, h]);
    if (side == "elbow") translate([L - wall - 1, p - w/2, z - h/2]) cube([wall + 2, w, h]);
    if (side == "thumb") translate([p - w/2, W/2 - wall - 1, z - h/2]) cube([w, wall + 2, h]);
    if (side == "pinky") translate([p - w/2, -W/2 - 1, z - h/2]) cube([w, wall + 2, h]);
  }
}

function main_tabs() = [for (f = tabs_main, s = [-1, 1]) [wall + f*(L - 2*wall), s]];
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
  for (t = tabs) {
    s = t[1];
    yo = s > 0 ? ly1 : ly0;
    translate([t[0] - tab_w/2, 0, 0]) {
      translate([0, s > 0 ? yo - tab_t : yo, -tab_len]) cube([tab_w, tab_t, tab_len + 0.01]);
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
//  Base
// =====================================================================
module flange() {
  difference() {
    union() {
      linear_extrude(flange_t) rr(0, L, -W/2 - fl_ext, W/2 + fl_ext, corner_r);
      for (s = [-1, 1])
        translate([0, s > 0 ? W/2 + slot_w : -W/2 - fl_ext, 0]) linear_extrude(bar_h) rr(0, L, 0, bar_w, 1.5);
    }
    for (s = [-1, 1])
      translate([slot_end, s > 0 ? W/2 : -W/2 - slot_w, -1]) cube([L - 2*slot_end, slot_w, bar_h + 2]);
  }
}

// Wedge rail on the thumb wall that the antenna bracket plugs into.
module rail() {
  arm_frame() translate([rail_x0, -25, -2]) cube([rail_x1 - rail_x0, 25, spine_h + 2]);
}
module rail_sockets() {
  arm_frame() for (x = [arm_x, rear_x])
    translate([x - peg_w/2 - 0.15, -peg_len - 0.5, -0.15]) cube([peg_w + 0.3, peg_len + 0.6, peg_h + 0.3]);
}

module driver_guides() {
  for (e = [-1, 1]) {
    x = drv_xc + e*(drv_l/2 + 1);
    translate([x - 2, y_pinky_in - 0.01, floor_t - 0.01]) difference() {
      cube([4, board_y0 - y_pinky_in - clr + 0.01, drv_h]);
      translate([2 - e*1.5 - 1.5, drv_y - y_pinky_in - 0.15, 1]) cube([3, drv_t + 0.3, drv_h]);
    }
  }
}

module pod_outer() {
  hull() {
    multmatrix(pod_M) translate([0, -pl/2, -pd]) linear_extrude(pd) rr(0, pw, 0, pl, corner_r);
    translate([pod_xc - pl/2, W/2 - wall, pod_pt(0, 0, -pd)[2]]) cube([pl, 0.5, H - pod_pt(0, 0, -pd)[2]]);
  }
}
module pod_cavity() {
  multmatrix(pod_M) translate([wall, -pin_l/2, -pin_h]) cube([pin_w, pin_l, pin_h + 20]);
}
module pod_posts() {
  multmatrix(pod_M)
    for (dx = [-1, 1], dy = [-1, 1])
      translate([btn_X + dx*(pv_w/2 - 1.5), dy*(pv_l/2 - 1.5), -pin_h - 0.01])
        cylinder(d = 3, h = pv_standoff + 0.01);     // corner posts, clear of the switch's solder joints
}

module base() {
  difference() {
    union() {
      linear_extrude(H) rr(0, L, -W/2, W/2, corner_r);
      flange();
      ant_m() intersection() {               // rail, kept within the box height
        rail();
        translate([rail_x0, 0, 0]) cube([rail_x1 - rail_x0, W, H]);
      }
      pod_outer();
    }
    translate([0, 0, floor_t])
      linear_extrude(H) rr(wall, L - wall, -W/2 + wall, W/2 - wall, max(corner_r - wall, 0.5));
    pod_cavity();
    multmatrix(pod_M) snap_pockets(-pin_l/2, pin_l/2, 0, pod_tabs());
    multmatrix(pod_M) translate([pw - wall - 1, -3, -1.2]) cube([wall + 2, 6, 1.3]);   // pod lid pry notch
    ant_m() rail_sockets();
    // coax into the box, just above the rail
    ant_m() translate([arm_x + 9, W/2 - wall - 1, coax_z]) rotate([-90, 0, 0]) cylinder(d = coax_hole_d, h = wall + 8);
    // wires from the pod
    translate([pod_xc, W/2 - wall - 1, link_pt[2] + 3]) rotate([-90, 0, 0]) cylinder(d = link_hole_d, h = 12);
    // motor wires
    if (tail_exit == "elbow")
      translate([L - wall - 1, y_pinky_in + tail_hole_d/2 + 1, tail_hole_z]) rotate([0, 90, 0])
        cylinder(d = tail_hole_d, h = wall + 2);
    else
      translate([drv_xc - drv_l/2 - 6 - tail_slot[0], y_pinky_in + drv_space + 0.5, -1])
        linear_extrude(floor_t + 2) rr(0, tail_slot[0], 0, tail_slot[1], tail_slot[1]/2 - 0.01);
    snap_pockets(-W/2 + wall, W/2 - wall, H, main_tabs());
    translate([-1, -4, H - 1.2]) cube([wall + 2, 8, 1.3]);     // pry notch
    cutouts_3d();
  }
  driver_guides();
  mount_pillars();
  pod_posts();
  for (f = [0.1, 0.56, 0.9])
    translate([board_x0 + f*board_l - 3, y_thumb_in - (tab_room - clr) + 0.01, floor_t + board_pcb_z - 3])
      cube([6, tab_room - clr, 4]);
}

// =====================================================================
//  Lids
// =====================================================================
function hole_xy(h) = [board_x0 + h[0], board_y0 + board_w - h[1]];
pcb_bot = floor_t + board_pcb_z;
pcb_top = pcb_bot + 1.6;

// pillar up to the board's underside, then a pin through the hole
module mount_pillars() {
  for (h = mount_holes) translate([hole_xy(h)[0], hole_xy(h)[1], floor_t - 0.01]) {
    cylinder(d = pillar_d, h = pcb_bot - floor_t - 0.2);
    cylinder(d = mount_hole_d - pin_clr, h = pcb_top + pin_above - floor_t - 0.6);
    translate([0, 0, pcb_top + pin_above - floor_t - 0.6])
      cylinder(d1 = mount_hole_d - pin_clr, d2 = mount_hole_d - pin_clr - 0.6, h = 0.6);
  }
}
// hollow posts under the lid that sit on the board around each pin
module lid_posts() {
  if (lid_hold) for (h = mount_holes) translate([hole_xy(h)[0], hole_xy(h)[1], -(H - pcb_top - 0.2)])
    difference() {
      cylinder(d = pillar_d, h = H - pcb_top - 0.2 + 0.01);
      translate([0, 0, -1]) cylinder(d = mount_hole_d + 0.6, h = pin_above + 1.5);
    }
}

module main_lid() {
  difference() {
    union() {
      snap_lid(wall, L - wall, -W/2 + wall, W/2 - wall, main_tabs());
      lid_posts();
    }
    translate([0, 0, -H]) cutouts_3d();
  }
}
// pod lid in pod-local coordinates (tabs on the faces across the forearm)
module pod_lid_local() {
  difference() {
    snap_lid(wall, wall + pin_w, -pin_l/2, pin_l/2, pod_tabs());
    translate([btn_X, 0, -lip_h - 1]) cylinder(d = cap_hole, h = lid_t + lip_h + 2);
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
//  Antenna bracket (bracket-local frame; prints with Z up)
// =====================================================================
module bracket() {
  r_in  = ant_d/2 - 0.2;
  r_out = ant_d/2 + 2;
  difference() {
    union() {
      translate([rail_x0, 0, 0]) cube([rail_x1 - rail_x0, spine_w, spine_h]);              // spine
      for (x = [arm_x, rear_x])                                                             // pegs
        translate([x - peg_w/2, -peg_len, 0]) cube([peg_w, peg_len + 0.01, peg_h]);
      translate([arm_x - strut_w/2, 0, 0]) cube([strut_w, arm_len, strut_t]);               // front strut
      translate([arm_x - 7, arm_len - plate_t - plate_back, 0])                               // SMA plate
        cube([14, plate_t + plate_back, axis_h + 7]);
      translate([rear_x - strut_w/2, 0, 0]) cube([strut_w, ant_Y, strut_t]);               // rear strut
      translate([rear_x - clip_t/2, ant_Y, axis_h]) rotate([0, 90, 0]) cylinder(r = r_out, h = clip_t);
      translate([rear_x - clip_t/2, ant_Y - r_out, 0]) cube([clip_t, 2*r_out, axis_h]);
    }
    translate([arm_x, arm_len + 1, axis_h]) rotate([90, 0, 0]) linear_extrude(plate_t + plate_back + 2) sma_hole2d();
    // hex pocket on the inner face: the connector's hex (or inner nut) sits in it and can't turn
    if (sma_hex_af > 0)
      translate([arm_x, arm_len - plate_t + 0.01, axis_h]) rotate([90, 0, 0])
        linear_extrude(sma_hex_d + 1) rotate(90) circle(d = sma_hex_af / cos(30), $fn = 6);
    translate([rear_x - clip_t/2 - 1, ant_Y, axis_h]) rotate([0, 90, 0]) cylinder(r = r_in, h = clip_t + 2);
    translate([rear_x - clip_t/2 - 1, ant_Y - ant_d*0.38, axis_h]) cube([clip_t + 2, ant_d*0.76, r_out + 1]);
    // cable-tie slot across the front strut
    translate([arm_x - strut_w/2 - 1, arm_len/2 - 1.5, -1]) cube([strut_w + 2, 3, 1.6 + 1]);
  }
}

module fit_test() {
  difference() {
    cube([75, 22, 6]);
    for (i = [0 : 2]) translate([9 + i*14, 11, -1]) cylinder(d = cap_d + 0.2 + i*0.1, h = 8);
    translate([52, 11, -1]) linear_extrude(8) sma_hole2d();
    translate([62, 8.85, 1]) cube([peg_w + 0.3, 20, peg_h + 0.3]);   // peg socket from the side
  }
}

// =====================================================================
//  Ghost parts
// =====================================================================
module ghosts() {
  color("green", 0.35) translate([board_x0, board_y0, floor_t + board_pcb_z]) difference() {
    cube([board_l, board_w, 1.6]);
    for (h = mount_holes) translate([h[0], board_w - h[1], -1]) cylinder(d = mount_hole_d, h = 4);
  }
  color("gray", 0.35) translate([board_x0 + 18, board_yc, floor_t + 9.25]) rotate([0, 90, 0]) cylinder(d = 18.5, h = 65);
  color("goldenrod", 0.5) translate([drv_xc - drv_l/2, drv_y, floor_t + 1]) cube([drv_l, drv_t, drv_h - 1]);
  color("black", 0.6) ant_m() arm_frame() {
    translate([arm_x, arm_len, axis_h]) rotate([-90, 0, 0]) cylinder(d = 8, h = ant_base);
    translate([arm_x, ant_Y, axis_h]) rotate([0, 90, 0]) cylinder(d = ant_d, h = ant_len);
  }
}

// =====================================================================
//  Output
// =====================================================================
// the bracket for the pinky side (left arm) is the mirror image of the thumb-side one
bracket_mirrored = (ant_side < 0) != (arm == "right");

if (part == "assembly") arm_m() {
  color("slateblue") base();
  color("lightsteelblue", 0.85) translate([0, 0, H]) main_lid();
  color("lightsteelblue", 0.85) multmatrix(pod_M) pod_lid_local();
  color("orange") multmatrix(pod_M) translate([btn_X, 0, sw_top]) cap();
  color("orange") ant_m() arm_frame() bracket();
  %ghosts();
}
if (part == "base")     arm_m() base();
if (part == "lid")      translate([0, 0, lid_t]) rotate([180, 0, 0]) arm_m() main_lid();
if (part == "pod_lid")  translate([0, 0, lid_t]) rotate([180, 0, 0]) arm_m() pod_lid_local();
if (part == "cap")      translate([0, 0, cap_top]) rotate([180, 0, 0]) cap();
if (part == "bracket")  { if (bracket_mirrored) mirror([1, 0, 0]) bracket(); else bracket(); }
if (part == "fit_test") fit_test();
