# MorseLink firmware

Wearable Morse-code communicator on a LilyGO T-Beam V1.1 or V1.2 (ESP32,
SX1276 or SX1262 868 MHz; AXP192 or AXP2101 power chip). You key a message with one button; it is decoded on the device and
sent as text over LoRa; the receiver feels it as Morse through two vibration
motors on the forearm (dot = wrist, dash = a sweep from wrist to forearm).

Design: [`docs/SPEC.md`](docs/SPEC.md) (v1.6, includes every decision made
since the original handover; §0d lists the phone-link decisions, §0c
discovery, §0b the power switch and quiet replies, §0a what the radio step
settled).

## Status — 24 October 2026 (v0.6.4)

v0.6.4 (24 Oct): **leaving command mode, keeping a draft, gentler
vibration, the dash's new shape.** Flash both boards; app v0.2.4 names the
new commands (optional).

- **Leave command mode:** in command mode, the 3 s hold again leaves it
  (OK; the draft is kept). The 5 s hold still cancels everything.
- **`K`: keep the draft** past the 60 s limit (OK + ON), until it is sent or
  cancelled; `K` again: the limit back (OK + OFF).
- **`I` 1-9** (was 1-5): `I8` = full drive, as the board starts; `I9` =
  full at 3.4 V (the old `I5`); `I1`-`I7` = PWM at 45-91 %, each motor
  starting with a 30 ms full-power kick (`HAPTIC_KICK_MS`) so it spins up.
  Stored settings convert once (`I5` -> `I9`, `I4` -> `I8`, `I1-3` -> `I7`;
  settings version 4).
- **The dash** (agreed 24 Oct): M1 1.4U, M2 1.9U, both on for 0.3U (M1
  alone 1.1U, both 0.3U, M2 alone 1.6U); the overlap is now in units, so it
  scales with the reading speed (was 1U / 2U with a fixed 20 ms). Console:
  `haptic overlap 30`, `haptic split 125` (hundredths of a unit).
- The timing of every message is unchanged at every setting (host-tested).

v0.6.3 (24 Oct): **`H`, how dashes feel**, for the participant sessions.
`H1` (the default, as before): a dash sweeps from the wrist (M1) to the
forearm (M2); `H2`: a dash is the forearm motor alone for 3 units, dots
stay on the wrist, so which motor buzzes tells dot from dash. The timing is
identical. Stored; `H` alone plays which; after `H1` / `H2` it plays OK and
a sample "A". Console: `haptic dash 1|2`; `status` shows it; the SP line
carries it as a 16th field (app v0.2.3 shows it; v0.2.2 ignores it).

v0.6.2 (21 Oct): **the learning view (for the participant sessions) and
vibration strength (`I`).** Flash it on **both** boards and update the app
to v0.2.2 (`morselink-app-v0.2.2.zip`). Nothing else changed: the tests,
the radio and the link behave exactly as in v0.6.1. Details: SPEC §0g,
HANDOVER §5B.

- **The phone sees Morse as it happens** (phone protocol 4, `phone.h`):
  every press the moment it is released (`KY`: the letter so far, the
  press's length and the dot / dash boundary it was judged by), every
  letter the decoder closes (`KC`, with the pattern as keyed, so an unknown
  `?` shows what was really keyed), each PARIS attempt's verdict (`CB`),
  the keying and reading speeds (`SP`: the learned dot, dash and gaps, the
  thresholds, how steady, Q, the strength), and what plays on the motors
  (`PL`: each Morse part with when it starts, so the app can follow it
  letter by letter; `PL end`).
- **The app** shows each letter's dots and dashes above it, the letter
  being keyed, where the last press landed against the boundary, the
  speeds, and what is buzzing now; its **Learn** page (the school icon) has
  all of it large, the PARIS helper, and modes for practising reading by
  feel (Morse only, Hidden).
- **`I` 1-5: vibration strength.** Sets the motors' supply (the power
  chip's DCDC1) to 3.0 / 3.1 / 3.2 / 3.3 / 3.4 V; `I4` (3.3 V) is what the
  board always had; `I` alone plays the level. Stored. On the T-Beam V1.2
  the same rail powers the ESP32, which needs at least 3.0 V, and the
  AXP2101's DCDC1 stops at 3.4 V: hence the narrow range. Console: `haptic
  strength 1-5`; `status` shows the level and the rail's voltage.

v0.6.1 (20 Oct): **point tests, the meter session from the app, `W`, the key
on GPIO14.** Flash it on **both** boards (the point test needs both), and
update the app to v0.2.1 (`morselink-app-v0.2.1.zip`). Details: TEST_PLAN
§17, SPEC §0f, HANDOVER.

- **Wire the key first:** a push button from **GPIO14** to **GND**, nothing
  else (the ESP32's internal pull-up). This build is the wearable one: the
  key on GPIO14, the board LED off (section 1). No button wired yet? Set
  `USE_ONBOARD_BUTTON 1` in `main/board.h` and the board's middle button is
  the key again. A board that was switched off (`F`) stays off after
  flashing until the key is held 2 s, and in this build that is the key on
  GPIO14 (the middle button no longer wakes it): wire the key before
  flashing, or touch a wire from GPIO14 to GND for 2 s.
- **Point test** (the app's Tests > `point`): both boards still at one spot,
  a label for the spot, a single setting or a sweep (default SF9 / 17,
  SF9 / 22, SF12 / 22 dBm, about 25 min). Each step: 20 messages of 25
  characters from each board, over LoRa, sent even while the link is lost.
  Both boards switch settings by themselves and back at the end.
- **Meter session from the app** (Tests > Meter session), with USB never
  connected: steps 1, 1b / 1c (keying), 2, 3a-3d (advertising, connected,
  LoRa standby with Bluetooth, SF12 listening), 4 (carriers), 5 (motors), 6
  (off); the wrist motor buzzes each step's number. `bench power` on the
  console now waits for USB to go, however long.
- **`W`** in command mode: Bluetooth off / on (OK + OFF / ON), stored
  through restarts. Console: `phone off` / `phone on`.
- **App:** a red notice when the base phone hasn't answered 20 s after
  Start; Stop is resent until the base confirms; a stronger warning when the
  base phone looks offline.
- Phone protocol 3 (`phone.h`): `BENCH POWER` / `BENCH STOP`, `BN` lines,
  `EV BLE_OFF`, the point test's `STEP` lines.

v0.6.0 (16 Oct): **the connection-lost fix, range settings, bench tests.**
Flash it on **both** boards (the two must run the same version for the
range level and the path reports), and update the app to v0.2.0
(`morselink-app-v0.2.0.zip`). The user manual is now
[`docs/MANUAL.md`](docs/MANUAL.md); the app shows the same text.

- **"Connection lost" is reported every time** (the mall session's bug,
  HANDOVER §5.1). The link now has two paths, **LoRa** and the **relay**
  (the phones), each with its own state: connected, checking, one-way (who
  can't hear whom), lost, off. Keepalives carry "I last heard you N s ago"
  for each path (2 bytes), so a board knows when its partner can't hear it.
  Restoring needs proof both ways. Every loss is one event with its reason:
  silence, no-receipt or one-way.
- **What you feel** (agreed 15 Oct): the usual error + ID only when *both*
  paths are down. LoRa alone dropping while the relay carries plays
  nothing: the app shows it. A failed message's error + ID is the only
  alert for that loss (no second one).
- **Range level `G 1-4`** = SF9 / SF10 / SF11 / SF12 (command mode; `G`
  alone plays the level). **Set the same level on both devices by hand**:
  on different levels they can't hear each other at all. The link's timing
  stretches with it (keepalive / lost: 30 / 90 s up to SF10, 45 / 135 s at
  SF11, 60 / 180 s at SF12), and so does the test pace. Stored; `radio sf
  9-12` on the console does the same.
- **22 dBm** is the new default transmit power (the SX1262's maximum; boards
  on 17 move to 22 once, at the first start of v0.6.0). `radio power 2-22`
  still sets others (the older SX1276 stops at 20).
- **Bench tests** (TEST_PLAN §14.1, §14.3): `bench power` steps the board
  through its states by itself for the meter session (USB out; the LED
  blinks each step's number), `bench motor [m1|m2]` plays the phyphox
  pattern, `bench stop` ends either.
- **`rundown-l`**: a LoRa-only run-down (messages over LoRa, Bluetooth off).
- **The phone sees more** (protocol 2): each path's state in `ST`, the radio
  setting (`RD`), command mode as you key it (`CM` / `CE`), and why a link
  was lost (`EV LOST <ID> <why>`). Test mode logs `LINK state=lost,why=...`
  and `PATH` lines; the analysis rebuilds the losses of older logs.
- `status` shows the paths and the radio's SF / range level / power.

v0.5.1 (14 Oct, late): **the power and motor tests**
([`docs/TEST_PLAN.md`](docs/TEST_PLAN.md) §14). Flash it on **both**
boards before the next run-down, so every log has the same format.
- `test dump` now starts with a header line saying which board, which test
  and how it ended, and has two more columns, `m1_ms` and `m2_ms` (how
  long each motor was on in that minute; 0 unless the test plays):
  `minute,mv,charging,m1_ms,m2_ms`.
- New test `rundown-m` (the motor night): `test start rundown-m` in the
  console plays "PARIS" on the motors over and over until you stop it
  (5 s hold) or the battery runs out; Bluetooth off, as in run-down A.
- No pin changes: the motor outputs (GPIO25 / GPIO13) are already driven
  in the test setup, next to the LED, so a motor wired there just works.

14 Oct: the app runs on both phones, and a desk baseline delivered every
message on both paths. New: `tools/analyse_tests.py` turns the phones'
session files into tables (summary.txt, messages.csv, sessions.csv): keep
it in a folder of its own with a `data` folder next to it, put the files
in `data`, run it (or double-click it); the results appear in `results`.

v0.5.0: **test mode** for the field tests in
[`docs/TEST_PLAN.md`](docs/TEST_PLAN.md) (its §11 says exactly what was
built), run from the new phone app (`morselink-app-v0.1.0.zip`, its own
README). Copy over your folder and build as usual.

- The app starts a session on both boards (the base phone follows the rover
  over the internet). Each board then makes its own test traffic, and logs
  every packet, receipt, link change and battery reading as `T,...` lines
  that its phone writes to a file with the time and GPS.
- Every test message goes **once over LoRa and once through the phones**,
  as separate copies with their own receipts, so the two paths can be
  compared. While a test runs, the normal "both ways" relay is off and the
  phone path can't keep the LoRa link up.
- **Nothing plays during a test** (no buzz, no LED) and the key does
  nothing, except: **hold 5 s to stop the test** (the cancel buzz then says
  it's over).
- Bench check without phones: `loopback on`, connect with `S`, then
  `test start dynamic lora rover` in the console; `test stop` ends it.
- Battery run-downs log to flash; afterwards `test dump` prints the CSV.

v0.4.0: **the phone link (Bluetooth LE).** The device now talks to your
phone. The phone app is the next step, so for now you try it with a free
BLE terminal app (§6 step 12).

**Building:** copy it over your folder and build as usual (the first build
takes longer: Bluetooth is compiled in now). If it ever stops with `BLE is
off in this build's sdkconfig`, run `idf.py set-target esp32` once and
build again.

- **Pairing:** command mode, `O`, hold 2 s. For the next 60 s a new phone
  can pair: Android asks "Pair with MorseLink-K7Q?", you tap Pair, and the
  device plays **OK**. No phone within 60 s: **error**. From then on the
  phone reconnects by itself, also after restarts; you never need `O` for
  it again. A phone that tries to pair without `O` is turned away.
- **What the phone gets:** your draft as you key it (letter by letter),
  each message you send and then whether it arrived, each message you
  receive, the link and the battery, and events (requests, connection lost /
  restored, SOS). The exact lines are in `main/phone.h`.
- **Relay:** while the phone is connected, every packet for another device
  also goes to the phone, to be passed on over the internet (MQTT topic
  `morselink/<their ID>`), and packets the phone gets for you come back down
  to the device. The other device keeps whichever copy arrives first, so a
  message gets through if either path works. A general SOS goes to all your
  contacts this way too. (The MQTT part is the app's job: next step.)
- No phone, no change: Bluetooth stays off until the first `O`.

v0.3.6: the two v0.3.5s together: discovery (below) and the loopback fix
from your other chat, which was made on v0.3.4.

Loopback fix: "connection lost" never played after the bench checklist.
The checklist's quiet-replies step switched the test network (`loopback`)
on and never off, and loopback was saved, so it stayed on through restarts.
While it's on, the pretend partner answers in your real partner's name and
the radio is ignored, so a real partner going away was never noticed.
Loopback is now never saved: every restart or switch-off turns it off (a
board still carrying a saved "on" from v0.3.4 clears it once at start-up and
says so in the log).

v0.3.5: **discovery (`D`)**. Command mode, `D`, hold 2 s: about 4 s later the
devices around you play one at a time, nearest first: their ID, then N, M or
F (near / mid / far). Tap to invite one (its ID is echoed, then it gets a
connection request, just like `S`); double tap for the next one; hold 2 s to
stop. Nobody around: error + 0. Devices in silent mode (`Z`) don't answer.
See §4 "Discovery". The near / mid / far limits are first guesses: every
`D` logs each device's path loss, so send me a few numbers and I'll set them.

v0.3.4: your additions.
- **Power switch:** `F` in command mode switches the device off (the echo
  plays OFF; a press during it aborts). Hold the key 2 s to switch it on: one
  buzz says let go, then OK when it's ready. When power arrives (USB plugged
  in, battery fitted, a reset) it only starts if it was switched on, so it
  charges while off. See §4 "Switching on and off".
- **Quiet replies:** in a running conversation (you exchanged a message with
  that device, or connected to it, in the last 5 minutes) a received message
  plays on its own, without attention and ID.

v0.3.3: fixes SX1262 start-up. v0.3.2 found the SX1262 but then misread a
leftover power-up flag as "no TCXO", switched the clock's supply off and
gave up (`no radio` when connecting). The driver now clears that flag
first, tries the TCXO and then a plain crystal, and says why if both fail:
the reason appears in the boot log, in `status` and in `radio`.

v0.3.2: **SX1262** radio support. Some T-Beams (often V1.2) have an SX1262
instead of the SX1276; the firmware now finds out which chip is fitted and
drives either. The two chips hear each other, so mixed pairs work. At boot
look for `SX1262 ready` (or `SX1276 ready`).

v0.3.1: T-Beam **V1.2** support (AXP2101 power chip). On a V1.2 the radio
has no power until the firmware switches its rail on, so v0.3 reported
`no SX1276 found`. On V1.2 the motors share the ESP32's 3.3 V rail, so rail
gating is off there. Also removed a harmless `GPIO isr service already
installed` error line at boot.

| Build step (spec §14) | State |
|---|---|
| 0. Board bring-up: AXP192 rails, pins | done |
| 1. Haptic engine: esp_timer player, M1/M2 sweep, system tones | done |
| 2. Key input + decoder | done |
| 3. Adaptive calibration (PARIS, dictionary validation, rollback, health) | done |
| 4. Command mode + menu | done (`Q R L E B C Z X S P A N F D O`, `G` since v0.6.0, `W` v0.6.1, `I` v0.6.2); `V` is Phase 2 |
| Power switch (`F` / hold 2 s), quiet replies in a conversation | **done (v0.3.4)** |
| 6. LoRa radio: SX1276 driver, packets, duty cycle | **done (v0.3)** |
| 7. Linking and pairing: requests, accept / decline, BUSY queue, keepalives, receipts, UNPAIR | **done (v0.3)** |
| 7. Discovery (`D`): sweep, near / mid / far list, invite | **done (v0.3.5)** |
| 8. SOS over the air, relayed by boards in between (up to two hops) | **done (v0.3)** |
| 5. BLE link to the phone: pairing (`O`), live draft, messages, events, relay | **done (v0.4.0)** |
| 9. Phone app (Android): status, live draft, chat, MQTT relay, tests | **done (app v0.1.0)**, running on two phones (14 Oct) |
| Test mode for the field tests (TEST_PLAN.md) | **done (v0.5.0)**; desk baseline 478 / 478 delivered (14 Oct) |
| Power and motor tests (TEST_PLAN.md §14) | `rundown-m` + the dump's header and motor columns **done (v0.5.1)**; `bench power` / `bench motor` **done (v0.6.0)**; the meter session from the app, its new steps **done (v0.6.1)** |
| Point tests (TEST_PLAN.md §17) | **done (v0.6.1)**: firmware, app, analysis |
| `W` (Bluetooth off / on) | **done (v0.6.1)** |
| The learning view (phone protocol 4: `KY`, `KC`, `CB`, `SP`, `PL`) and the app's Learn page | **done (v0.6.2, app v0.2.2)** |
| `I` (vibration strength, DCDC1 3.0-3.4 V) | **done (v0.6.2)**; on the board: untried |
| Connection-lost fix: LoRa and relay paths, every loss reported with its reason | **done (v0.6.0)** |
| Range settings: SF9–12 (`G 1-4`), 22 dBm | **done (v0.6.0)** |
| Analysis of the test logs (`tools/analyse_tests.py`) | tables; lost periods and availability; by SF / power; run-down dumps and battery slopes (v0.6.0); no graphs yet |

Every message now gets a **silent delivery receipt**: OK plays when it
arrives; without one you get error + ID and the text goes back into your
draft.

Verified so far (v0.6.0):
- builds warning-free on **ESP-IDF v6.1**;
- **44,116 host-side checks** (including the 5-minute rule, discovery,
  the phone lines and relay, the test schedules and, new, the paths:
  the 15 Oct "fail then silence" case, one-way with and without the relay,
  the relay dropping, an older partner, SF12): simulated human keying,
  plus the link protocol between simulated devices over a lossy radio
  channel (airtime, collisions, range, restarts, phones relaying),
  including 24 simulated hours of random use; the protocol soak (150 seeds
  × 4 h) passes;
- **60/60 end-to-end checks** with the real firmware in Espressif's ESP32
  emulator, the protocol running against a pretend partner and a pretend
  phone (the emulator has no Bluetooth and can't deep-sleep, so real
  pairing and switching on with the key are only testable on the board);
- not yet on real hardware: the range level and the bench tests (your
  checks: §6, and TEST_PLAN §14).

## 1. Build switches: the wearable build and the test setup

Since v0.6.1 the default is **the wearable build**: the key is a push
button from **GPIO14 to GND** (the ESP32's internal pull-up, no resistor),
the motors on GPIO25 / GPIO13, and the board LED off. Three switches at the
top of `main/board.h`:

```c
#define USE_ONBOARD_BUTTON 0   /* 1: key = the board's middle button (IO38) instead of GPIO14 */
#define USE_ONBOARD_LED 0      /* 1: motor output mirrored on the board LED (GPIO4) */
#define VERBOSE_LOG 1          /* the packet and playback logs below */
```

The two parts of the test setup are now independent (the key no longer
shares the LED's pin). Keep the LED off for the meter session: it adds its
current to every reading. The rest of this section describes the test
setup.

- **Key:** the middle of the T-Beam's three buttons (RST and PWR are the
  others).
- **LED:**

  | Pattern | Looks like |
  |---|---|
  | dot | a bright flash |
  | dash | bright, then dim (the dim part is where the forearm motor takes over) |
  | system tones | bright flashes, e.g. OK = short-long, error = three quick |

- **Serial monitor:** verbose logging is on by default (`VERBOSE_LOG`). Every
  keyed character is logged with its timings, everything played is logged
  as text (`play: [attention] "K7Q" "HI"`), and every radio packet is logged
  with its signal strength (`rx MSG K7Q->ABC #4211 "HI"  -61 dBm, SNR 9.5 dB`).
- **Motor pins:** GPIO25 and GPIO13 are driven either way, so motors can be
  added before the external key.

Pins: M1 = GPIO25, M2 = GPIO13 (each an NPN, 2N2222, through 330 Ω; motor
from 3.3 V with a diode across it), key = **GPIO14** (agreed 20 Oct; GPIO4
until v0.6.0, but that is the LED's pin: the LED lit on every press). M1
must never be on GPIO32 (the radio's DIO2); the build refuses a key on a
strapping pin (0, 2, 12, 15) or one that can't wake the chip. If stray dots
ever appear while the motors run (the key wire picking up noise), add a
10 kΩ resistor from GPIO14 to 3.3 V.

## 2. Build and flash (ESP-IDF v6.1)

In the **ESP-IDF terminal** (the extension's flash button had PATH trouble):

```
idf.py set-target esp32        # once per fresh folder (it resets the build settings)
idf.py build
idf.py -p COM4 flash monitor
```

First boot, in the monitor:

```
id K7Q            <- this device's ID (3 characters, no 0/O/1/I/L)
```

Give the two boards **different** IDs (e.g. `K7Q` and `ABC`).

**Upgrading to v0.4.0 from v0.3.x:** copy the new files over your folder
and `idf.py build` as usual: Bluetooth switches itself on in the build
settings. (Only if the build stops with `BLE is off in this build's
sdkconfig`: `idf.py set-target esp32` once, then build.) Your ID,
settings, contacts and calibration on the board are kept.

Upgrading from v0.2: your settings, contacts and calibration are kept;
loopback is switched off and the old (local-only) recipient is cleared, so
connect again with `S`.

## 3. First use: calibration

The device plays the attention pattern, then **PARIS** in Morse. Key PARIS
back: `.--. .- .-. .. ...`. Then stop for 3 s, or hold 2 s.

- **OK** (pip–long) means calibrated.
- **Error** means try again. The device replays the prompt.

`status` in the console shows what it learned. SOS works even before you calibrate.

## 4. Using it

In the test setup, "press" means the board's middle button, and everything
"felt" is shown on the LED.

| Gesture | Meaning |
|---|---|
| short / long press | dot / dash, decoded live (check with `verbose on`) |
| 8 dots (7+ accepted) | error: deletes the last letter |
| hold → pip (2 s) → release | **send** the draft; in command mode, execute |
| hold → pip pip (3 s) → release | **command mode** (your draft is kept) |
| hold → long (5 s) → release | **cancel everything**, including an active SOS |
| release between 1.5 s and 2 s | nothing (deliberately) |
| any press during playback | stops it. A lone tap is ignored; anything more is keying, so SOS always works |

**Connecting (walkie-talkie link):** command mode, `S` + a slot digit or an
ID (or `P` + an ID), hold 2 s. Nothing plays until the other person answers:

| You feel | Means |
|---|---|
| OK + their ID | connected (a new contact: OK + its slot digit) |
| error + `BUSY` | they're in a conversation; your request waits in their queue, and if they accept later you are connected then |
| error + `NO` | declined |
| error + their ID | unavailable (no answer within ~10 s) |

**Being asked:** you feel attention + their ID + "decision pending". Command
mode, `A` to accept or `N` to decline (`A2` / `N2` for the second request
waiting). The echo plays the requester's ID before it executes. `N` on
someone who isn't a contact also blocks them (`X` + their ID unblocks).

**Sending:** key the message, then hold 2 s. The device plays the
recipient's ID, the message leaves, and **OK** plays when its silent receipt
comes back (usually under a second). A press during the ID cancels the send
and keeps the draft. If no receipt comes, you get **error + ID** and the
text is put back into your draft (in front of anything you keyed since):
hold 2 s to try again.

**Receiving:** a message plays attention + the sender's ID + the text. In a
running conversation it plays the text alone: when your last exchange with
that same device (a message either way, or connecting to them) was at most
5 minutes ago. A message from anyone else is always announced in full, and
after a restart the first message is too. `R` replays with the ID.

**Switching on and off:**

| You do | What happens |
|---|---|
| command mode, `F`, hold 2 s | the echo plays **OFF** (press to abort), then the device goes dark: radio off, nothing plays, messages to you fail with "error + ID" on the sender's side |
| hold the key 2 s while it's off | one short buzz: let go. It starts and plays OK when ready (a few seconds) |
| a short press while it's off | nothing: it goes back to sleep |
| plug in USB, fit a battery, press RST | it only starts if it was switched on; switched off, it stays off (and charges) |

`F` is refused (error) while your own SOS is active: cancel the SOS first.
The board's PWR button still cuts all power, as before, but it doesn't move
the switch. Switched off, the device is asleep and still draws a little, so
for storage over weeks use PWR or take the battery out.

**Discovery:** command mode, `D`, hold 2 s. The echo plays D; about 4 s
later the devices around you play one at a time, nearest first: each one's
ID, then N, M or F (near, mid-range, far). Error + 0 means nobody answered.

While a device is being offered:

| You do | What happens |
|---|---|
| tap once | invite it: its ID plays again (press now to go back to the list), then it gets a connection request, as with `S`. The answer plays as usual: OK + slot digit, or error + BUSY / NO / ID |
| double tap | the next device; after the last one, the long cancel buzz |
| hold 2 s | stop the list (3 s: command mode; 5 s: cancel everything) |
| anything else | error, then the same device again |

Nothing keyed for 30 s ends the list. Messages that arrive meanwhile wait
until it's over; SOS works throughout. Answering someone else's `D` happens
by itself and you feel nothing; in silent mode (`Z`) your device doesn't
answer at all. A device answers the same person at most 3 times in 10
minutes, so nobody can track you by searching over and over.

**The link:** you stay connected to your partner until you pick someone
else. It has two paths, LoRa and the relay through both phones (when both
are connected and online); a message goes both ways and arrives if either
works. When **neither** works you feel **error + ID** (connection lost):
nothing heard for the lost time (90 s at range levels 1–2, 135 s at 3,
180 s at 4), the partner can't hear you (one-way), or a message got no
receipt (its own error + ID is then the only alert). When both ways work
again, **OK + ID**. LoRa alone dropping while the relay still carries plays
nothing (the app shows it). If they connect to someone else, you feel
error + their ID. Sending while the link is lost or ended gives error + ID
and keeps the draft. The app shows each path: connected, checking...,
one-way (who can't hear whom), lost, off.

**Range level (`G`):** command mode, `G` + 1–4, hold 2 s: 1 = SF9 (the
default), 2 = SF10, 3 = SF11, 4 = SF12. Higher reaches further but is
slower on the air (a 100-character message: 0.6 s at 1, 4.3 s at 4).
`G` alone plays the level in use. **Set the same level on both devices**:
on different levels they can't hear each other at all, SOS included.

**Your phone (Bluetooth):** one phone per device, and the device works the
same without it.

| You do | What happens |
|---|---|
| command mode, `O`, hold 2 s | the echo plays O. For 60 s a new phone can pair (in the app, or Android's "Pair" dialog). When it has: **OK**. Nobody in 60 s: **error**. (Your paired phone never needs `O`: during those 60 s it is kept out, then it comes back by itself) |
| nothing (phone already paired) | it reconnects by itself whenever it is near, also after restarts; you feel nothing when it comes or goes |
| `O` again, with a different phone | the new phone replaces the old one, which is kept out during the 60 s so that the new one can get in (there is room for one connection) |
| a phone pairs without `O` | turned away; nothing plays |

While a phone is connected it sees your draft letter by letter, each message
sent (then OK or not delivered) and received, the link, the battery, and
events: requests, lost / restored, SOS. Packets for other devices also go
through it as a second path (the relay, once the app has MQTT).

If the phone "forgets" the device (or you factory-reset the board): `O`,
then pair again. After a factory reset, also tap "Forget" on the phone
(Android Bluetooth settings), or its old keys stop it from reconnecting.

**Commands:** hold 3 s, key the letter and its argument, hold 2 s. The
argument is echoed first, and a press during the echo aborts.

| Command | Does |
|---|---|
| `S` slot or ID | connect to a contact or an ID (asks them) |
| `P` ID | connect to a new ID; plays OK + its slot digit |
| `A` [n] / `N` [n] | accept / decline a request (n = which one, oldest first) |
| `Q` 1–9 | reading speed (5 = 200 ms unit; 9 = 120 ms) |
| `R` [1–9] | replay the last (or an older) received message |
| `L` | list contacts: slot digit + ID |
| `E` | read back the draft |
| `F` | switch off (echo: OFF); hold the key 2 s to switch on again |
| `B` | battery % and the link: OK/error + partner ID |
| `C` | recalibrate (PARIS again; cancel with 5 s to keep the old profile) |
| `D` | discovery: who is around? (see "Discovery" above) |
| `Z` | toggle silent mode (undiscoverable): plays ON/OFF |
| `X` slot or ID | forget a contact (they're told), or unblock a blocked ID |
| `O` | pair your phone: 60 s window; OK when it has, error if none came (see "Your phone" above) |
| `G` [1–4] | range level 1–4 = SF9–SF12 (see "Range level" above); `G` alone plays the level. Set the same on both devices |
| `V` | verification codes: Phase 2. Currently it gives an error |

**SOS:** key `...---...` (or S O S as letters) at any time. To target someone,
follow it with a slot or ID. After 3 s of silence (2 s after a target) you get
the ID echo and three long buzzes, and a press there aborts. It then resends
every 30 s for 10 min, then every 2 min, until a 5 s hold cancels it. Every
device that hears it passes it on once, so it reaches a little beyond direct
range. Whoever taps to acknowledge becomes your recipient if you had none.

## 5. Console (115200 baud, same port as the logs)

| Command | |
|---|---|
| `status` | state, link, radio, calibration, contacts, discovery, battery, draft |
| `id K7Q` | set the device ID |
| `radio` | radio status: power, duty cycle, noise, packet counts |
| `radio ping` | every device in range answers with how well it heard you (`PONG from ABC: they hear us at -61 dBm ...`) |
| `radio power 2-22` | transmit power in dBm (default 22; the SX1276 stops at 20) |
| `radio sf 9-12` | the range level by its SF (as `G 1-4`; stored) |
| `radio log on/off` | log every packet sent and received, with RSSI and SNR |
| `verbose on` | log every keyed character with its timings, and everything played on the motors |
| `text [-s] HELLO` | key a message by injection (`-s`: then hold to send) |
| `key [-u ms] PATTERN` | inject raw keying: `. -`, space = letter gap, `/` = word gap, `S`/`C`/`X` = 2/3/5 s holds |
| `rx K7Q HELLO` | pretend a message arrived |
| `sos K7Q [cancel]` | pretend an SOS (or its cancellation) arrived |
| `loopback on/off` | one board: a pretend partner (running its own copy of the protocol) answers instead of the radio, which is ignored meanwhile. Lasts until the next restart or switch-off |
| `sim req XYZ` | (loopback) the pretend partner, as XYZ, asks to connect, so you can try `A` / `N` |
| `sim deaf on/off` | (loopback) the pretend partner goes out of range / comes back |
| `sim disc` | (loopback) the pretend partner does `D`: its log line says whether your device answered (not in silent mode) |
| `phone` | the phone link: Bluetooth on/off, paired or not, connected, the `O` window, lines and relayed packets (also in `status`) |
| `phone pair` | same as `O`: 60 s for a new phone to pair; OK or error plays |
| `phone forget` | forget the paired phone; Bluetooth is off until the next `O` |
| `phone off` / `phone on` | as `W` (v0.6.1): Bluetooth off, stored through restarts / on again (needs a paired phone) |
| `phone sim on/off` | a pretend phone (no Bluetooth needed): every line the device would send it appears in the log as `ble: phone << ...` |
| `phone send LINE` | (pretend phone) as if the phone had sent LINE, e.g. `phone send HI` |
| `test start dynamic both rover` | test mode from the console (TEST_PLAN.md; normally the app starts it). Tests: `baseline`, `dynamic`, `point` (v0.6.1, the default sweep; `test start test=point plan=9:17,12:22 n=20 role=rover` for another plan), `rundown-a`, `rundown-b`, `rundown-l` (LoRa only, Bluetooth off), `rundown-m` (PARIS on the motors until stopped; TEST_PLAN §14); paths `both`, `lora`, `relay`; role `rover`, `base`. The pace follows the range level |
| `bench power` | the meter session (TEST_PLAN §17.7): prints the plan, then once USB is out (it waits, however long) the board steps through its states by itself, motor M1 buzzing each step's number; a 5 s hold or `bench stop` ends it. The app starts it without USB (Tests > Meter session) |
| `bench motor [m1\|m2]` | the phyphox pattern (TEST_PLAN §14.3) on one motor, about 3 min; a press or `bench stop` ends it |
| `test stop` | end the test (or hold the key 5 s) |
| `test dump` | the last battery run-down, as CSV: a `# morselink-rundown board=... test=... end=...` header line, then `minute,mv,charging,m1_ms,m2_ms` (TEST_PLAN §14.4) |
| `play TEXT`, `tone NAME` | render text / a system tone on the motors |
| `motor 255 0` | drive the motors directly (`motor 0 0` stops) |
| `speed 1-9` | reading speed |
| `haptic overlap CU` (hundredths of U since v0.6.4, 30), `haptic split CU` (125), `haptic m1 PCT`, `haptic m2 PCT`, `haptic farnsworth X10`, `haptic gating 0/1` | tune the dash sweep and intensity |
| `haptic strength 1-9` | vibration strength, as `I` (1-9 since v0.6.4): 8 = full (as the board starts), 9 = full at 3.4 V, 1-7 = PWM 45-91 % with a start kick |
| `peer add ID`, `peer del SLOT` | edit contacts |
| `off` | switch off, as `F` does (hold the key 2 s to switch on) |
| `calibrate`, `factory-reset yes`, `restart` | (factory reset also forgets the paired phone) |

## 6. Bench checklist, one board

1. **Boot:** the log shows `TEST SETUP`, `key on GPIO38`, `AXP192 ready` and
   `SX1276 ready: 869.525 MHz, SF9, 125 kHz, CR 4/5, sync 0x5E, 17 dBm`.
   The LED gives an OK flash (or the PARIS prompt on first use).
2. **Key:** press the middle button a few times. The log shows each element
   with its duration, one per press with no doubles.
3. **Calibration:** key PARIS on the button (`.--. .- .-. .. ...`), then stop
   for 3 s. Compare the `status` numbers with how you keyed.
4. **Holds:** hold the button and watch the LED.
   - one flash at 2 s;
   - two at 3 s;
   - a long one at 5 s.
5. **Compose and send (loopback):**
   - `loopback on` in the console.
   - Hold 3 s for command mode, key `S K7Q` (any ID other than this board's),
     hold 2 s. The LED plays K7Q (the echo); a second later OK + 1 (new
     contact in slot 1).
   - Key HI, hold 2 s. The LED plays K7Q, then OK (the receipt), and about
     1.5 s later the message comes back (attention, K7Q, HI).
   - `sim req XYZ`, wait for attention + XYZ + pending, then command mode,
     `A`, hold 2 s: the echo is XYZ, then OK + 2.
   - `loopback off` when done.
6. **SOS (loopback on):**
   - Key SOS. After 3 s you see three long flashes, then OK, then
     delivered + the pretend partner's ID.
   - Hold 5 s to cancel.
   - Then type `sos ABC` in the console and tap the button to acknowledge.
   - `loopback off` when done.
7. **Reboot:** run `restart`. The ID, contacts, recipient and calibration
   should all survive.
8. **Battery:** `status` shows sensible mV and %.
9. **Quiet replies (loopback on):** connect with `S K7Q`, then send HI. The
   echo comes back as just HI (no attention, no K7Q). `rx XYZ HELLO` plays
   attention + XYZ + HELLO; `rx XYZ AGAIN` right after plays just AGAIN.
   `status` shows the conversation and how long ago the last exchange was.
   `loopback off` when done (a restart also ends it).
10. **Power switch:**
    - Command mode, `F`, hold 2 s: the LED plays OFF, then the log says
      `off: hold the key 2 s to switch on` and the board goes quiet.
    - Press the button briefly: nothing (the log shows `the key was only
      tapped: staying off` if the monitor is open).
    - Hold it: after about 2 s one flash; let go. The board starts and gives
      the OK flash.
    - Switch off again, unplug USB and plug it back in (with a battery in,
      nothing happens at all; without one, the log shows `switched off (F),
      so staying off although power arrived`). Hold the button to switch on.
11. **Discovery (loopback on):**
    - Command mode, `D`, hold 2 s. The LED plays D; about 4 s later the
      first device: the pretend partner's ID and N (near). The log lists all
      three: the partner (near), `M4X` (mid) and `F9Z` (far), each with its
      path loss.
    - Double tap: the LED plays `M4X M`. Tap: it plays M4X again, then the
      request goes out (log: `asking M4X to connect`) and OK + a slot digit
      follows (the pretend partner became M4X and accepted).
    - `D` again; while the first entry plays, tap a dash: error, then the
      same entry. Hold 2 s: the long buzz, list closed.
    - `sim disc`: the log says `the pretend partner ... found R2D, near`
      (your ID). Then `Z` (silent mode on) and `sim disc` again: `found
      nobody`. `Z` once more to switch silent mode off.
    - `loopback off` when done (a restart also ends it).
12. **Phone (Bluetooth)**, with a BLE terminal app on your Android phone
    (*Serial Bluetooth Terminal*, as in your first BLE test; *nRF Connect*
    works too). The board's ID is K7Q in these lines; yours shows instead.
    - **Pair:** type `phone pair` in the console (or command mode, `O`, hold
      2 s). The log says `pairing window open for 60 s: connect to
      "MorseLink-K7Q" from the phone`.
    - In the app: menu → Devices → **Bluetooth LE** → Scan → tap
      `MorseLink-K7Q`, then connect. Android asks to pair: tap **Pair**.
    - The LED gives OK; the log says `phone paired` and `phone ready`. The
      terminal shows three lines: `HI K7Q 1 0.4.0`, then `ST - none 87 0`
      (partner, link, battery %, silent mode), then `DR` (the empty draft).
    - **Live draft:** key a few letters on the board. Each one shows up in
      the terminal as `DR H`, `DR HE`, ... A word gap adds a space; the
      error prosign takes the letter back off.
    - **Messages** (`loopback on` for a pretend partner): connect with
      `S ABC`, then send HI. The terminal shows `TX 1 ABC HI`, then `OK 1`
      when the receipt comes, `RX ABC HI` for the echo, and `RL ABC ...`
      lines (copies of the packets, which the app will pass on over the
      internet). `loopback off` afterwards.
    - **From the phone:** send `HI` from the terminal: the three first lines
      come again.
    - **Reconnect:** `restart` the board. The phone connects again by
      itself (in Serial Bluetooth Terminal switch on auto-reconnect in its
      settings, or just tap connect): no `O`, no pairing dialog, no buzz.
    - **Strangers:** a second phone, without `O`: it can't get in (log:
      `refused`).
    - `phone` in the console shows the state at any time. `phone forget`
      unpairs (then also "Forget" `MorseLink-K7Q` in Android's Bluetooth
      settings before pairing it again).

## 7. Two boards: the radio

**Screw both antennas on before powering the boards.** Transmitting without
an antenna can damage the radio, and these boards now transmit by themselves
(keepalives).

Flash both boards and give them different IDs (`id K7Q` on one, `id ABC` on
the other). `loopback` must be **off** on both (`status` shows
`radio: SX1262 ...` or `radio: SX1276 ...`). Keep both serial monitors open if you can; one is
enough.

1. **They hear each other (first milestone).** On K7Q: `radio ping`. Within
   two seconds K7Q's log shows
   `PONG from ABC: they hear us at -45 dBm (SNR 9.8 dB); we hear them at -47 dBm ...`.
   Then the same from ABC. With the boards a metre apart, expect a strong
   signal (well above -80 dBm) and an SNR around +10 dB.
2. **Connect.** On K7Q: command mode, `S ABC`, hold 2 s (echo: ABC).
   - ABC plays attention + K7Q + "decision pending"
     (log: `K7Q asks to connect`).
   - On ABC: command mode, `A`, hold 2 s (echo: K7Q).
   - Both play OK + 1 (each is the other's contact 1). `status` on both:
     `link: ... connected`.
3. **Messages.** On K7Q, key HI, hold 2 s: K7Q plays ABC, then OK; ABC plays
   attention + K7Q + HI. Reply from ABC the same way.
4. **Keepalives.** Leave both alone for 5 minutes: nothing should play. The
   log shows a `KEEPALIVE` each way about every 30 s.
5. **Connection lost / restored.** Switch ABC off (hold its PWR button until
   it goes dark, or unplug it if it has no battery). About 90 s later K7Q
   plays error + ABC. Key a message on K7Q and hold 2 s: error + ABC at
   once, draft kept. Switch ABC on: within ~30 s K7Q plays OK + ABC. Send
   the kept draft (hold 2 s): it goes through.
6. **No receipt.** On K7Q, send a message, and switch ABC off during the ID
   announcement. About 10 s after the message leaves, K7Q plays error + ABC
   and the text is back in the draft (`status`, or `E`). Switch ABC back on.
7. **Forget and decline.** On ABC: `X` + 1 (forget K7Q: K7Q is told, drops
   ABC too and plays error + ABC). On K7Q: `S ABC` again; on ABC: `N` (echo:
   K7Q). K7Q plays error + NO, and ABC now has K7Q blocked (`status`:
   `blocked: K7Q`). On ABC: `X K7Q` unblocks it.
8. **SOS.** On K7Q, key SOS and wait: three long flashes, then OK. ABC
   alarms (5 long buzzes, repeating every 30 s). Tap ABC: it plays K7Q, and
   K7Q plays delivered + ABC; each is now the other's recipient. Hold 5 s on
   K7Q to cancel.
9. **Range.** Carry one board away with `radio ping` every few tens of
   metres (or watch the RSSI of the keepalives in the log). Note where the
   PONGs stop. Try `radio power 20` (both boards) for the report.
10. **Discovery.** On K7Q: command mode, `D`, hold 2 s. About 4 s later K7Q
    plays ABC + N (if the boards are close); the log shows something like
    `1. ABC near: path loss 62 dB (heard at -45 dBm, sent at 17 dBm)`.
    - Tap: K7Q plays ABC again, and ABC plays attention + K7Q + pending
      (a normal request: accept with `A`, as in step 2).
    - On ABC: `Z` (silent mode on). On K7Q: `D` again: error + 0 after
      about 4 s. On ABC: `Z` again.
    - **For the near / mid / far limits:** do `D` with the boards at a few
      distances (same desk, across the room, next room, down the street,
      as far as it still works) and note the path loss each time. Send me
      those numbers and I'll set the limits (now: near below 80 dB, far
      from 105 dB).

With three boards you could also see BUSY (a request while two are chatting);
the host tests cover that case in simulation.

## 8. Tests

```
make -C test/host && ./test/host/run_tests        # pure logic + link protocol, ~10 s
. $IDF_PATH/export.sh && idf.py build
python tools/qemu_test.py                          # firmware in the ESP32 emulator, ~15 min
```

The emulator has no Bluetooth: starting it there stops the emulated chip
(`assert failed: btdm_low_power_mode_init`), so the test uses the pretend
phone (`phone sim on`) and never `O`.

`SOAK_SEEDS=50 ./test/host/run_tests` runs the protocol soak with more random
seeds (4 simulated hours each); `DBG_LINK=1` prints every protocol event.

QEMU needs `python $IDF_PATH/tools/idf_tools.py install qemu-xtensa`; on Linux
it also needs `libslirp0`.

### Simulation results

Each simulated "fist" keys a 600-character corpus after its own PARIS
calibration. Timing jitter is Gaussian.

| Keyer | Errors (all) | Errors (letters only) |
|---|---|---|
| 12 WPM, 10 % jitter | 0.0 % | 0.0 % |
| low dash:dot ratio 2.2 | 0.0 % | 0.0 % |
| speeding up 140 → 85 ms dots | 0.0 % | 0.0 % |
| slowing down 90 → 150 ms dots | 0.0 % | 0.0 % |
| beginner: slow, 16 % jitter, thinking pauses | 7.6 % | 2.0 % (rest are word spaces) |
| very sloppy, 22 % jitter | 12.7 % | 7.5 % (inherent at that jitter) |

SOS:
- 0 false triggers in 400 random keyers (about 240,000 characters of normal text).
- 98 % detection of SOS keyed in "panic": random speed 0.6–2.5×, squashed
  dash ratio, erratic gaps, wrecked calibration.

Link protocol (simulated SX1276 channel with airtime, half-duplex radios,
collisions and range):
- keepalives: about 2 packets a minute per device, **0.5 % duty cycle**, no
  false "connection lost" in an hour;
- at **25 % packet loss**, 40 messages: every one gets exactly one result,
  none is delivered twice, every receipted one really arrived;
- 5-minute rule: first message after connecting, at exactly 5 minutes, just
  over, replies either way, another sender in between, after a restart;
- soak, 150 seeds × 4 hours of four people using it at random (10 % loss,
  walking out of range, restarts, and since v0.3.5 discovery sweeps): no
  message delivered twice except right after a restart, no receipt without
  a delivery, both ends always agree on who is linked once things settle,
  no sweep left hanging;
- discovery: four devices at different distances all answer one sweep and
  come back sorted near, near, mid, far; silent mode and the blocklist both
  ways; the answer limits (3 per searcher, 20 in all, per 10 minutes);
  nobody in range; a sweep replaced by a newer one; answers meant for
  another sweep; the duty cycle used up on either side; two people
  sweeping at the same moment; inviting someone found.

## Layout

```
main/
  board.h       pins (+ compile-time checks)      config.h   timing / behaviour / radio constants
  pmic.c        AXP192 rails, battery              haptics.c  esp_timer + LEDC player
  hseq.c        haptic sequences (pure)            keyer.c    ISR-timestamped key + debounce + holds
  morse.c       code table, ID codec (pure)        decoder.c  keying -> characters, SOS by shape (pure)
  calib.c       adaptive calibration (pure)        peers.c    contacts + blocklist (pure)
  convo.c       "in a conversation?" (pure)        power.c    on/off switch: deep sleep, key wake-up
  proto.c       packet format, airtime, duty       link.c     link protocol engine (pure)
                cycle, duplicates, message IDs     disc.c     discovery: found list, near/mid/far,
                (pure)                                        answer limits (pure)
  radio.c       SX1276 driver + chip detection; radio_sx1262.c: SX1262 driver
                                  net.c      network task: engine + radio + loopback + phone relay
  phone.c       lines to / from the phone, base64, ble.c      Bluetooth LE: NUS service, pairing
                relay targets (pure)                          window, bonding, pretend phone
  ttest.c       test traffic: schedule, texts      ntest.c    test mode in the net task: copies,
                (pure)                                        receipts, T lines, run-down log
  ui.c          interaction state machine          store.c    NVS persistence
  console.c     serial console                     app_main.c start-up
test/host/      host tests: keying simulator, packet layer, link protocol over a simulated radio, phone lines
tools/          qemu_test.py end-to-end emulator test; analyse_tests.py field-test
                logs -> summary.txt + CSV (put it in a folder with data\ next to it)
docs/SPEC.md    design specification v1.6           docs/TEST_PLAN.md  the field tests (§11: as built)
```
