// =====================================================================
//  MorseLink wrist case v10  (LilyGO T-Beam V1.2) -- FLIPPED BOARD
//  Modular: base, two bolted bars, bolted button pod, hinged lid.
// ---------------------------------------------------------------------
//  Axes, AS WORN with arm = "right" (the owner's build: case on the back
//  of the LEFT forearm):
//    X runs along the forearm: x = 0 is the ELBOW end, x = L the HAND end.
//    Z is up, away from the skin.
//    The pod and the USB are on the thumb side, the antenna on the pinky side.
//    (Model code builds the thumb side at +y; pinky() mirrors to -y and
//    arm_m() mirrors everything for arm = "right".)
//
//  v10 (19 Oct), what changed from v9:
//    * Bars (wings) are separate parts, bolted at each end (vertical M3)
//      onto flat lugs on the base, flush with its floor (nut in a hex
//      recess underneath).  Same 45 deg profile.  The band slot is now
//      VERTICAL (straight up and down when the bar prints upside down) and
//      shorter (86 mm).  The base prints upright on its floor, no supports.
//    * Lid: flat top (30 mm) + two 45 deg flats, hinged at the elbow end on
//      an M3 bolt across, a click flap over the hand-end wall holds it
//      shut.  Prints upside down, flat top on the bed, no supports.  The
//      snap tabs, their pockets, the pry notch and lid_end are gone.
//    * Button pod: separate part, pod_angle 45, bolted to the thumb wall
//      with one M3 at each end (horizontal, nut inside the case).  Prints
//      on its flat back.  Moved 8 mm toward the elbow so the hand-end bolt
//      lands on the straight wall.  Pry notches now on its hand and elbow
//      faces.
//    * Antenna connector and clip 2 mm toward the hand (ant_x 15, clip_x
//      78) to clear the bar bolt; patch's underside cut at 45 deg above
//      the pinky bar.
//
//  Parts (pick one with `part`, F6, export STL).  All in print orientation:
//    "assembly"   everything in place, with ghost board/battery/antenna/bolts
//    "base"       upright on its floor, no supports
//    "bar_thumb"  thumb bar (USB cut-away), upside down, no slicer supports:
//                 thin breakaway ribs (bar_ribs) hold up the outer bar's
//                 edge, which starts in mid-air; snap them off after printing
//    "bar_pinky"  pinky bar, the same
//    "pod"        button pod, on its flat back, no supports
//    "lid"        upside down, flat top on the bed, no supports
//    "pod_lid"    button pod lid (top-down, no supports)       -- unchanged
//    "cap"        button plunger (upside down, no supports)    -- unchanged
//    "clip"       antenna clip, flat, no supports               -- unchanged
//    "clip_test"  three antenna clips, 1 notch = tightest .. 3 = loosest
//
//  Hardware: 8 x M3 + 8 nuts.
//    4 x M3x8   bars to base (head on the bar, nut in the lug underneath)
//    2 x M3x10  pod to the thumb wall (head on the pod, nut inside the case)
//    1 x M3x30  lid hinge (pin), nut on the end
//    1 x M3x12-16 antenna clip (as v9)
//
//  Assembly order:
//    1. Button: wires from inside the case through the small hole into
//       the pod, soldered to two DIAGONALLY OPPOSITE legs of the switch
//       (legs bent out flat).  Switch between the four posts with a dab
//       of hot glue.  Cap into the pod lid from underneath, snap the pod
//       lid on.  Bolt the pod to the thumb wall (nuts inside).
//    2. Motors and their wires out through the big hole in the elbow end.
//    3. Antenna (before the board): SMA bulkhead through the pinky wall
//       from inside (hex in the pocket), nut outside, u.FL to the board.
//    4. Board, driver veroboard (taped to the pinky wall).
//    5. Lid: knuckle between the base knuckles, M3x30 across, nut.  Swing
//       it down until the flap clicks over the hand-end wall.
//    6. Bars: nut into each lug's hex recess (from below; a bit of
//       Blu-tack holds it), slide the bar in against the side wall, M3x8
//       down through the bar into the nut.
//    7. Antenna: screw on, bend the hinge 90 deg, bolt the clip, press the
//       element in.  Without the antenna the firmware must not transmit.
//    8. Band and strap: up through the slots, round the bars.
//
//  All sizes in mm.  MEASURE = estimate, check against your parts.
// =====================================================================

part = "pod_lid";
arm  = "right";       // the owner's build (see Axes above)

/* ---------- T-Beam ---------- */
board_l     = 100.5;  // MEASURE  PCB length
board_w     = 33.0;   // MEASURE  PCB width
board_pcb_z = 19.5;   // battery side: PCB surface to the top of the cell
under_clear = 2.5;    // MEASURE  pillar height under the board
holder_w    = 21.0;   // MEASURE  width of the 18650 holder
top_clr     = 0.0;    // gap between the cell and the lid: 0 = flush (raise to 0.3 if the lid won't close)
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
thumb_gap = 3;        // extra channel on the thumb side
pinky_gap = 1.6;      // board edge to the pinky wall
corner_r  = 3;

/* ---------- Lid: flat top + two flats, hinged at the elbow end ---------- */
lid_flat_w   = 30;    // outer width of the flat top (sets the side-wall height)
lid_side_ang = 45;    // side flats, degrees below horizontal
lid_gx       = 0.2;   // gap between the lid and the hand-end wall
lid_lip_h    = 2.5;   // lips down inside the side walls
lid_lip_t    = 1.2;
lid_lip_clr  = 0.25;
hg_r         = 3.75;  // hinge knuckle radius
hg_out       = 3.5;   // hinge axis this far outside the elbow-end wall (bolt head and nut clear the wall)
hg_kl        = 4.3;   // lid knuckle: y from -hg_kl to +hg_kl
hg_ko        = 10.8;    // base knuckles: from hg_kl + hg_gap out to hg_ko, both sides
hg_gap       = 0.4;   // between knuckles
hg_clr       = 0.3;   // clearance round each knuckle
hg_zg        = 0.2;   // lid's end plate above the elbow-end wall
fl_w         = 12;    // click flap over the hand-end wall: width
fl_t         = 1.2;   // flap thickness (thin = springy)
fl_h         = 10;    // flap length down from the lid top
fl_gap       = 0.2;   // flap to the end wall
fl_bump      = 0.6;   // click bump: +0.2 tighter, -0.2 looser
fl_bump_h    = 2.0;
fl_bump_z    = 2;     // bump's bottom above the flap's lower edge
fl_groove_d  = 1.0;   // groove in the end wall that the bump drops into

/* ---------- Pod-lid snap (unchanged from v9) ---------- */
lip_h    = 3;
lip_t    = 1.2;
lip_clr  = 0.25;
tab_w    = 10;
tab_len  = 7;
tab_t    = 1.6;
bump     = 0.5;
bump_h   = 2.0;
recess_d = 1.0;
tab_gap  = 1.0;

/* ---------- Bars (separate, bolted; the same on both sides) ---------- */
wing_angle = 45;      // degrees below horizontal (v9 profile)
wing_t     = 3;       // plate thickness
bar_len_s  = 14.25;   // plate length along its underside, case edge to tip (v9: 4 + 5.25 + 5)
root_fil   = 2;       // fillet at the plate's root
slot_u     = 3.9;     // band slot: inner wall, out from the case side.  The slot is VERTICAL
slot_w     = 5.25;    // band slot width, measured across (horizontal)
bar_gap    = 0.3;     // bar to the case side wall
bar_root_u = 3.0;     // root strip against the wall, out to here
bar_top    = 7.5;     // top of the root strip and end tabs (on the bed when printed)
bar_end    = 1.2;     // solid plate between each end cap and the slot
lug_t      = 4;       // base lug thickness (flush with the floor)
lug_u      = 3.8;     // bar bolt: out from the case side
lug_r      = 4.2;     // lug radius round the bolt
lug_dx     = 4.5;     // bar bolt from each case end
lug_clr    = 0.25;    // bar tab above the lug (the bolt pulls it down)
bar_ribs   = true;    // breakaway ribs under the outer bar in the print STLs (it starts in mid-air when printed upside down)
rib_t      = 0.8;     // rib thickness (along the arm)
rib_g      = 0.25;    // air gap between rib and bar (about one layer)
rib_sp     = 12;      // max spacing: the outer bar bridges between ribs
nut_af     = 5.5;     // M3 nut across flats
nut_h      = 2.4;
nut_clr    = 0.4;
bolt_d     = 3;       // M3
bolt_clr_flat = 0.5;  // extra on holes printed flat (vertical in print)
bolt_clr_base = 0.8;  // extra on holes printed sideways (horizontal in print)

/* ---------- Antenna: SMA bulkhead through the pinky wall, hinge bent 90 deg ---------- */
ant_x      = 15;      // connector from the elbow end (v9 13: +2 to clear the bar bolt)
ant_z      = 12.5;    // connector axis height
ant_plate_r= 6.75;    // thickened patch of wall round the connector
plate_t    = 2.5;
sma_hole_d = 7.2;
sma_flat   = 0;
sma_hex_af = 8.4;     // MEASURE
sma_hex_d  = 1.5;
sma_back   = 8;       // MEASURE
ant_d0     = 9.0;     // element diameter at the bend (measured)
ant_d1     = 8.0;     // element diameter at the tip (measured)
ant_nut    = 2;       // MEASURE  panel nut on the outside
ant_reach  = 30;      // measured
ant_len    = 90;      // element, hinge to tip

/* ---------- Element clip, bolted to a tab on the base (unchanged, moved with the antenna) ---------- */
clip_x     = 78;      // v9 76: +2 with the connector
clip_t     = 4;
clip_wall  = 2;
ctab_t     = 4;
bolt_u     = 4.5;
bolt_boss_r= 4;
clip_foot  = 9;
foot_gap   = 0.3;

/* ---------- Motors and their wires ---------- */
tail_end  = "elbow";
tail_hole = [14, 8];

/* ---------- Button pod (separate, bolted to the thumb wall) ---------- */
pod_angle   = 45;     // button face tilt from horizontal (v9 25)
pod_raise   = -4.5;   // pod top this far above the side-wall top
pod_x       = -1;     // shift toward the hand (v9 7; -1 puts the hand-end bolt on the straight wall)
pod_wgap    = 0.3;    // pod's flat back to the thumb wall
pod_fl      = 1.2;
switch_w    = 6;
switch_h    = 5.0;    // MEASURE
sw_post_d   = 1.6;
sw_post_h   = 1.5;
link_hole_d = 3.0;    // wires into the pod (v9 3.5: at 45 deg a bigger hole reaches the cap's collar)
link_up     = 0.6;    // wire hole's bottom above the pod's cavity floor (v9 0.9)
podlug_t    = 3;      // pod lugs: flat against the wall
podlug_r    = 3.75;   // lug radius round the bolt
podlug_z0   = 11.5;   // lug's lower edge (above the bar bolt heads)
podlug_dx   = 3.5;    // bolt from the pod's end face

/* ---------- Button cap ---------- */
cap_d     = 10;
cap_hole  = cap_d + 0.35;
cap_proud = 1.5;
stem_d    = 7;
flange_d  = 12.0;
collar_t  = 0.8;
cap_gap   = 0.3;
cap_stem  = 0.3;

/* ---------- USB ---------- */
usb_x     = 44;
usb_shift = -4.5;
usb_w     = 15;
usb_h     = 11;
extra_cutouts = [];

$fn = 48;

// =====================================================================
//  Derived: main box
// =====================================================================
drv_above  = pinky_gap < drv_t + 0.5;
inner_w    = board_w + clr + thumb_gap + pinky_gap;
W          = inner_w + 2*wall;
y_thumb_in = W/2 - wall;
y_pinky_in = -W/2 + wall;
board_y0   = y_thumb_in - clr - thumb_gap - board_w;
pcb_bot    = floor_t + under_clear;
pcb_top    = pcb_bot + 1.6;
cell_top   = pcb_top + board_pcb_z;
H_top      = cell_top + top_clr + lid_t;                              // flat top, outside
H_side     = H_top - (W/2 - lid_flat_w/2)*tan(lid_side_ang);          // side-wall top
H          = H_side;
board_yc   = board_y0 + board_w/2;
board_x0   = wall + clr;
board_x1   = board_x0 + board_l;
L          = board_x1 + clr + wall;
drv_xc     = (board_x0 + board_x1)/2 + drv_x;
drv_y      = y_pinky_in + (drv_above ? 0 : 0.5);
drv_z0     = drv_above ? pcb_top + 0.5 : floor_t;
drv_hmax   = H_side - lid_lip_h - 0.3 - drv_z0;

// ---------- hinge (axis along y)
hg_ax   = -hg_out;
hg_az   = H_top - hg_r;                         // knuckle tops flush with the lid top
hg_zw   = hg_az + hg_ax - hg_r*sqrt(2);         // 45 deg underside of the base knuckles meets the wall here
hg_hole = bolt_d + bolt_clr_base;
// elbow-end wall stops at the hinge axis; the lid's end plate covers it above

// ---------- bars.  Profile (u = out from the case side, v = up from the case bottom)
wa      = wing_angle; cw = cos(wa); sw = sin(wa); ta = tan(wa);
w_h     = wing_t/cw;
w_drop  = bar_len_s*sw;
w_reach = (w_drop + w_h)/ta;
bar_xs  = [lug_dx, L - lug_dx];                  // bar bolts
pocket_u1 = lug_u + lug_r + 0.3;                 // bar's pocket round the lug
cap_u1  = pocket_u1 + 2;                         // end cap's outer web
cap_x1  = lug_dx + lug_r + 0.3 + bar_end;        // end cap length; the slot starts here
slot_len= L - 2*cap_x1;
nut_rc  = (nut_af + nut_clr)/sqrt(3);            // hex recess corner radius

// ---------- antenna
plate_full = plate_t + (sma_hex_af > 0 ? sma_hex_d : 0);
boss_t     = max(0, plate_full - wall);
ant_out    = boss_t + ant_nut + ant_reach - ant_d0/2;
ant_tip    = ant_x + ant_len;
hex_r      = sma_hex_af/2/cos(30);
ant_skin   = ant_out*sw + ant_z*cw - ant_d0/2;
ant_d      = ant_d0 - (ant_d0 - ant_d1)*(clip_x + clip_t/2 - ant_x)/ant_len;
clip_r_in  = ant_d/2 - 0.2;
clip_r_out = clip_r_in + clip_wall;
tab_x0     = clip_x - ctab_t;
tab_zw     = w_h + root_fil + 0.3;               // as v9 (clip unchanged)
bolt_z     = tab_zw + bolt_u + bolt_boss_r*sqrt(2) + 0.3;

// ---------- motor hole
tail_yc = (W/2 - wall)/2;
tail_zc = H_side/2;

// =====================================================================
//  Derived: button pod.  Pod-local frame: X = down the face (outward),
//  Y = along the forearm (toward the hand), Z = out of the face.
// =====================================================================
cp = cos(pod_angle); sp = sin(pod_angle);
pin_w   = flange_d + 1;
pin_l   = flange_d + 2*(lip_clr + tab_t) + 1.5;
cham    = (flange_d - cap_d)/2;
pin_h   = switch_h + collar_t + cham + cap_gap + cap_stem;
pd      = pin_h + pod_fl;
pw      = pin_w + 2*wall;
pl      = pin_l + 2*wall;
pod_xc  = L - wall - pl/2 - 8 + pod_x;
pod_x0  = pod_xc - pl/2;
pod_x1  = pod_xc + pl/2;
pod_y0  = W/2 + pod_wgap + sp*pd;               // cup's inner bottom edge sits at the pod's flat back
pod_z0  = H_side + pod_raise;
pod_M   = [[0, -1, 0, pod_xc], [cp, 0, sp, pod_y0], [-sp, 0, cp, pod_z0], [0, 0, 0, 1]];
function pod_pt(X, Y, Z) = [pod_xc - Y, pod_y0 + X*cp + Z*sp, pod_z0 - X*sp + Z*cp];
function pod_under_z(y) = pod_z0 - sp*(y - pod_y0 + pd*sp)/cp - pd*cp;
btn_X   = wall + pin_w/2;
sw_top  = -pin_h + switch_h;
cap_ft  = -cap_gap - sw_top;
cap_top = lid_t + cap_proud - sw_top;
stem_len= cap_ft - cham - collar_t;
link_z    = pod_z0 + (-pin_h + link_up)*cp - sp*wall + link_hole_d/2;
link_Ztop = (sp*wall + (link_z - pod_z0) + link_hole_d/2)/cp;
cav_corner= pod_pt(wall, 0, -pin_h);
pod_gap   = pod_under_z(W/2 + slot_u) - (w_h - slot_u*ta);   // pod underside above the bar at the slot
pod_out   = pod_pt(pw, 0, -pd);                              // pod's outer bottom edge
pb_z      = podlug_z0 + podlug_r;                            // pod bolts: height
pb_xs     = [pod_x0 - podlug_dx, pod_x1 + podlug_dx];        //            along the arm
pb_nut_top= pb_z + nut_af/sqrt(3) + 0.05;                    // pod nut inside, worst case

usb_xw  = board_x0 + board_l - usb_x + usb_shift;
usb_z   = pcb_bot - usb_drop;
usb_top = usb_z + usb_h/2;
usb_k0  = usb_xw - usb_w/2 - 1;
usb_k1  = usb_xw + usb_w/2 + 1;
cutouts = extra_cutouts;

// ---------- checks
assert(lid_flat_w < W - 4, "lid_flat_w too wide for the case");
assert(H_side >= ant_z + hex_r + lid_lip_h + 0.3, "SMA connector hits the lid's lip inside: lower ant_z or lid_flat_w wider");
assert(ant_z - hex_r >= pcb_top + 0.5, "SMA connector sits on the board: raise ant_z");
assert(drv_comp <= pinky_gap - drv_t - (drv_above ? 0 : 0.5) + (board_w - holder_w)/2 - 0.5,
       "driver components won't fit beside the battery holder");
assert(drv_h <= drv_hmax, str("driver veroboard too tall: ", drv_hmax, " mm fits under the lid's lip"));
assert(hg_out >= nut_af/sqrt(3) + 0.2, "hinge nut hits the elbow-end wall: raise hg_out");
assert(hg_zw >= tail_zc + tail_hole[1]/2 + 0.2, "hinge knuckles run into the motor hole");
assert(hg_ko + 3 <= W/2 - corner_r, "hinge too wide");
assert(wing_angle >= 20 && wing_angle <= 60, "wing_angle outside 20..60");
assert(slot_u >= bar_root_u + 0.5 && slot_u >= root_fil + 0.5, "band slot runs into the bar's root: raise slot_u");
assert(w_reach - (slot_u + slot_w) >= 4, "bar outside the slot too thin: lower slot_u or slot_w");
assert(slot_len >= 80, str("band slot only ", slot_len, " mm (want 80+)"));
assert(lug_u - nut_rc >= 0.3, "nut recess in the bar lug runs into the case wall: raise lug_u");
assert(lug_dx - lug_r >= 0.2, "bar lug runs past the case end: raise lug_dx");
assert(bar_top - lug_t - lug_clr >= 2.5, "bar end tab too thin");
assert(ant_x - ant_plate_r >= lug_dx + 2.75 + 0.5, "SMA patch runs into the bar bolt head: raise ant_x");
assert(ant_tip <= L, "antenna tip runs past the hand end: lower ant_x");
assert(clip_x + clip_t < ant_tip - 3, "clip is past the antenna tip: lower clip_x");
assert(clip_x - ctab_t > ant_x + ant_plate_r + 5, "clip tab too close to the connector");
assert(bolt_z + bolt_boss_r <= H_side - 0.3, "clip tab sticks up past the wall top: the lid won't go on");
assert(bolt_z - clip_foot >= bar_top + 0.3, "clip's foot sits on the pinky bar");
assert(tail_yc + tail_hole[0]/2 <= W/2 - corner_r && tail_yc - tail_hole[0]/2 >= 0,
       "motor hole doesn't fit between the centre line and the thumb wall");
assert(link_Ztop < -(collar_t + cham + cap_gap) - 0.3, "pod wire hole reaches the cap's collar");
assert(link_z + link_hole_d/2 <= H_side - lid_lip_h - 0.25, "pod wire hole hits the lid's lip: lower pod_raise");
assert(cav_corner[1] > W/2 + pod_wgap + 1.2, "pod cavity too close to its back");
assert(pod_gap > 5, "pod too low: the band can't get round the thumb bar under it");
assert(pod_x1 <= L - cap_x1 - 0.3, "pod runs into the thumb bar's hand-end cap: lower pod_x");
assert(pb_xs[1] + nut_af/sqrt(3) <= L - wall - 1, "pod's hand-end nut runs into the case corner: lower pod_x");
assert(podlug_z0 >= bar_top + 3 + 0.4, "pod lugs sit on the bar bolt heads: raise podlug_z0");
assert(pb_nut_top <= H_side - lid_lip_h - 0.2, "pod nuts inside hit the lid's lip: lower podlug_z0");
assert(podlug_dx >= 2.75 + 0.5, "pod bolt head hits the pod: raise podlug_dx");
assert(usb_k1 < pb_xs[0] - podlug_r - 0.5, "USB opening runs into the pod's elbow-end lug");
assert(stem_len >= 0, "cap stem negative");

echo(str("Main box: ", L, " x ", W, "; ", H_side, " tall at the sides, ", H_top, " on the flat top"));
echo(str("Lid: flat top ", lid_flat_w, ", sides ", lid_side_ang, " deg; hinge axis ", hg_out, " mm outside the elbow end, ",
         hg_az, " up; base knuckle underside meets the wall at ", hg_zw));
echo(str("Bars: reach ", w_reach, " mm out, ", w_drop, " below the case bottom; band slots ", slot_len,
         " mm long (x ", cap_x1, " to ", L - cap_x1, "), ", slot_w, " wide, vertical; bolts at x ", bar_xs, ", ", lug_u, " mm out"));
echo(str("Antenna: connector ", ant_x, " mm from the elbow end, ", ant_z, " up; element ", ant_out,
         " mm out; tip ", L - ant_tip, " mm short of the hand end"));
echo(str("Clip: x ", clip_x, " to ", clip_x + clip_t, "; bolt ", bolt_u, " mm out, ", bolt_z, " up"));
echo(str("Pod: x ", pod_x0, " to ", pod_x1, "; ", pw, " x ", pl, " x ", pd, " deep; top ", pod_z0, " up; sticks out ",
         pod_out[1] - W/2, " mm; ", pod_gap, " mm above the band at the slot; bolts at x ", pb_xs, ", ", pb_z, " up"));
echo(str("Driver veroboard: max height ", drv_hmax, " mm"));
echo(str("USB opening: centre ", usb_xw, " mm from the elbow end, ", usb_z, " mm up, top at ", usb_top));
echo(str("Motor hole: ", tail_hole[0], " x ", tail_hole[1], ", centre ", tail_zc, " up"));

// =====================================================================
//  Helpers
// =====================================================================
module arm_m() { if (arm == "right") mirror([0, 1, 0]) children(); else children(); }
module pinky() { mirror([0, 1, 0]) children(); }

module rr(x0, x1, y0, y1, r) {
  r2 = min(r, (x1-x0)/2 - 0.01, (y1-y0)/2 - 0.01);
  translate([x0 + r2, y0 + r2]) offset(r = r2) square([x1-x0-2*r2, y1-y0-2*r2]);
}
// 2D profile in (u = out from the case side, v = up), extruded along x, on the +y side
module along_x(x0, len) { translate([x0, W/2, 0]) rotate([90, 0, 90]) linear_extrude(len) children(); }
// 2D profile in (y, z), extruded along x from x0 to x1
module prism_x(x0, x1) { translate([x0, 0, 0]) rotate([90, 0, 90]) linear_extrude(x1 - x0) children(); }
// 2D shape in (x, z), extruded across y from y0 to y1
module across_y(y0, y1) { translate([0, y1, 0]) rotate([90, 0, 0]) linear_extrude(y1 - y0) children(); }

// half-hexagon outline of the case top, (y, z)
module prof2d(z0) {
  polygon([[-W/2, z0], [-W/2, H_side], [-lid_flat_w/2, H_top], [lid_flat_w/2, H_top], [W/2, H_side], [W/2, z0]]);
}
module prof_in2d() { offset(delta = -lid_t) prof2d(H_side - 30); }

module cutouts_3d() {
  for (c = cutouts) {
    side = c[0]; p = c[1]; z = c[2]; w = c[3]; h = c[4];
    if (side == "elbow") translate([-1, p - w/2, z - h/2]) cube([wall + 2, w, h]);
    if (side == "hand")  translate([L - wall - 1, p - w/2, z - h/2]) cube([wall + 2, w, h]);
    if (side == "thumb") translate([p - w/2, W/2 - wall - 1, z - h/2]) cube([w, wall + 2, h]);
    if (side == "pinky") translate([p - w/2, -W/2 - 1, z - h/2]) cube([w, wall + 2, h]);
  }
}

// ---------- pod-lid snap (unchanged from v9)
function pod_tabs()  = [for (s = [-1, 1]) [btn_X, s]];
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
//  Bars (separate parts).  Built on the +y side, in place.
// =====================================================================
module wing_solid2d() {     // v9 profile: plate from the case's bottom edge, flat foot, root fillet
  polygon([[-1.5, 0], [0, 0], [w_drop/ta, -w_drop], [w_reach, -w_drop],
           [root_fil, w_h - root_fil*ta], [0, w_h + root_fil], [-1.5, w_h + root_fil]]);
}
module wing2d() { offset(r = 0.8) offset(delta = -0.8) wing_solid2d(); }
module bar2d() {            // cross-section along the slot
  intersection() { wing2d(); translate([bar_gap, -50]) square([100, 100]); }
  translate([bar_gap, 0]) square([bar_root_u - bar_gap, bar_top]);
}
module cap2d() {            // cross-section at each end: tab over the lug, web outside it
  hull() {
    bar2d();
    translate([bar_gap, lug_t + lug_clr]) square([cap_u1 - bar_gap, bar_top - lug_t - lug_clr]);
  }
}
module bar(usb) {
  difference() {
    union() {
      along_x(0, L) bar2d();
      along_x(0, cap_x1) cap2d();
      along_x(L - cap_x1, cap_x1) cap2d();
    }
    // band slot, vertical
    translate([0, W/2, -30]) linear_extrude(60) rr(cap_x1, L - cap_x1, slot_u, slot_u + slot_w, 1);
    for (xb = bar_xs) {
      // pocket round the base lug: open toward the wall and underneath
      translate([xb - lug_r - 0.3, W/2 - 5, -30]) cube([2*(lug_r + 0.3), 5 + pocket_u1, 30 + lug_t + lug_clr]);
      translate([xb, W/2 + lug_u, -1]) cylinder(d = bolt_d + bolt_clr_flat, h = bar_top + 2, $fn = 24);
    }
    if (usb) {   // USB plug: everything inside the slot cut away, as in v9
      translate([usb_k0, W/2 - 1, -30]) cube([usb_k1 - usb_k0, 1 + slot_u + 0.01, 60]);
      translate([usb_k0, W/2 - 1, usb_z - usb_h/2]) cube([usb_k1 - usb_k0, w_reach + 5, 40]);
    }
  }
}
module bar_thumb() { bar(true); }
module bar_pinky() {        // +y side; pinky() puts it in place
  difference() {
    bar(false);
    along_x(tab_x0 - 0.3, ctab_t + 0.6) offset(r = 0.3) clip_tab2d();   // root notched under the clip tab
  }
}

// breakaway print ribs: fill the slot locally, up to just under the outer bar (snap off after printing)
module bar_ribs3d() {
  n = ceil(slot_len/rib_sp) - 1;
  u0 = slot_u + 0.5; u1 = slot_u + slot_w + 1.6;
  for (i = [1 : n]) along_x(cap_x1 + i*slot_len/(n + 1) - rib_t/2, rib_t)
    polygon([[u0, w_h - u0*ta + rib_g], [u1, w_h - u1*ta + rib_g], [u1, bar_top], [u0, bar_top]]);
}

// lugs on the base, flush with the floor; hex nut recess underneath
module base_lugs() {
  for (xb = bar_xs) difference() {
    linear_extrude(lug_t) hull() {
      translate([xb, W/2 + lug_u]) circle(r = lug_r);
      translate([xb - lug_r, W/2 - 1.9]) square([2*lug_r, 1.9]);
    }
    translate([xb, W/2 + lug_u, -1]) cylinder(d = bolt_d + bolt_clr_flat, h = lug_t + 2, $fn = 24);
    translate([xb, W/2 + lug_u, -0.01]) rotate([0, 0, 30]) cylinder(r = nut_rc, h = nut_h + 0.2 + 0.01, $fn = 6);
  }
}

// =====================================================================
//  Antenna (+y side; base() puts it on the pinky side)
// =====================================================================
module sma_boss() {         // patch outside the wall; its underside rises at 45 deg above the bar
  if (boss_t > 0) intersection() {
    translate([ant_x, W/2 - 0.5, ant_z]) rotate([-90, 0, 0]) cylinder(r = ant_plate_r, h = boss_t + 0.5);
    along_x(ant_x - ant_plate_r - 1, 2*ant_plate_r + 2)
      polygon([[-1, bar_top + 0.3 - bar_gap - 1], [boss_t + 1, bar_top + 0.3 + boss_t + 1 - bar_gap], [boss_t + 1, 40], [-1, 40]]);
  }
}
module sma_cuts() {
  translate([ant_x, W/2 - wall - 1, ant_z]) rotate([-90, 0, 0]) linear_extrude(plate_full + 2) sma_hole2d();
  if (sma_hex_af > 0) translate([ant_x, W/2 - wall - 0.01, ant_z]) rotate([-90, 0, 0])
    linear_extrude(sma_hex_d + 0.01) circle(r = hex_r, $fn = 6);
}
module clip_tab2d() {
  hull() {
    translate([-1, tab_zw - 1]) square([1, bolt_z + bolt_boss_r - tab_zw + 1]);
    translate([bolt_u, bolt_z]) circle(r = bolt_boss_r);
  }
}
module clip_tab() {
  difference() {
    along_x(tab_x0, ctab_t) clip_tab2d();
    translate([tab_x0 - 1, W/2 + bolt_u, bolt_z]) rotate([0, 90, 0])
      cylinder(d = bolt_d + bolt_clr_base, h = ctab_t + 2, $fn = 24);
  }
}
module clip_piece() {
  along_x(clip_x, clip_t) difference() {
    union() {
      hull() {
        translate([bolt_u, bolt_z]) circle(r = bolt_boss_r);
        translate([ant_out, ant_z]) circle(r = clip_r_out);
      }
      hull() {
        translate([bolt_u, bolt_z]) circle(r = bolt_boss_r);
        translate([foot_gap, bolt_z - clip_foot]) square([2.5, clip_foot]);
      }
    }
    translate([ant_out, ant_z]) circle(r = clip_r_in);
    translate([ant_out - 0.38*ant_d, ant_z]) square([0.76*ant_d, clip_r_out + 20]);
    translate([bolt_u, bolt_z]) circle(d = bolt_d + bolt_clr_flat, $fn = 24);
  }
}
module clip_test() {
  for (i = [0 : 2]) translate([i*16, 0, 0]) linear_extrude(clip_t) {
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
//  Button pod (separate part, +y = thumb side, in place)
// =====================================================================
module pod_cup() { multmatrix(pod_M) translate([0, -pl/2, -pd]) linear_extrude(pd) rr(0, pw, 0, pl, corner_r); }
module pod_solid() {        // cup, filled back to a flat face against the wall
  zb = pod_under_z(W/2 + pod_wgap);
  hull() {
    pod_cup();
    translate([pod_x0, W/2 + pod_wgap, zb]) cube([pl, 0.5, pod_z0 - zb]);
  }
}
module pod_lugs() {         // flat against the wall at each end
  for (e = [-1, 1]) {
    xe = e > 0 ? pod_x1 : pod_x0;
    xb = e > 0 ? pb_xs[1] : pb_xs[0];
    translate([0, W/2 + pod_wgap + podlug_t, 0]) rotate([90, 0, 0]) linear_extrude(podlug_t) hull() {
      translate([xb, pb_z]) circle(r = podlug_r);
      translate([e > 0 ? xe - 1 : xe, pb_z - podlug_r]) square([1, pod_z0 - (pb_z - podlug_r)]);
    }
  }
}
module link_hole() {        // wires: one straight hole through the case wall and into the pod's cavity
  translate([pod_xc, W/2 - wall - 1, link_z]) rotate([-90, 0, 0]) cylinder(d = link_hole_d, h = wall + 1 + pod_wgap + 12);
}
module pod_bolt_holes(d) {
  for (xb = pb_xs) translate([xb, W/2 - wall - 1, pb_z]) rotate([-90, 0, 0]) cylinder(d = d, h = wall + pod_wgap + podlug_t + 2, $fn = 24);
}
module pod_cuts() {
  multmatrix(pod_M) translate([wall, -pin_l/2, -pin_h]) cube([pin_w, pin_l, pin_h + 20]);    // cavity
  multmatrix(pod_M) snap_pockets(-pin_l/2, pin_l/2, 0, pod_tabs());
  // pod-lid pry notches on the hand and elbow faces, over the lid's snap tabs
  for (s = [-1, 1]) multmatrix(pod_M)
    translate([btn_X - 3, s > 0 ? pin_l/2 - 1 : -pin_l/2 - wall - 1, -1.2]) cube([6, wall + 2, 1.3]);
  link_hole();
  pod_bolt_holes(bolt_d + bolt_clr_flat);
}
module pod_posts() {
  q = switch_w/2 + 0.3 + sw_post_d/2;
  multmatrix(pod_M) for (dx = [-1, 1], dy = [-1, 1])
    translate([btn_X + dx*q, dy*q, -pin_h - 0.01]) cylinder(d = sw_post_d, h = sw_post_h + 0.01, $fn = 16);
}
module pod_part() {
  difference() { union() { pod_solid(); pod_lugs(); } pod_cuts(); }
  pod_posts();
}

// =====================================================================
//  Base
// =====================================================================
module body_outer() {
  intersection() {
    linear_extrude(H_top + 1) rr(0, L, -W/2, W/2, corner_r);
    union() {
      translate([-1, -W, 0]) cube([L + 2, 2*W, H_side]);                       // side walls
      intersection() {                                                         // hand-end wall: full profile
        translate([L - wall, -W, 0]) cube([wall + 1, 2*W, H_top + 1]);
        prism_x(-1, L + 1) prof2d(-1);
      }
      intersection() {                                                         // elbow-end wall: up to the hinge axis
        translate([-1, -W, 0]) cube([wall + 1, 2*W, hg_az]);
        prism_x(-1, L + 1) prof2d(-1);
      }
    }
  }
}
module base_knuckle2d() {
  hull() {
    translate([hg_ax, hg_az]) circle(r = hg_r);
    translate([0, hg_zw]) square([max(0.25, hg_ax + hg_r), hg_az - hg_zw]);
  }
}
module base_knuckles() {
  across_y(hg_kl + hg_gap, hg_ko) base_knuckle2d();
  across_y(-hg_ko, -(hg_kl + hg_gap)) base_knuckle2d();
}
module base() {
  difference() {
    union() {
      body_outer();
      base_lugs();
      pinky() { base_lugs(); sma_boss(); clip_tab(); }
      base_knuckles();
    }
    translate([0, 0, floor_t])
      linear_extrude(H_top + 2) rr(wall, L - wall, -W/2 + wall, W/2 - wall, max(corner_r - wall, 0.5));
    // USB: open-bottomed notch in the thumb wall
    translate([usb_xw - usb_w/2, W/2 - wall - 1, -1]) cube([usb_w, wall + 2, usb_top + 1]);
    // pod: wire hole and two bolt holes
    link_hole();
    pod_bolt_holes(bolt_d + bolt_clr_base);
    pinky() sma_cuts();
    // motors and their wires
    translate([tail_end == "hand" ? L - wall - 1 : -1, 0, 0]) rotate([90, 0, 90]) linear_extrude(wall + 2)
      rr(tail_yc - tail_hole[0]/2, tail_yc + tail_hole[0]/2, tail_zc - tail_hole[1]/2, tail_zc + tail_hole[1]/2, 1);
    // hinge: bolt hole, room for the lid's knuckle
    translate([hg_ax, -hg_ko - 1, hg_az]) rotate([-90, 0, 0]) cylinder(d = hg_hole, h = 2*hg_ko + 2, $fn = 24);
    translate([hg_ax, -(hg_kl + hg_gap), hg_az]) rotate([-90, 0, 0]) cylinder(r = hg_r + hg_clr, h = 2*(hg_kl + hg_gap));
    translate([-1, -(hg_kl + hg_gap), hg_az - hg_r - hg_clr]) cube([wall + 1.01, 2*(hg_kl + hg_gap), 20]);   // slot for the lid's knuckle
    // hand end: notch for the lid's flap bridge, groove for its click bump
    translate([L - wall - 0.5, -(fl_w/2 + 0.4), H_top - lid_t - 0.25]) cube([wall + 2, fl_w + 0.8, 10]);
    translate([L - fl_groove_d, -(fl_w/2 + 0.4), H_top - fl_h + fl_bump_z - 0.3]) cube([fl_groove_d + 1, fl_w + 0.8, fl_bump_h + 0.6]);
    cutouts_3d();
  }
  mount_pillars();
}

function hole_xy(h) = [board_x0 + board_l - h[0], board_y0 + board_w - h[1]];
module mount_pillars() {
  for (h = mount_holes) translate([hole_xy(h)[0], hole_xy(h)[1], floor_t - 0.01]) {
    cylinder(d = pillar_d, h = pcb_bot - floor_t + 0.01);
    cylinder(d = mount_hole_d - pin_clr, h = pcb_top + pin_above - floor_t - 0.6);
    translate([0, 0, pcb_top + pin_above - floor_t - 0.6])
      cylinder(d1 = mount_hole_d - pin_clr, d2 = mount_hole_d - pin_clr - 0.6, h = 0.6);
  }
}

// =====================================================================
//  Lid, in place (closed)
// =====================================================================
module main_lid() {
  lx0 = wall;                      // elbow end: the lid's end plate covers the end wall
  lx1 = L - wall - lid_gx;
  yi  = W/2 - wall - lid_lip_clr;  // outer face of the lips
  difference() {
    union() {
      prism_x(lx0, lx1) difference() { prof2d(H_side); prof_in2d(); }        // shell
      for (s = [-1, 1])                                                       // side lips
        translate([lx0 + 1.2, s > 0 ? yi - lid_lip_t : -yi, H_side - lid_lip_h]) cube([lx1 - lx0 - 2.2, lid_lip_t, lid_lip_h + 1]);
      intersection() {                                                        // end plate over the elbow-end wall
        prism_x(0, lx0 + 0.01) prof2d(H_side);
        translate([-1, -W, hg_az + hg_zg]) cube([lx0 + 2, 2*W, 20]);
      }
      across_y(-hg_kl, hg_kl) {                                               // hinge knuckle
        hull() {
          translate([hg_ax, hg_az]) circle(r = hg_r);
          translate([hg_ax - hg_r + 1, H_top - lid_t]) square([lx0 + 3 - (hg_ax - hg_r + 1), lid_t]);
        }
        translate([hg_ax, hg_az - hg_r]) square([lx0 - hg_ax, hg_r]);       // fills its slot in the end wall
      }
      // click flap over the hand-end wall
      translate([lx1 - 0.01, -fl_w/2, H_top - lid_t]) cube([L + fl_gap + fl_t - lx1 + 0.01, fl_w, lid_t]);
      translate([L + fl_gap, -fl_w/2, H_top - fl_h]) cube([fl_t, fl_w, fl_h]);
      translate([L + fl_gap + 0.01, -fl_w/2, H_top - fl_h + fl_bump_z]) across_y(0, fl_w)
        polygon([[0, 0], [-fl_bump, fl_bump], [-fl_bump, fl_bump_h - fl_bump], [0, fl_bump_h]]);
    }
    translate([hg_ax, -hg_kl - 1, hg_az]) rotate([-90, 0, 0]) cylinder(d = hg_hole, h = 2*hg_kl + 2, $fn = 24);
    for (s = [-1, 1])                                                         // room round the base knuckles
      translate([hg_ax, s > 0 ? hg_kl + 0.001 : -(hg_ko + hg_gap), hg_az]) rotate([-90, 0, 0])
        cylinder(r = hg_r + hg_clr, h = hg_ko + hg_gap - hg_kl - 0.001);
  }
}
module lid_open(a) {   // lid swung open by a degrees about the hinge
  translate([hg_ax, 0, hg_az]) rotate([0, -a, 0]) translate([-hg_ax, 0, -hg_az]) main_lid();
}

// pod lid in pod-local coordinates (unchanged)
module pod_lid_local() {
  difference() {
    snap_lid(wall, wall + pin_w, -pin_l/2, pin_l/2, pod_tabs());
    translate([btn_X, 0, -lip_h - 1]) cylinder(d = cap_hole, h = lid_t + lip_h + 2);
    translate([btn_X, 0, -lip_h - 1]) cylinder(d = flange_d + 0.8, h = lip_h + 1);
    translate([wall - 1, -(link_hole_d/2 + 1), -lip_h - 1])
      cube([lip_clr + lip_t + 1.5, link_hole_d + 2, lip_h + 1 - 0.01]);
  }
}
module cap() {
  cylinder(d = stem_d, h = stem_len + 0.01);
  translate([0, 0, stem_len]) cylinder(d = flange_d, h = collar_t + 0.01);
  translate([0, 0, stem_len + collar_t]) cylinder(d1 = flange_d, d2 = cap_d, h = cham + 0.01);
  translate([0, 0, cap_ft - 0.01]) cylinder(d = cap_d, h = cap_top - cap_ft + 0.01);
}

// =====================================================================
//  Ghost parts and hardware
// =====================================================================
module ghost_board()   { translate([0, 0, pcb_bot]) difference() {
    translate([board_x0, board_y0, 0]) cube([board_l, board_w, 1.6]);
    for (h = mount_holes) translate([hole_xy(h)[0], hole_xy(h)[1], -1]) cylinder(d = mount_hole_d, h = 4); } }
module ghost_cell()    { translate([board_x0 + 18, board_yc, cell_top - 9.25]) rotate([0, 90, 0]) cylinder(d = 18.5, h = 65); }
module ghost_drv()     { translate([drv_xc - drv_l/2, drv_y, drv_z0]) cube([drv_l, drv_t, drv_h]); }
module ghost_switch()  { multmatrix(pod_M) translate([btn_X - switch_w/2, -switch_w/2, -pin_h]) cube([switch_w, switch_w, switch_h]); }
module ghost_antenna() { pinky() {
    translate([ant_x, W/2 - wall, ant_z]) rotate([90, 0, 0]) cylinder(r = hex_r, h = sma_back);
    translate([ant_x, W/2 + boss_t, ant_z]) rotate([-90, 0, 0]) cylinder(d = 9.2, h = ant_nut);            // panel nut
    translate([ant_x, W/2 + boss_t, ant_z]) rotate([-90, 0, 0]) cylinder(d = 9, h = ant_nut + ant_reach - ant_d0);
    translate([ant_x, W/2 + ant_out, ant_z]) rotate([0, 90, 0]) cylinder(d1 = ant_d0, d2 = ant_d1, h = ant_len); } }
module m3_head() { cylinder(d = 5.5, h = 3, $fn = 24); }
module m3_nut()  { cylinder(r = nut_af/sqrt(3), h = nut_h, $fn = 6); }
module hw_bars()  {   // both sides: head on the bar tab, nut in the lug recess
  for (side = [0, 1]) mirror([0, side, 0]) for (xb = bar_xs) translate([xb, W/2 + lug_u, 0]) {
    translate([0, 0, bar_top]) m3_head();
    translate([0, 0, bar_top - 8]) cylinder(d = 3, h = 8, $fn = 16);
    translate([0, 0, 0.2]) rotate([0, 0, 30]) m3_nut();
  }
}
module hw_pod()   { for (xb = pb_xs) translate([xb, 0, pb_z]) rotate([-90, 0, 0]) {
    translate([0, 0, W/2 + pod_wgap + podlug_t]) m3_head();
    translate([0, 0, W/2 + pod_wgap + podlug_t - 10]) cylinder(d = 3, h = 10, $fn = 16);
    translate([0, 0, W/2 - wall - nut_h]) rotate([0, 0, 30]) m3_nut(); } }
module hw_hinge() { translate([hg_ax, 0, hg_az]) rotate([-90, 0, 0]) {
    translate([0, 0, hg_ko]) m3_head();
    translate([0, 0, hg_ko + 3 - 30]) cylinder(d = 3, h = 30, $fn = 16);
    translate([0, 0, -hg_ko - nut_h]) m3_nut(); } }
module hardware() { hw_bars(); hw_pod(); hw_hinge(); }

// =====================================================================
//  Output
// =====================================================================
if (part == "assembly") arm_m() {
  color("slateblue") base();
  color("mediumpurple") bar_thumb();
  color("mediumpurple") pinky() bar_pinky();
  color("royalblue") pod_part();
  color("lightsteelblue", 0.85) main_lid();
  color("lightsteelblue", 0.85) multmatrix(pod_M) pod_lid_local();
  color("orange") multmatrix(pod_M) translate([btn_X, 0, sw_top]) cap();
  color("darkorange") pinky() clip_piece();
  color("silver") hardware();
  %union() { ghost_board(); ghost_cell(); ghost_drv(); ghost_switch(); ghost_antenna(); }
}
if (part == "base")      arm_m() base();                                                     // upright on its floor
if (part == "bar_thumb") translate([0, 0, bar_top]) rotate([180, 0, 0]) arm_m() { bar_thumb(); if (bar_ribs) bar_ribs3d(); }            // upside down
if (part == "bar_pinky") translate([0, 0, bar_top]) rotate([180, 0, 0]) arm_m() pinky() { bar_pinky(); if (bar_ribs) bar_ribs3d(); }    // upside down
if (part == "pod")       translate([0, 0, -(W/2 + pod_wgap)]) rotate([arm == "right" ? -90 : 90, 0, 0]) arm_m() pod_part();   // on its back
if (part == "lid")       translate([0, 0, H_top]) rotate([180, 0, 0]) arm_m() main_lid();    // flat top on the bed
if (part == "clip")      rotate([0, -90, 0]) translate([-clip_x, 0, 0]) arm_m() pinky() clip_piece();
if (part == "pod_lid")   translate([0, 0, lid_t]) rotate([180, 0, 0]) arm_m() pod_lid_local();
if (part == "cap")       translate([0, 0, cap_top]) rotate([180, 0, 0]) cap();
if (part == "clip_test") clip_test();
