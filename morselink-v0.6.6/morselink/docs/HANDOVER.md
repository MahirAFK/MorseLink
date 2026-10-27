# MorseLink — Handover (21 October 2026: firmware v0.6.2 + app v0.2.2 built, the learning view and `I`; next: the owner's builds, the participant sessions, run-down A to empty, the point tests, the meter session, analysis and graphs, the report)

**Update of 27 October:** firmware **v0.6.6** (app unchanged, v0.2.4):
`bench motor m1|m2 all`, the IMU strength sweep (owner: "no gaps in the
data"): `bench_sweep_block_start()` / `bench_sweep_block_ms()` /
`bench_sweep_ms()` in `bench.c` (pure, host-tested), blocks chained by the
UI on `UIE_PLAY_DONE` (`bench_sweep_block()`, `bench_sweep_done()`), the
supply per block (3.4 V for I9), the stored strength restored on the end or
a stop. 9 × 178.4 s + 1 s = 26.8 min per motor. TEST_PLAN §14.3.

**Update of 26 October:** firmware **v0.6.5** (app unchanged, v0.2.4):
`bench motor` (the IMU / phyphox pattern) now plays steady buzzes at
`I1`-`I8` (with the start kick) and the dot / dash trains at the `I` set;
`bench_motor_start(b, motor, drive_pct, kick_ms)`; 214.4 s. TEST_PLAN §14.3.

**Update of 24 October (later):** firmware **v0.6.4** + app **v0.2.4**:
the 3 s hold in command mode leaves it (draft kept, `CE left`); **`K`**
keeps the draft past 60 s (`U.draft_kept`, RAM only); **`I` 1-9** (PWM
45-91 % for 1-7 with a 30 ms start kick in `hseq.c`'s `push()`, 8 = full,
9 = full at 3.4 V; `hstrength_drive_pct()`; settings version 4 converts);
the dash **M1 1.4U / M2 1.9U / 0.3U together** (`overlap_cu` in hundredths
of U). `HSEQ_BUF` 24. SPEC §0i.

**Update of 24 October:** firmware **v0.6.3** + app **v0.2.3**: command
**`H`** (`H1` the dash sweep M1 → M2, the default; `H2` dashes on M2 alone,
dots on M1, same timing) for comparing the two in the participant sessions;
`hparams_t.dash_style`, `settings_t.dash_style` (the last reserved byte),
console `haptic dash`, SP's 16th field; host tests (H2's segments, equal
duration, the random timing check with both styles) and a QEMU step. SPEC
§0h.

**Update of 21 October (the chat after this file was written):** the owner
reported that v0.6.1 and app v0.2.1 work, and asked for the participant
sessions (questionnaire JS-04): a live transcript of dots and dashes on the
phone above each letter, the calibrated speeds on the display, "last press"
(the timing bar) in, and a command for the vibration strength through
DCDC1. Built as **firmware v0.6.2 + app v0.2.2: §5B** (SPEC §0g). The rest
of this file is as of 20 October unless §5B says otherwise.

For the next chat. This file says where the project stands, how the owner
works, what was built on 20 October and how, what to do next, and the
pitfalls already solved. It replaces the handover of 16 October and its
addendum of 20 October (`HANDOVER_v0.6.1.md`, which specified this update);
nothing in them is lost that still matters.

**The brief of this chat (owner, 20 Oct):** continue from the addendum, which
specified the update (point tests with sweeps, the meter session started
from the app with new steps, `W` for Bluetooth off / on, the app's
robustness fixes, docs and tests); before building, look at the key's GPIO
for when the onboard button goes, and whether an internal pull-up or
pull-down can replace the external resistor. The discussion and the
owner's answers are in §2.0.

**What this chat did:** moved the key to **GPIO14** (internal pull-up, no
resistor) and made the wearable build the default; built and verified
firmware **v0.6.1** and app **v0.2.1** (§5A); TEST_PLAN §17, SPEC v1.8
(§0f), MANUAL, README and README_APP updated; the analysis gained the point
tests (points.csv, attenuation, the path-loss exponent). Delivered as
`morselink-v0.6.1.zip` and `morselink-app-v0.2.1.zip`.

**Read in this order:**
1. This file.
2. `docs/TEST_PLAN.md`: §0–§10 the tests as agreed, §11 test mode, §14 the
   power and motor tests, §15 the field results of 14–15 Oct, §16 what
   v0.6.0 changed, **§17 the point tests and the meter session (v0.6.1)**.
3. `docs/SPEC.md` (v1.8): the design and every decision (**§0f the latest**;
   §6–§8 protocol and input; §10 SOS; §12 the phone).
4. `docs/MANUAL.md` (the user manual; the app shows the same text),
   `README.md` (firmware: build, use, console, bench checklists) and
   `morselink_app/README_APP.md` (the app).
5. The code (§3 is the map).

**The package:** `morselink-v0.6.1.zip` (firmware with today's docs) and
`morselink-app-v0.2.1.zip` (the app's own files only). The field data of
14–19 Oct is on the owner's PC (session files, dumps); ask for it when the
analysis needs it. Unzip into the sandbox and treat it as the reference
("check the work folder against the last zip", §7).

**First things to do in the next chat:**
1. Read; then ask the owner how the builds went: **the app was not compiled
   in the sandbox** (no Flutter or Dart SDK reachable; §6.2), so a compile
   error in the owner's first `flutter run` is possible: fix it from the
   pasted error at once. (v0.2.0 compiled first time the same way.) The
   firmware built warning-free here.
2. If it comes up: **the key must be wired to GPIO14** (button to GND)
   before flashing, or `USE_ONBOARD_BUTTON 1` set (§2.0): in the wearable
   build a board that was switched off can only be woken by the key on
   GPIO14. Flash **both** boards; use `tools/analyse_tests.py` from the
   v0.6.1 zip (his test folder had an old copy on 18 Oct).
3. Ask once about the two defaults taken (§2.4): `W`'s "off" survives a
   restart; the point test's longer gaps (a sweep takes 25 min, not ~20).
4. Set up the sandbox toolchain only if firmware changes are needed (§6.2;
   ~18 minutes in the background).
5. Then follow the owner's pacing (§1): field results first, then graphs and
   the report's numbers (§2.5). The deadline is **26 October**.

---

## 1. The project and the owner

- **What:** a wearable Morse communicator. One button to key; two vibration
  motors on the forearm to read (a dot is the wrist motor; a dash sweeps
  from wrist to forearm). Messages are decoded on the device and sent as
  text over LoRa; the receiver feels them at their own reading speed.
  Transport: LoRa first; the phone (BLE + MQTT relay) is auxiliary. The
  device must work fully with no phone.
- **Who:** a student. **Full project and report due 26 October 2026**
  (6 days from this handover).
- **Hardware (as it really is):** two **LilyGO T-Beam V1.2** boards: ESP32,
  **AXP2101** power chip, **SX1262** LoRa radio with a TCXO (868 MHz),
  **IPEX** antenna sockets, NEO-M8N GPS (unused, powered off), one
  **3000 mAh 18650** each. Board IDs: **DEA** (the rover) and **ABC** (the
  base). The original spec assumed a V1.1 with AXP192 and SX1276; the
  firmware detects and drives both.
- **Hardware design is done** and is not to be changed, with one change by
  the owner on 20 Oct: **the key moved from GPIO4 to GPIO14** (§2.0). The
  motors (Pololu #1638 coin ERM) are driven by the owner's circuit:
  **2N2222, 330 Ω base resistor** (1 kΩ was too weak for the ~120 mA
  start-up), motor from 3.3 V, a diode across it; M1 = GPIO25 (wrist), M2 =
  GPIO13 (forearm). He also bench-tests it with a supply, a signal generator
  (0–3.3 V square) and a scope. **v0.6.1's default build is the wearable
  build** (the external key on GPIO14, the LED off); `board.h`'s switches
  bring back the test setup (the board's middle button IO38 as the key; the
  LED, GPIO4, mirroring the motors), each on its own (§3.3).
- **Phones:** the owner's **Galaxy S10e** (Android 12) and two **Galaxy
  S22+** (the base phone, and one borrowed now and then); all run the app.
  Since 16 Oct the weak point is **the base phone's internet** (§2.3).

**How the owner works:**
- Short replies. When they ask several things, answer point by point,
  numbered. "rq" = quickly: then a few lines only.
- **Plain language and concrete scenarios** over jargon ("you press X, you
  feel Y"). Concrete numbers help a lot: the airtime table settled the SF
  question in one message; on 20 Oct the pull-up currents (0.07 mA against
  0.33 mA against the LED's 2 mA) settled the key pin.
- Few questions, each with a **recommended default**, so a one-word answer
  works ("14", "yes").
- Thinks ideas through out loud and asks one thing at a time before any
  code ("let me know when you're ready to start building"): answer the
  reasoning honestly, including "this won't show up in the data, and here's
  why", with a suggested order.
- For bigger steps they like to **talk it through before any code**, and
  they decide the pacing: on 15 Oct they asked for a handover instead of
  code. Don't start the next step unasked.
- Wants **explanations alongside code blocks**, and **comments in the code**:
  every function (purpose, inputs, outputs, side effects) and every
  non-obvious line, including existing files you touch (§3.3 lists the
  files not yet at that standard).
- Asks quick practical questions in the field ("why are messages skipped?",
  "it shows connected, why?") and sends screenshots. Answer from the code,
  in a few lines; check the code before blaming the field (§7).
- Sends build errors as screenshots or pasted terminal text: find the
  "What went wrong" part, give the fix in a couple of lines.
- Likes things that save typing (the analysis finds its folders itself;
  "Repeat at the next spot") and wants consistency in collected data.
- Receives each firmware version as a zip (`morselink-vX.Y.Z.zip`), copies
  it over their folder and builds in the ESP-IDF terminal (§6.1). App
  updates: a zip of the app's own files, unzipped over the Flutter project
  ("Replace"), then `flutter run --release`.

**Decisions the owner made (still in force):** the relay works "both ways,
always"; a general SOS goes through the phone "to all my contacts"; Android
only for now; a borrowed second phone for the tests; HiveMQ's public
broker; the app's plain default look; discovery thresholds from their own
measurements; **(14 Oct)** the power tests: a multimeter on campus for
current per state, plus battery-curve nights compared with run-down A;
**(15 Oct)** SF and transmit power become optional settings on the device,
including the highest SF "to showcase the full capability"; the app shows
command-mode input; an in-app manual with a Morse table; **(15–16 Oct,
§5.0)** LoRa and Relay as two statuses, no alert while only LoRa is down,
`G 1-4` by hand on each board, max power by default, `rundown-l`; **(20
Oct, §2.0, SPEC §0f)** the key on GPIO14 with the internal pull-up, the
wearable build by default, the meter session on the real motors with a
keying step, point tests (20 × 25 characters per step, the sweep preset, a
sweep at every spot, a typed label, settings for the test only), no
hand-versus-pocket test, the meter session started from the app (`bench
power` stays on the console), `W` for Bluetooth off / on; one run-down A to
empty on DEA is enough.

---

## 2. Where things stand

### 2.0 The key's GPIO and the meter session (20 Oct)

The owner asked first, before any code: once the onboard button goes,
should the key move, and can an internal pull-up or pull-down replace the
external resistor? The discussion, in order:

1. **GPIO4, the planned key pin, is the board LED's pin** on the T-Beam
   (LED from 3.3 V to GPIO4, lit while the pin is low): a key to GND there
   lights the LED on every press (~2 mA while pressed). The owner: "doesn't
   it mean GPIO4 is sometimes an input and sometimes an output?" — yes, in
   the test setup the same pin was the LED mirror (an output), which is why
   the test setup keyed on IO38.
2. **Pull-up or pull-down, internal or 10 kΩ** (the owner asked about
   power): released, every option draws nothing; pressed, the internal
   pull-up (~45 kΩ) draws ~0.07 mA, a 10 kΩ pull-up 0.33 mA (more, not
   less); a pull-down (key to 3.3 V) saves nothing anywhere, and on GPIO4
   it would leave a permanent path through the LED.
3. **"Are you sure there's no other GPIO...?"** The key needs a pin that is
   free, RTC-capable (the power switch wakes the chip with ext0 on the key's
   pin: 4, 12–15, 25–27, 32–39), not a strapping pin (0, 2, 12, 15) and with
   an internal pull-up (34–39 have none). On the T-Beam that leaves
   **GPIO14**; 13 and 25 are the motors, 26 / 27 / 32 / 33 the radio, 35 the
   power chip, 38 the board's button. **The owner: "14".**
4. **The LED off to save power** (asked): in the wearable build the LED
   mirror is off and GPIO4 is held as an input with its pull-up, awake and
   in deep sleep: its reset state includes a weak pull-down (Espressif's pin
   list), a faint glow and ~35 µA through the LED, switched off included.
5. **"More the bench test, we need to test with the actual motors, not the
   LED":** the default build became the wearable build (`USE_ONBOARD_LED
   0`, so the LED adds nothing to the readings), and the meter session's
   step signals buzz on **motor M1** (owner: "yes").
6. **A keying step after the idle one** (the owner's idea): **1b** keying
   PARIS (45 s) and **1c** the key held (three holds of ~3 s, 15 s): what
   keying costs, and the pull-up while pressed.

Fallback if stray dots ever show up (a long key wire next to the motor
wires picking up noise): a 10 kΩ resistor from GPIO14 to 3.3 V.

**The consequence to remember:** in the wearable build only the key on
GPIO14 wakes a board that was switched off (`F`, or the meter session's
step 6); a switched-off board stays off through a reset, a flash or USB
power (`power.c`). Without the key wired, a wire from GPIO14 to GND for 2 s
does it (README's status says so).

### 2.1 Built and tested

| Area | State |
|---|---|
| Board bring-up: power chip (AXP192 or AXP2101), pins, build switches | done; **v0.6.1: the key on GPIO14 (internal pull-up), the wearable build by default, the LED's pin held dark** |
| Haptics: esp_timer player, M1/M2 dash sweep, system tones, 9 reading speeds | done; per-motor on-time counters (v0.5.1); generator playback for `bench motor` (v0.6.0) |
| Key: interrupt timestamps, debounce, holds 2 / 3 / 5 s with feedback | done |
| Decoder + adaptive calibration (PARIS, rollback, health), SOS by shape | done |
| Command mode with echo: `S P A N Q R L E B C Z X F D O G`, **`W`** (v0.6.1) | done (`V` is Phase 2 and gives an error) |
| LoRa radio: SX1276 **and** SX1262 drivers, chip detected at boot | done; SF 9–12 at run time, 22 dBm default (v0.6.0); **SF12 / 22 dBm ran 920 min on both boards (19 Oct)** |
| Packet layer, duty cycle, duplicates, message IDs | done |
| Link protocol: connect / accept / decline / BUSY queue, keepalives with reports, two paths, receipts, UNPAIR | done (v0.6.0's fix works in the field: `EV LOST ... no-receipt`, `EV LORA ... they-cant-hear / lost`, `EV RESTORED` seen); **v0.6.1: test messages may go while the link is lost (`link_send_test_msg`)** |
| SOS over the air, relayed by boards in between (up to two hops) | done |
| Power switch (`F` off, hold 2 s on); quiet replies in a conversation | done (v0.3.4); the key's pull-up set again for deep sleep (v0.6.1) |
| Battery %, low-battery warnings, charging indication | done (the % is a generic voltage table, §4) |
| Discovery `D`: sweep, near / mid / far list, invite, silent mode, answer limits | done (v0.3.5); thresholds still placeholders (§3.7) |
| BLE link to the phone: NUS, `O` window + bonding, lines, relay both ways | done; **protocol 3** (v0.6.1: `BENCH POWER` / `STOP`, `BN`, `EV BLE_OFF`, non-connectable advertising) |
| Phone app (Flutter, Android) | v0.2.0 built and running on both phones (16 Oct); **v0.2.1 written (20 Oct), not yet built by the owner** (§5A) |
| Test mode (TEST_PLAN §11) | done; **`point` (v0.6.1): plans of 1–8 steps, temporary settings, `STEP` lines** |
| Run-down logs (TEST_PLAN §14.4) | done (v0.5.1); `sf` in the meta (v0.6.0) |
| Bench tests `bench power` / `bench motor` (TEST_PLAN §14, §16.4, **§17.7**) | done; **v0.6.1: new steps (1b, 1c, 3a–3d), started from the app, signals on M1, no USB time limit**; untried on the boards |
| User manual `docs/MANUAL.md` = the app's Manual page (`tools/gen_manual.py`) | done (v0.6.1: `W`, the point test, the meter session) |
| Analysis `tools/analyse_tests.py` | tables; lost periods + availability; by SF / power; size bands; first try; RXBAD; run-down dumps, slopes, A-curve mapping (v0.6.0); **point tests: points.csv, the per-step table, attenuation, the path-loss exponent (v0.6.1)**. **No graphs, meter table or phyphox reader yet** |

**Verification (v0.6.1):** builds warning-free on ESP-IDF v6.1 (binary
0xa7970, 55 % of the app partition free); **44,170 host checks** pass; the
protocol soak (150 seeds × 4 h: 44,890 checks) passes; **63/63 QEMU steps** (new: a
2-step point test, `W`, the meter session from the pretend phone); the
whole QEMU suite ran on the wearable build (key on GPIO14). **The app:**
syntax-checked (tree-sitter), read function by function, new unit tests,
**not compiled** (§6.2). Details: §5A.6.

### 2.2 Version history

| Version | Date | What |
|---|---|---|
| v0.1–v0.2 | 1–6 Oct | everything up to command mode; loopback stand-in for the radio; v0.2 added the test setup |
| v0.3 | 7 Oct | the radio step: SX1276 driver, packets, link protocol, receipts, SOS over the air |
| v0.3.1–v0.3.3 | 8 Oct | AXP2101 (V1.2), SX1262 driver, SX1262 start-up fix. Owner: "It works" |
| v0.3.4 | 8 Oct | the owner's additions: power switch, quiet replies |
| v0.3.5–v0.3.6 | 13 Oct | discovery `D`; loopback never saved |
| v0.4.0 | 13 Oct | the BLE link: `ble.c`, `phone.c`, relay, pretend phone |
| v0.5.0 + app v0.1.0 | 13 Oct | test mode (`ttest.c`, `ntest.c`); the Flutter app |
| (no firmware change) | 14 Oct | app built on both phones; desk session; `tools/analyse_tests.py` |
| v0.5.1 | 14 Oct (late) | power and motor tests (TEST_PLAN §14): `rundown-m`, `test dump` header + `m1_ms` / `m2_ms`, run-down end reason |
| (no firmware change) | 15 Oct | relay night (12 h), mall session; connection-lost gaps found; handover with the next update specified |
| v0.6.0 + app v0.2.0 | 15–16 Oct | the connection-lost fix (paths, reports, every loss with its reason), range level `G` / SF 9–12, 22 dBm, bench tests, `rundown-l`, phone protocol 2; app: paths, radio, command box, manual, start checks, Samsung-friendly Bluetooth, background service; analysis extended; MANUAL.md, SPEC v1.7, TEST_PLAN §16 |
| (no firmware change) | 16–20 Oct | v0.6.0 and app v0.2.0 built and running; run-down A (DEA only), Dynamic 2–4, run-down L; the motor circuit (330 Ω); the addendum specifying v0.6.1 |
| **v0.6.1 + app v0.2.1** | 20 Oct | the key on GPIO14 and the wearable build by default; point tests with sweeps; the meter session from the app (steps 1b, 1c, 3a–3d; signals on M1); `W`; phone protocol 3; app: point tests, the meter page, the base check, the resent stop, the offline warning; analysis: point tests; TEST_PLAN §17, SPEC v1.8 |
| **v0.6.2 + app v0.2.2** | 21 Oct | the learning view (phone protocol 4: `KY`, `KC`, `CB`, `SP`, `PL`); `I` 1-5 (DCDC1 3.0–3.4 V); app: patterns above the letters, the letter being keyed, the timing bar, the speeds, now playing, the Learn page (PARIS helper, reading-by-feel modes, Morse chart); SPEC v1.9 (§0g); §5B |

### 2.3 Field testing so far (details: TEST_PLAN §13, §15, §17)

- **14 Oct desk run** (baseline, 1 m): 239 / 239 on both paths.
- **14 → 15 Oct, relay night** (run-down B with paths = both, 2 m apart,
  12 h, SF9 17 dBm): 5,760 messages per path, all delivered; LoRa 99.6 %
  first try; relay median 0.85–0.91 s; **−13.2 (ABC) / −14.0 (DEA) mV/h**
  after the first 30 min.
- **15 Oct, mall session** (SF9 17 dBm, 65 min, ~400 m into a mall): LoRa
  down indoors (12.7–25.3 and 30.0–59.5 min), RSSI −110 to −116 dBm, SNR to
  −14 dB; relay 99 %; no `LINK state=lost` logged: the bug fixed in v0.6.0.
- **15 → 16 Oct, run-down A:** only DEA ran it (the base phone's MQTT
  dropped 25 s before Start and stayed down, so ABC never started; its dump
  showed the relay-night log, as only starting a run-down wipes it). DEA:
  900 min, 4159 → 3980 mV, **−10.0 mV/h** after the first 30 min,
  `end=none` (opening the serial monitor restarted it). Not to empty.
- **18 Oct, Dynamic 2–4** (`ML_20261018-184526_*`, `-194848_*`,
  `-215100_rover_DEA`):
  - 18:45, SF9 / 22 dBm, walking, urban: RSSI median −104 / −108 dBm (the
    mall walk at 17 dBm: −110 / −112), LoRa up ~50–53 %, indoors 11–14 %.
  - 19:48, SF9 / 22 dBm, car, urban: LoRa 97 % delivered within 250 m, 0 %
    from 500 m (the "cliff"); **relay only 16–38 %: the base phone's MQTT
    reconnected 108 times**; the rover's Stop was dropped (rover offline),
    so the base board ran 21 min longer.
  - 21:51, SF12 / 22 dBm: **not a valid test** (the base phone never got the
    start; ABC wasn't in test mode); 23 messages in 42 min, 35 of it indoors.
  - The owner's impression: higher power and SF12 "didn't feel like they're
    doing much". Explained: +5 dB moves the edge out by maybe a third in
    town; one building costs 10–30 dB; the SF12 run proves nothing either
    way. Hence the point tests (TEST_PLAN §17).
- **19 Oct, run-down L** (SF12, 22 dBm, LoRa only, Bluetooth off), both
  boards, 920 min: **−13.1 (ABC) / −13.4 (DEA) mV/h**: as much as run-down
  B, about 30 % more than A. LoRa traffic at SF12 / 22 dBm is expensive; the
  meter session measures the 22 dBm transmit current.
- **The phones' internet is the weak point** (three lost runs: the A night,
  Dynamic 3, the SF12 run). Advice given: the base phone on Wi-Fi and its
  charger, MorseLink's battery Unrestricted, and don't leave until the
  rover's test page says "Base phone: running". v0.2.1 checks it (§5A.5).
- **Not yet done:** run-down A to empty (DEA), the point tests (TEST_PLAN
  §17.3–17.5), the meter session (§17.7), phyphox (`bench motor m1`, then
  `m2`, optionally both for 3 s; §14.3), the motor night (`rundown-m`; not in
  the plan to 26 Oct: ask if there's time), optionally a fair SF12 dynamic
  walk on the Dynamic 2 route with the base phone on Wi-Fi.

### 2.4 Loose ends (ask once)

- **Defaults taken in this chat** (say so, accept a change):
  1. `W`'s off is **stored**: it survives restarts until `W` again (or `O`).
  2. **The point test's gaps** are 15 / 20 / 25 / 40 s at SF9 / 10 / 11 / 12,
     not the addendum's 10 / 15 / 20 / 30 s: a 25-character message that
     fails takes up to 11.8 / 13.5 / 19.8 / 33.0 s to give up, and the
     engine fails every message waiting behind it, so with the shorter gaps
     one failure became two. The default sweep takes **25 min 10 s** (not
     ~20). Told the owner on 20 Oct, no answer yet. If he wants it shorter:
     a 2-step plan at the obstacles (SF9 / 22, SF12 / 22: 19 min 45 s), or
     fewer messages (`n=15`: 19 min 20 s; the console takes `n=`, the app
     always runs 20: a small app change if wanted, `pointMessages` in
     `protocol.dart` and `n=` in `_startLine()`, both phones updated).
- **Defaults taken earlier and still standing:** transmit power is a console
  setting (no device command); SOS on the shared SF (no SF9 copy; future
  work, about a day); the app's background notification on.
- **The meter session's date:** the owner will say (campus meter).
- **The S10e** (it kept dropping on 15 Oct: "phone disconnected (reason
  0x213)", the phone hanging up; advised: unpair MorseLink-DEA, Bluetooth
  off / on, MorseLink's battery Unrestricted and out of "Sleeping apps",
  then `O` and pair again): no news since; ask only if it comes up.
- **KiCad:** if the owner's schematic still has the key on GPIO4, it needs
  GPIO14 for the report (his call).
- The owner's discovery path-loss measurements for `DISC_NEAR_LOSS_DB` /
  `DISC_FAR_LOSS_DB` (13 Oct, "I'll check it later"). Don't push.

### 2.5 Next (plan to 26 October; confirm with the owner)

| When | What |
|---|---|
| 20–21 Oct | **owner:** wire the key to GPIO14; build and flash v0.6.1 on **both** boards, app v0.2.1 on both phones; quick checks (below). Run-down A to empty on DEA (≈ 2–2.5 days; console start, don't open the monitor until it is off): **either now** (it ends before the point tests need DEA) **or after the point tests** (while he writes) |
| 21–23 Oct | **owner:** the point tests (TEST_PLAN §17.3–17.5: 5 obstacle pairs + the ladder at 50 / 100 / 200 / 400 m, 14 sweeps, ~7 h with the moving: two sessions), the meter session (§17.7), phyphox (§14.3) |
| 22–24 Oct | **analysis:** `analyse_tests.py` on the point data (points.csv, attenuation, n); **graphs** (still to write; SVG with the standard library, or matplotlib if he installs it): RSSI against distance per setting, the loss per obstacle, availability over time, battery curves (A, B, L); the meter table (the §17.7 sheet → power per state, battery life per use mix); the phyphox reader (§14.3) |
| 24–26 Oct | the report (the owner writes): numbers, tables, figures, the limitations (TEST_PLAN §8, §14.2) |

**Quick checks after flashing** (README §6 style, ~15 min): the boot line
says `key=GPIO14, internal pull-up`; key a few letters; `W` → OK + OFF and
the app says "Bluetooth switched off on the board (W)", `W` again → OK + ON
and the app reconnects; a desk point test (Single, SF9 / 17 dBm: 5 min
25 s) shows its steps and ends with the board's own setting back
(`status`); Tests → Meter session on battery (M1 buzzes "1" first), then
a 5 s hold on the key in step 1 stops it (the board's Bluetooth is off
then, so the app's Stop can't reach it).

---

## 3. How it is put together

### 3.1 Firmware: tasks and data flow

```
keyer task (core 1) --key events--> UI task (core 1) --net_* calls--> net task (core 0)
                                       ^   |                              |  link engine
haptics player (esp_timer) --done-->---|   +--plays--> haptics            |  + radio / loopback
console task --commands-------------->-|                                  |  + relay_to_phone()
                                       +<---------- ui_post_net(events) --+  + ntest (test mode)
                                       |   +--phone lines (ble_send_line)--+       | RL / T lines
                                       |   v                               v       v
            ui_post_phone(UIP_*) ------+  ble_tx task (core 0) --notify--> phone (NUS TX)
                                          NimBLE host task (core 0) <--write-- phone (NUS RX)
                                             | HI / TEST / BENCH -> ui, RL -> net_phone_rx()
```

- **UI task** (`ui.c`) owns everything the user sees: state machine, draft,
  command buffer (`U.cmd`), contacts, alerts, settings, the link state *as
  the user sees it* (`U.link_state`), the test-mode flag and the bench
  sessions. Other tasks talk to it only through its event queue. It writes
  the phone's view (HI / ST / DR / TX / OK / NO / RX / EV / BN lines);
  `phone_sync()` runs after every event and every 250 ms.
- **Net task** (`net.c`) owns the link engine and the radio. The UI calls
  `net_*()` functions, which only queue a command; results come back as
  events (`LEV_*`, `link.h`), passed through `deliver_events()`, where
  test mode may take them first (`ntest_on_event()`; returns true = the UI
  never sees it). Every packet it transmits is also offered to the phone
  (`relay_to_phone()`; off during a test); packets from the phone enter the
  engine through `link_on_rx_via(..., via_phone = true)` (never during a
  test). A point test changes the radio through `test_set_radio()`
  (`want_sf` / `want_dbm`, applied between packets, never saved).
- **BLE** (`ble.c`): NimBLE's host task runs the GAP / GATT callbacks; the
  `ble_tx` task sends queued lines.
- **Pure modules** (no ESP-IDF; compiled by the host tests too): `morse`,
  `hseq`, `calib`, `decoder`, `peers`, `proto`, `link`, `convo`, `disc`,
  `phone`, `ttest`, `bench`.
- **Start-up order** (`app_main.c`): outputs off (and the LED's pin dark) →
  GPIO interrupt service → NVS → power chip → power switch check → haptics →
  net → BLE line queue → UI → key → console.

### 3.2 Files

| File | Lines | Role |
|---|---|---|
| `board.h`, `config.h` | 117, 251 | pins + **build switches** (`USE_ONBOARD_BUTTON`, `USE_ONBOARD_LED`, `VERBOSE_LOG`) and their compile checks (strapping pins, RTC-capable key); every tunable constant; `MORSELINK_VERSION` "0.6.1" |
| `ui.c` | 3994 | the brain: holds, compose, send, command mode (incl. `G`, **`W`**: `ble_switch()`, `ble_wanted()`), alerts, SOS, discovery walk, the link as the user sees it, the phone's view, test-mode silence, `rundown-m`'s motor loop, **bench power** (`bench_power_start / apply / timers / undo / stop`, from the console or the phone, `BN` lines) / **bench motor** |
| `link.c/h` | 2095, 556 | protocol engine (pure): requests, BUSY queue, keepalives with reports, paths and their states, losses with reasons, receipts, retries, SOS relay, discovery sweep, `link_timing()` per SF, **`link_send_test_msg()`** |
| `proto.c/h` | 418, 223 | packet format, airtime (`lora_airtime_ms`, any SF / LDRO), duty-cycle buckets, duplicate ring, message-ID blocks (pure) |
| `net.c/h` | 1356, 160 | the net task: engine ↔ radio, listen-before-talk, loopback pretend partner (`sim deaf`), packet log, phone relay, test hooks (**`test_set_radio()`**), SF changes (`net_set_sf`), bench radio states (`net_bench`) |
| `ble.c/h` | 890, 106 | Bluetooth LE: NimBLE NUS service, advertising (**`ble_set_connectable()`**), `O` window, bonding rules, **`ble_flush()`**, `BENCH` lines to the UI |
| `phone.c/h` | 447, 256 | the phone's line protocol (pure); **`phone.h` is the contract** (protocol 3) |
| `ttest.c/h` | 487, 219 | test traffic (pure): tests incl. `rundown-m`, `rundown-l`, **`point`** (plan parsing, `tt_plan_text`, the timetable `tt_point_*`), the pace per SF, test texts |
| `ntest.c/h` | 976, 71 | test mode in the net task: copies, receipts, skips, `T,` lines (incl. `LINK why`, `PATH`, **`STEP`**), STAT, run-down samples; **the point test's steps (`pt_switch`, `pt_restore`, held messages)** |
| `bench.c/h` | 190, 131 | the bench tests (pure): `bench power`'s steps (**ids 1 … 6, kinds, `bench_signal_ms`, `bench_power_ms`, `bench_skips`**), `bench motor`'s pattern generator |
| `radio.c/h`, `radio_sx1262.c/h` | 766, 779 | SX1276 front end and the SX1262 back end; `radio_set_sf()`, `radio_tx_dbm_max()`, the bench states (standby, listen, carrier) |
| `pmic.c/h`, `power.c/h` | 354, 224 | AXP192 / AXP2101 rails and battery (`battery_percent_from_mv`); the on/off switch (**`sleep_pulls()`**: the key's and the LED's pull-ups for deep sleep) |
| `convo.c/h`, `disc.c/h` | 32, 215 | "are we in a conversation?"; discovery list and answer limits (pure) |
| `haptics.c/h`, `hseq.c/h`, `keyer.c/h` | 451, 277, 165 | the player (+ on-time counters, generator playback, `haptics_led`, `haptics_raw`); job → segments; key |
| `decoder.c/h`, `calib.c/h`, `morse.c/h`, `peers.c/h` | 389, 475, 197, 136 | keying → characters; calibration; Morse table (ITU + extras); contacts |
| `store.c/h` | 560, 168 | NVS: ID, settings (v3: `sf`), calibration, contacts, message IDs, power switch, `ble`, **`bleoff` (W)**, run-down log (meta v2: `sf`) |
| `console.c/h` | 780 | serial console: key injection, `play`, `motor`, `speed`, `haptic`, `radio power\|sf\|ping\|log`, `sim`, `phone` (**`on\|off`**), `test start\|stop\|dump`, `bench power\|motor\|stop` |
| `app_main.c` | 129 | start-up order; the LED's pin in the wearable build |
| `test/host/` | ~4050 | keying simulator, packet layer, protocol scenarios (incl. the paths, **sending while lost, the point gaps against a failing message**), soak, discovery, phone lines + relay, test schedules and pace (**the point timetable**), bench |
| `tools/qemu_test.py` | 1014 | end-to-end test in the ESP32 emulator (63 steps, ~24 min) |
| `tools/analyse_tests.py` | 1164 | the phones' session files and the run-down dumps → summary.txt, messages.csv, sessions.csv, lost.csv, rundowns.csv, **points.csv** |
| `tools/gen_manual.py` | 147 | `docs/MANUAL.md` → the app's `lib/manual.dart`, with checks against `morse.c` and `cmd_valid()` |
| `docs/MANUAL.md` | 379 | the user manual (also the app's Manual page) |

### 3.3 Code conventions

- Constants live in `config.h` (behaviour) and `board.h` (pins), with a
  comment saying where the value came from: (spec) or (agreed + date).
- New logic goes into a pure module with host tests where possible; the
  ESP-side files stay thin.
- **Comment standard** (the owner's): every function and every non-obvious
  line. Five older files still have only summary comments: `calib.c`,
  `decoder.c`, `hseq.c`, `morse.c`, `peers.c`. Bring each up to standard
  when you touch it.
- **Build switches** (`board.h`, independent since v0.6.1):
  `USE_ONBOARD_BUTTON` (1: the key is the board's IO38 button; 0: the
  external key on GPIO14), `USE_ONBOARD_LED` (1: the LED mirrors the motors;
  0: the LED is dark and its pin held so), `VERBOSE_LOG` (the packet and
  playback logs; the console's `radio log` / `verbose` switch them at run
  time). **Default: 0 / 0 / 1, the wearable build.** Pins: M1 = GPIO25, M2
  = GPIO13, key = GPIO14. `board.h` refuses a key on a strapping pin or a
  pin that can't wake the chip.
- New tests go at the **end** of `tt_test_t` (the run-down log stores the
  number).

### 3.4 BLE link as built (v0.4.0; protocol 3 in v0.6.1)

- **NimBLE, Nordic UART Service** (6E400001-…; RX …0002 encrypted only; TX
  …0003 notify), name `MorseLink-<ID>`. One connection. MTU 256.
- **Accepting a phone** (`ble.c`, top comment): outside the `O` window a
  known bond is accepted and a new one deleted and dropped; inside it, a new
  phone is accepted and replaces the others. 3 bond slots.
- **Lines** (`phone.h`): HI / ST / DR / TX / OK / NO / RX / EV / RL / RD /
  CM / CE / **BN** / `T,` to the phone; HI / RL / TEST START / TEST STOP /
  **BENCH POWER / BENCH STOP** from it. **The app tolerates extra trailing
  fields** on ST (≥ 4) and EV (≥ 2) and ignores unknown line tags, and the
  board ignores unknown lines, so new fields at the end and new tags are
  backward-compatible.
- **Non-connectable advertising** (v0.6.1, the meter session's step 3a):
  `ble_set_connectable(false)` restarts the advertisement with
  `BLE_GAP_CONN_MODE_NON`; `true` puts it back.

### 3.5 The app as built (v0.2.1, `morselink_app`)

| File | Role |
|---|---|
| `lib/app_state.dart` | everything the app knows and does (one `ChangeNotifier`): `_onDeviceLine` (HI / ST / RD / CM / CE / DR / TX / OK / NO / RX / EV / RL / **BN**), chat, events, SOS; the relay; the radio exchange with the partner's phone; test sessions (`startChecks`, rover start, base follows over MQTT; **point tests: `TestMeta` label / sweep / plan, per-step counts, the `pt` ctl message**), **the base check, the resent stop (`_stopBase`, `_PendingStop`), the offline warning**; **the meter session (`BenchRun`, `startBench` / `stopBench` / `clearBench`, `_onBench`, `_benchTick`)**; `W` (`bleOffByW`); GPS, labels, logs; the background service's text |
| `lib/bench.dart` | **new:** the meter session's steps and timetable (the same as `bench.c`), `benchAt()` (where the session is) |
| `lib/ble_link.dart` | flutter_blue_plus: connect (wait for pairing, MTU after), reconnect with back-off (**`slow`: 30 s while the board's Bluetooth is off by W; `kick()`**), lines in and out, why it failed (`bleCodeText`) |
| `lib/background.dart` | the foreground service (in `MainActivity.kt`) over the `morselink/service` channel |
| `lib/relay.dart` | mqtt_client: `broker.hivemq.com:1883`, prefix `morselink-za7q` |
| `lib/protocol.dart` | the board's lines and their wording: `RadioInfo`, `pathStateText`, `eventText`, `commandNames` (**`W`**), `commandEndText`, `refusalText`, `testPaceText`; `TestLine.parse()` (text fields in `_textFields`, **incl. `plan`**); **the point test's arithmetic (`PlanStep`, `defaultPlan`, `planText`, `parsePlan`, `pointStepS`, `planSeconds`: the firmware's numbers)** |
| `lib/storage.dart` | settings JSON (incl. `background`); JSON-lines logs in `Download/MorseLink` |
| `lib/manual.dart`, `lib/markdown_lite.dart` | the manual (generated, §3.2) and its Markdown reader |
| `lib/ui/home_page.dart` | status (Bluetooth + why, internet, partner + link, battery, LoRa / Relay rows, radio), test, SOS and **meter-session** banners, the keying box (draft, or COMMAND), chat |
| `lib/ui/manual_page.dart` | the manual, one tile per section |
| `lib/ui/connect_page.dart`, `settings_page.dart` | scan `MorseLink*`; settings (incl. the background switch) |
| `lib/ui/test_page.dart` | test set-up (6 tests incl. **`point`**: label, Sweep / Single, duration; paths default `both`), the start dialog, running view (losses, LoRa path; **the point test's step, counts per step, "We hear them" / "They hear us"; the red base banner**), labels, **Repeat at the next spot, the Meter session button** |
| `lib/ui/meter_page.dart` | **new:** the meter session: checklist, Start, the step now, what to do, time left, "read the meter now", next; every step listed |
| `android/.../MainActivity.kt` | keeps the screen on; hands the app its folders; `KeepAlive` + `KeepAliveService` |
| `android/.../AndroidManifest.xml` | permissions (+ `FOREGROUND_SERVICE_CONNECTED_DEVICE`), the service |

Plugins: flutter_blue_plus ^1.35 (resolves to 1.36.8), mqtt_client ^10,
geolocator ^13. The zip holds only the app's own files (§6.1).

### 3.6 Adding a test type: the touchpoints

Firmware: `ttest.h/.c` (names, parse, `tt_sends` / `tt_is_rundown` /
`tt_plays` / `tt_ble_off`, schedule; host-tested in
`test/host/test_ttest.c`); `ntest.c` (`ntest_start()` and preconditions,
`generate()`, results, hooks, STAT, run-down samples via `rd_begin()` /
`rd_sample()` / `rd_save()`, `emit()`); `ui.c` (`on_test()`,
`test_timers()`, `test_motor_play()`, the 5 s hold; CON_TX_POWER / CON_SF
refused during tests); `console.c`; `phone.h/.c`; `tools/qemu_test.py`.
**A longer parameter line** needs `TT_ARGS_MAX` (160 since v0.6.1), which
sizes the UI event's text, the net command, the console buffer and
`PHONE_ARGS_MAX`; a longer `T,` line needs `NT_LINE_MAX` (224). **A `T,`
value with commas must be written another way** (the plan uses `/`): the
fields are comma-separated (§7).
App: `lib/ui/test_page.dart` (`_testNames`, `_describe()`, `_paths`);
`lib/app_state.dart` (`testSends` / `testUsesLora` / `testUsesRelay`,
`_blocking()`, `startChecks()`, `TestMeta` (+ `copy()`), `_startLine()`,
`_startBase()`, `_onTestLine()`); `lib/protocol.dart` (`testPaceText()`;
**every new text-valued `T,` field into `_textFields`**, or "2E5" is logged
as a number). Manual: `docs/MANUAL.md` (then `tools/gen_manual.py`).
Analysis: `tools/analyse_tests.py` (`messages()`, `summarise()`,
`lost_periods()`, `point_report()`, `MESSAGE_FIELDS` / `SESSION_FIELDS` /
`POINT_FIELDS`).

### 3.7 Discovery as built (v0.3.5)

`link_discover()` broadcasts `DISCOVER` (16 slots; **slot length computed
from PRESENCE's airtime** in `disc_slot_ms()`, so it scales with SF); every
discoverable device answers `PRESENCE`; `LEV_DISC_DONE` carries the list
(≤ 12), sorted near / mid / far by path loss. `DISC_NEAR_LOSS_DB` 80 /
`DISC_FAR_LOSS_DB` 105 are placeholders (path loss = the sender's power −
the RSSI, so the power setting doesn't move it; only in QEMU, whose pretend
RSSI is fixed, do the losses read 5 dB higher at 22 dBm). The UI's safety
timeout follows the SF (`link_disc_window_ms()`).

---

## 4. Behaviour worth knowing

All of this is in SPEC (§0b, §0a, §7, §8, §10) and TEST_PLAN §11, §17.

**Linking (walkie-talkie model):**
- A device stays linked to its last partner. Every connection is asked for
  (`S` contact or ID, `P` new ID) and needs the other side to accept.
- **Busy** = a message on the link in the last 10 min: a request then gets
  BUSY and waits in a queue (at most 3).
- Keepalive every 30 s (45 s at SF11, 60 s at SF12), each with the
  sender's report of when it last heard us on each path; a path is
  connected only with proof both ways; the link is **lost** when no path is
  (error + ID; once, with its reason), **restored** when one is again (OK +
  ID). LoRa alone dropping while the relay carries plays nothing (§5.1).
- Messages: every one gets a **silent receipt**; OK plays when it arrives.
  No receipt after the retries (5 copies, ~10 s at SF9): error + ID, the
  text goes back into the draft, and the link is lost (no-receipt; no
  second alert) until the next fresh report. **The engine fails every
  message queued behind a failing one** (they'd go the same way), which is
  what the point test's gaps are sized against (§2.4).
- **Sending is refused while the UI thinks the link is lost** (error + ID,
  draft kept). Only the point test sends while lost
  (`link_send_test_msg()`), never the user.
- **Quiet replies:** a message plays without attention + ID when the last
  exchange with that device was at most 5 min ago.

**SOS:** recognised anywhere; 3 s silence confirms; resent every 30 s for
10 min, then every 2 min; 5 s hold cancels. Relayed by boards in between
(hop limit 2, duty-cycle exempt).

**Power switch:** `F` → off (deep sleep); hold the key 2 s → starts. "Off"
is stored: a reset, a flash or USB power keeps it off; **only the key wakes
it** (in the wearable build the key on GPIO14, §2.0).

**Radio:** 869.525 MHz, BW 125 kHz, **SF9–12** (`G 1-4`, stored; default
9), CR 4/5, sync word 0x5E, **22 dBm** default (`radio power 2-22`; SX1276
20), 10 % duty cycle per rolling hour (SOS exempt), listen-before-talk up to
max(1.5 s, the longest packet + 0.3 s). Airtime, SF9 / 10 / 11 / 12:
keepalive 0.14 / 0.29 / 0.58 / 1.16 s; a 100-character message 0.6 / 1.1 /
2.4 / 4.3 s. Decodes down to SNR −12.5 / −15 / −17.5 / −20 dB.

**Battery %:** `battery_percent_from_mv()` (`pmic.c`): a generic Li-ion
table, 4180 mV = 100 %, 3300 mV = 0 %. A fully charged cell under the
board's ~60 mA load reads ~4157 mV = **97 %** (owner asked; normal). In
practice the board stops near 3.3–3.4 V (the regulator can't hold 3.3 V);
"empty" for the report = the last run-down sample. Report mV, not %.

**Phone:** Bluetooth is off until the first `O`; after a phone has paired
it starts at every boot, **unless `W` switched it off** (stored; `W` or `O`
turns it on again; `status` says `phone: BLE off (W)`). Before going off
the board tells the phone (`EV BLE_OFF`), and the app then tries only every
30 s. Outside tests, every packet this device sends to one device is also
handed to the phone; packets arriving through the phone prove the **relay**
path only (v0.6.0).

**Test mode** (TEST_PLAN §11): started from the rover phone (base follows
over MQTT) or the console; both boards linked and the link up; nothing
plays (except `rundown-m`); the key does nothing except the 5 s hold. Each
test message goes as an L copy (LoRa, real retries) and an R copy (phones).
Skips: `lost`, `duty` (≥ 8 % airtime), `queue`, `nophone`. Packets from the
phone never reach the link engine during a test. `radio sf` / `radio power`
are refused during a test (v0.6.1).

**Point test** (v0.6.1, TEST_PLAN §17.2): LoRa only; both boards run the
same plan on their own clocks (no coordination packets; step i starts at
START + the plan's offset). Each step: the setting applied for the test
only (a `STEP` line), the settle time (10 s at SF9–10, 20 s at SF11–12),
then 20 messages, one every gap (15 / 20 / 25 / 40 s), the base half a gap
after the rover. The L copy goes **even while the link is lost**; a message
due while the one before still waits for its result is held and goes when
that one settles (`late_ms`), or is dropped as `lora_skip=late` by the next
one or a step change, so every message is counted. At the end, a Stop or a
5 s hold: the board's own setting back (`STEP,i=0,...,restored=1`).
Default sweep SF9 / 17, SF9 / 22, SF12 / 22 dBm: **25 min 10 s, ~95 s of
airtime per board** (two sweeps an hour: 5.3 %).

**Meter session** (v0.6.1, TEST_PLAN §17.7): started from the phone (no
USB) or the console (`bench power`: waits until USB is unplugged, no time
limit). 16 steps, **543.8 s** from step 1's signal to step 6 (532.7 s
without 22 dBm), plus 5 s to settle; M1 buzzes each new step number (0.3 s
on / off, then 2 s), one 0.1 s buzz between the parts of a step. A 5 s hold
stops it; at the end the board puts back its Bluetooth setting and SF.

**Run-down logs (v0.5.1, TEST_PLAN §14.4):** `test dump` prints `#
morselink-rundown board=... test=... paths=... fw=... tx_dbm=... speed=...
unit_ms=... m1_pct=... m2_pct=... motor_log=... end=... samples=...`, a
comment line, then `minute,mv,charging,m1_ms,m2_ms`. Starting a run-down
wipes the last log. Opening the serial monitor restarts the board (DTR);
the log survives. The owner captures dumps with the monitor's file logging
(**Ctrl+T, Ctrl+L**; VS Code's terminal keeps only 1,000 lines).

---

## 5. The update of 16 October as built (v0.6.0 + app v0.2.0)

What the 15 Oct handover specified, as it was built. Exact behaviour: SPEC
§0e, TEST_PLAN §16, `docs/MANUAL.md`; the code's comments say why. (v0.6.1
changed the bench steps (§5A.3) and nothing else here.)

### 5.0 The owner's decisions (15–16 Oct)

| Question | Answer |
|---|---|
| What "connected" means while the phones relay | **Two statuses** (owner: "one says LoRa connected, the other says Relay connected"); the link counts as up while either works |
| The alert when only LoRa drops | **none** (option "1. a"): the usual error + ID only when both paths are down; the app shows LoRa's state |
| The range command | **`G 1-4` = SF9–SF12**; power separate (`radio power`) |
| Changing SF on both boards | **each board by hand**, and a note at `G` in the manual (no coordinated switch) |
| One update or two | **one firmware + one app update**, the fix built and verified first |
| LoRa-only run-down | **include it** (`rundown-l`) |
| Power default | **max power by default**: 22 dBm on the SX1262 (17 → 22 once); lower levels stay on the console (default taken, §2.4) |
| SOS at SF9 | advised against (§2.4); built: SOS on the shared SF |
| Background notification | default yes (§2.4) |
| Meter session date | the owner will say |

### 5.1 The connection-lost fix

**The bug** (mall session, 15 Oct; the owner: "when a device doesn't receive
a heartbeat signal back after a few times it still isn't aware that the
other device's connection is lost"), traced in v0.5.1: (1) a failed message
set the link lost **without an event**; (2) the 90 s check only ran while the
link was up, so after (1) nothing was ever reported; (3) packets through the
phones counted as hearing the partner, so a dead LoRa link never showed
while both phones relayed; (4) in test mode the UI never heard of (1), so
the ST line kept saying connected; (5) keepalives were one-way, so a board
that heard its partner couldn't know the partner didn't hear it.

**As built** (`link.c/h`; host-tested first):
- **Paths** `LP_LORA`, `LP_RELAY`, each with a state (`path_state_t`):
  none, checking (just started: a new link, the phone just came, the SF
  changed), connected, they-cant-hear, cant-hear-them, lost, off (the relay
  with no phone, or during a test). `link_path_state()`, `LEV_PATH` on every
  change (arg = path << 4 | state).
- **Keepalive report** (`KA_REPORT_LEN` 2): seconds since this board last
  heard the partner on LoRa, and on the relay (255 = never / > 254 s). A
  path is **connected** only when we heard the partner on it within the lost
  time **and** a fresh report (newer than the last failure) says they heard
  us on it. A v0.5.x partner sends no report: then hearing counts as proof
  (the old rule), and `status` says "no report: older firmware".
- **The link** is up while any path is connected (or the link was up and a
  path is still checking); it goes lost when none is, **once**, with
  `LEV_LINK_LOST` arg = `LOST_SILENCE`, `LOST_NO_RECEIPT` or `LOST_ONE_WAY`.
  A failed message (no receipt by any path) sets `fail_at`: earlier
  reports no longer prove anything, so the link goes lost at once
  (no-receipt) and comes back with the next fresh report. Restored
  (`LEV_LINK_RESTORED`) when a path is connected again.
- **Quick keepalives** (`ka_soon`: 0.2–1.2 s, at most one per
  `quick_ka_gap_ms`) when a path turns they-can't-hear or connected, so the
  partner learns at once; scheduled keepalives go out whatever else is sent.
- **UI** (`ui.c`): `LEV_LINK_LOST` → state lost, `EV LOST <ID> <why>`, the
  error + ID alert unless no-receipt (the failure's own error said it), log
  "connection to X lost (reason)". `LEV_PATH` → nothing plays; `EV LORA` /
  `EV RELAY <ID> <state>` for changes that mean something (going down,
  one-way, back; not checking → connected, nor the relay going off with the
  phone); log "LoRa to X: state". `LEV_MSG_FAILED` no longer touches the
  link state.
- **Net** (`net.c`): `link_set_relay(eng, phone ready && no test)` every
  loop; `link_set_lora()` when the SF changes (both engines in loopback).
- **Test mode** (`ntest.c`): `LINK,state=lost,why=...,heard_ago_ms=...`,
  `PATH,path=L|R,state=...,heard_s=...,they_s=...`; the event goes on to the
  UI, so the phone's ST changes; a LoRa test is refused (`lora-lost`) while
  LoRa itself is down.
- **Host scenarios** (`test/host/test_link.c`): fail then silence (the
  15 Oct case: exactly one lost, no-receipt), one-way with and without the
  relay, the relay dropping, reports by hand, an older partner, SF12, the
  timing table; the soak (150 seeds × 4 h) passes.
- **The data already collected** (v0.5.1): the analysis rebuilds the losses
  (§5.5): down from a failed L copy (`FAIL path=L why=no-receipt`) or the
  lost time after the last packet heard, until the next `LINK
  state=restored`.

### 5.2 Range settings as built

- **`G [1-4]`** in command mode (`ui.c`): `G n` sets SF 8 + n, saves it,
  `net_set_sf()`, plays OK, logs "range level n = SFx. Set the SAME level on
  <partner> ..."; `G` alone plays the level digit. Console `radio sf 9-12`
  does the same. Stored in the settings (`SETTINGS_VERSION` 3, `sf` in a
  former reserved byte; the v2 → v3 migration sets SF9 and **22 dBm**).
  Applied at boot.
- **Radio** (`radio.c`, `radio_sx1262.c`): `radio_set_sf()` reprograms the
  modem in standby (LDRO at SF11/12) and resumes receiving;
  `radio_tx_dbm_max()` = 22 on the SX1262, 20 on the SX1276; power is
  clamped to the chip's maximum.
- **Timing** (`link_timing(lp, &t)`, from the airtime at the SF): keepalive
  / lost after 30 / 90 s at SF9–10, 45 / 135 s at SF11, 60 / 180 s at SF12;
  retry gap = max(800 + 2.5 × receipt airtime, 1600) ms, jitter = gap / 2,
  receipt wait = gap + 400, hard limits and the listen-before-talk limit
  (≥ the longest packet + 300 ms) likewise; SF9's values equal v0.5.1's
  constants. Discovery's timeout follows (`link_disc_window_ms()`).
- **Test pace** (`ttest.c`): TEST_PLAN §16.3 (airtime ~4–5.5 % per board).
- **Logs:** `START ... sf=`, the run-down meta (`RD_META_VERSION` 2; a v1 meta
  reads as SF9) and the dump header `sf=`; `status`: `radio: SX1262 869.525
  MHz SF9 (range level 1) BW125, 22 dBm (max 22) ...`.
- Boards on different SFs can't hear each other at all (SOS included): the
  manual says so at `G`, the app warns before a test (§5.3).

### 5.3 App v0.2.0 as built (`morselink_app`)

- **Lines** (`app_state.dart`, `protocol.dart`): ST's paths (`lora`,
  `relayPath`), `RD` (`RadioInfo`), `CM` / `CE` (`cmd`, `cmdEnd`), EV with
  a detail (`eventText(what, who, detail)`); `HI`'s protocol; "accepted"
  once the board said HI on a connection.
- **Home screen:** status card with the board's Bluetooth (and why it isn't
  connected), internet, partner + link, battery, silent mode, then **LoRa**
  and **Relay** rows with coloured dots, and the radio line; the keying box
  turns into **COMMAND** (tertiary colour) with what is keyed and what the
  letter does (`commandNames`), the kept draft underneath, and briefly how
  the command ended; a ? opens the manual at "Command mode".
- **Manual** (`ui/manual_page.dart`): `lib/manual.dart` is generated from
  `docs/MANUAL.md` by the firmware's `tools/gen_manual.py`, which also
  checks the Morse table against `morse.c` and the command list against
  `cmd_valid()`; `lib/markdown_lite.dart` reads the Markdown subset; each
  `##` section is an ExpansionTile.
- **Radio exchange between the phones** (ctl topic): `{"cmd":"radio","from",
  "board", "sf","bw","dbm"}` when the board's setting or the partner
  changes, when MQTT connects, and in answer to `{"cmd":"radio?"}`. The rover
  asks before Start (waits up to 3 s) unless it heard within 10 min.
- **Start** (`test_page.dart`): `startChecks()` → a dialog: blocking (red:
  board, ID, link, internet, LoRa down for a LoRa test) or warnings
  (orange: range levels differ or unknown, relay down for a relay test,
  silent mode off, battery < 30 %, older firmware, Bluetooth-off run-downs),
  then "Start anyway". The base phone also warns when the rover's level
  differs (its status detail). The **baseline stop fix**: the rover no
  longer stops the base when its STOP reason is `done`. `rundown-l` chip
  (paths fixed to LoRa). The test page describes each test at the board's
  SF; the transmit-power field shows only for boards without `RD`.
- **Session files:** `tx_dbm`, `sf`, `bw_khz` = this board's (from `RD`),
  `peer_radio` = the other's, `protocol`; the running view counts `LINK`
  losses with their reason and shows the LoRa path state (`PATH` lines);
  the end record has `link_lost`.
- **Bluetooth** (`ble_link.dart`): `connect(mtu: null)`; on Android, wait
  for a pairing in progress to end (or the board hanging up: then "open its
  pairing window first"), at most 60 s; `requestMtu(247)` afterwards;
  back-off 4 / 8 / 16 / 30 s; every failure and drop gets words
  (`bleCodeText()`: 8 out of reach, 19 the board ended it, 22 this phone,
  5 / 6 / 61 pairing broken, 133 Android's error); two quick hang-ups in a
  row → "forget it in Bluetooth settings, O, pair again".
- **Background** (`background.dart` + `MainActivity.kt`): a foreground
  service (`KeepAliveService`, type `connectedDevice`, manifest permission
  `FOREGROUND_SERVICE_CONNECTED_DEVICE`) while a board is chosen and the
  Settings switch is on; the notification shows the board, partner and
  paths (updated through `NotificationManager.notify`, only with
  notification permission on Android 13+); started only when "Nearby
  devices" is granted (Android 14 requires it for this type); stopped when
  the app closes or the board is forgotten; every Android refusal is caught.
- **Tests** (`test/widget_test.dart`): the new helpers and the manual's
  parsing (54 Morse characters, the command table, the G note).

### 5.4 Bench tests as built

`bench.c/h` (pure, host-tested): the `BENCH_POWER` steps and the motor
pattern generator; `ui.c` runs them (`bench_power_start / timers / apply /
stop`, `bench_motor_go`), the net task switches the radio (`net_bench()`:
standby / listen / carrier, counted in the airtime), the haptics player
plays the pattern (`haptics_play_gen()`). Nothing else plays or queues
meanwhile. Details: TEST_PLAN §16.4; v0.6.1's steps and signals: §5A.3,
TEST_PLAN §17.7. Not yet tried on a board: the carrier
(`SetTxContinuousWave` 0xD1 on the SX1262; TxContinuousMode on the SX1276)
and the meter readings.

### 5.5 Analysis as built (`tools/analyse_tests.py`, standard library only)

TEST_PLAN §16.5: lost periods and availability per board (rebuilt for
≤ v0.5.1; the mall session reproduces §15.2's outages), PATH-state shares,
availability by label, by range setting, size bands, first try, RXBAD; the
dump reader (finds the header in raw monitor logs, skips charging rows,
dedupes repeated dumps), slopes (relay night: −13.2 / −14.0 mV/h), hourly
medians, motor on-time, and the A-curve mapping once a run-down A to empty
exists (tested only on a synthetic curve). Outputs: summary.txt,
messages.csv, sessions.csv, lost.csv, rundowns.csv (and points.csv since
v0.6.1, §5A.2).

### 5.6 What v0.6.0 left unverified, and what the field showed since

- Verified since: the app compiled first time on the owner's machine;
  SF12 on the real radios (run-down L: 920 min on both boards; Dynamic 4);
  the loss events on the phone (`EV LOST ... no-receipt`, `EV LORA ...`,
  `EV RESTORED`); the radio exchange between the two phones (`peer_radio` in
  the session files).
- Still not: the carrier and `bench power` on a board, the background
  service on Samsung over a whole night (One UI may still put the app to
  sleep: Battery → Unrestricted), the S10e with v0.2.x, the A-curve mapping
  on real data (needs run-down A to empty).

---

## 5A. The update of 20 October as built (v0.6.1 + app v0.2.1)

What the addendum specified, as it was built (exact behaviour: TEST_PLAN
§17, SPEC §0f, `docs/MANUAL.md`, `phone.h`; the code's comments say why).

### 5A.1 The key on GPIO14, the wearable build (§2.0)

- `board.h`: `PIN_EXT_BUTTON 14`; `USE_ONBOARD_BUTTON 0`, `USE_ONBOARD_LED
  0` by default (independent now); **`VERBOSE_LOG 1`** replaces the LED
  switch's side effect (the packet log `N.log` and the playback log
  `U.cal_log`). New compile checks: no key on a strapping pin (0 / 2 / 12 /
  15), the key on an RTC-capable pin (the power switch's ext0 wake). Motor
  comments: 2N2222 via 330 Ω.
- `keyer.c` / `power.c`: the internal pull-up as the key's only pull-up;
  `power.c`'s new `sleep_pulls()` sets it again in the RTC domain before deep
  sleep (on the ESP32 `gpio_config()`'s pull-up on an RTC pin already lands
  in the RTC register, and ext0 keeps RTC_PERIPH powered: checked in
  ESP-IDF v6.1's `gpio.c` / `sleep_modes.c`), and in the wearable build the
  LED's pin too.
- `app_main.c`: in the wearable build GPIO4 (the LED, from 3.3 V to the
  pin) becomes an input with its pull-up: its reset state is a pull-down,
  which would leak ~35 µA through the LED, switched off included. The boot
  banner says `key=GPIO14, internal pull-up`.

### 5A.2 The point test

- **`ttest.c/h`** (pure, host-tested): `TT_POINT` (after `rundown-l`);
  `tt_params_t` gains `n_steps`, `plan[8]` (`tt_step_t`: sf, dbm), `n`.
  `tt_parse_params()` reads `plan=9:17,9:22,12:22` (`,` or `/` between the
  steps; SF 9–12, power 2–22, at most 8 steps; a trailing comma is refused)
  and `n=1..20`; a point test gets the default sweep without a plan and
  `paths = lora` always. `tt_plan_text(p, sep, ...)`. The timetable:
  `tt_point_gap_ms` 15 / 20 / 25 / 40 s, `tt_point_settle_ms` 10 / 10 / 20 /
  20 s, `tt_point_step_ms` = settle + (n + 1) gaps (325 / 430 / 545 / 860 s
  with n = 20), `tt_point_offset_ms`, `tt_point_step_at`; `tt_next()`
  follows it (the base half a gap later) and records each message's step.
  `TT_ARGS_MAX` 160 (the parameter line outgrew 100: the UI event, the net
  command, the console and `PHONE_ARGS_MAX` all take 160 now).
- **The gaps are not the addendum's** (§2.4): a 25-character message that
  fails takes up to 11.8 / 13.5 / 19.8 / 33.0 s to give up (the host
  simulator, `test_point_gaps`), and the engine fails every message waiting
  behind a failing one.
- **`link.c`**: `link_send_test_msg()` = `link_send_msg()` but also while
  `LS_LOST` (a shared `send_msg(..., even_if_lost)`); an ended link or
  another partner still refuses. The user's rule is unchanged.
- **`ntest.c`**: `ntest_init(eng, noise, set_radio)`; net.c's
  `test_set_radio()` sets `want_sf` / `want_dbm` (applied between packets,
  never saved) and returns the clamped power. At START the point test keeps
  the board's own sf / dbm and switches to step 1 (`pt_switch`: a `STEP`
  line); `ntest_tick()` switches at each step's start and at the end puts
  the setting back (`pt_restore`: `STEP,i=0,...,restored=1`) and stops
  (`done`); `ntest_stop()` puts it back whatever stopped it. `generate()`
  sends the L copy with `link_send_test_msg` (skip `lost` only if the
  partner changed or the link ended); a message due while the one before
  still waits is **held** until it settles (`late_ms` in its GEN; a newer
  one or a step change drops it as `lora_skip=late`; a stop as `stopped`),
  so the counts always add up. START adds `plan=9:17/9:22/12:22,n=..,size=25`
  (`/`: the T line's fields are comma-separated); GEN adds `step`.
  `NT_LINE_MAX` 224 (an 8-step START is ~210 characters; the ring is 28 KB
  during a test).
- **UI / console**: `radio sf` and `radio power` are refused during a test.
  `test start point` (the default sweep) or `test start test=point plan=...
  n=... role=...`.
- **The app** (`test_page.dart`, `app_state.dart`, `protocol.dart`): the
  `point` chip; a required **Label**; **Sweep** (the default plan, steps
  added / removed with SF and power dropdowns, "Default sweep" resets it) or
  **Single** (SF chips G1–G4, power chips 17 / 20 / 22); the duration
  (`planSeconds()`, the firmware's arithmetic). TEST START with `plan=`; the
  ctl start carries `plan` and `label` (the base phone uses them; its
  session record has the label). The running view: the spot, "Step i of n:
  SF / dBm, time left" (from the STEP line's `step_ms`), each step's counts
  (GEN's `step`; ACK / FAIL by their seq), "We hear them" (RX), "They hear
  us" (the base phone sends `{"cmd":"pt", rssi, snr, step}` at most every
  5 s); "Edit the label" (a `label` record). **Repeat at the next spot**
  (after a test, on the setup page) asks for a new label and starts the same
  plan. The end record has each step's counts. A board on protocol < 3
  can't start it (blocking check; the base phone refuses it too).
- **The analysis** (`tools/analyse_tests.py`): `points.csv` (one row per
  session × step × direction: label, setting as used, sent / delivered /
  failed / skipped, rate, first try, rtt, RSSI and SNR median / min of
  everything the receiver heard from the sender during its own step, the
  margin over the SF's decoding limit, the distance from the label or GPS,
  the two boards' step-start skew); summary.txt: the per-step table, the
  **attenuation** (labels grouped by the words before the first comma; the
  one with "clear" / "ref" is the reference) with the power-consistency
  check, and the **distance ladder** ("open" / "ladder" + a distance):
  RSSI = A − 10 n log10(d), n and R² per setting and direction. Checked on
  synthetic sessions (losses of 6 and 18 dB and n = 2.8 put in: 5.5–6.5 dB,
  17–19 dB and n = 2.62–2.79 came out). Point sessions are left out of the
  "by range setting" block (their steps have their own table). **Graphs:
  still to do** (§2.5).

### 5A.3 The meter session

- **`bench.c/h`** (pure): the steps `1`, `1b` (keying), `1c` (key held), `2`,
  `3a` (advertising, not connectable), `3b` (connected), `3c` (LoRa standby
  + Bluetooth), `3d` (SF12 listening), `4a–c` (carriers), `5a–d` (motors),
  `6` (off), each with its `id`; `bench_signal_ms()` matches what the UI does
  (a new number N: (2N − 1) × 300 ms + 2 s; a part: 0.1 + 1 s);
  `bench_power_ms(22)` = **543,800 ms** (host test and app test both check
  it), 532,700 without 4c. `BENCH_WAIT_USB_S` removed.
- **`ui.c`**: `bench_power_start(now, from_phone)`: refused (test / sos /
  running) with `BN refused <why>`; with USB in it waits however long
  (`BN wait-usb`), without it starts (`BN start <s> <max dBm>`, then 5 s to
  settle); signals on **motor M1** (`haptics_raw`; the test-setup LED
  mirrors them too); `bench_apply()` sends `BN step <id> <s>` first, then:
  1 standby + `ble_flush` + `ble_stop`; 1b / 1c nothing; 2 SF9 + listen; 3a
  `ble_set_connectable(false)` + `ble_start`; 3b connectable; 3c standby;
  3d SF12 + listen; 4x SF9 + carrier; 5x motors; 6 `BN end off`, flush,
  switch off. `bench_undo()`: motors off, radio back at the stored SF,
  connectable, Bluetooth as `ble_wanted()` says. `bench_power_stop(how)`:
  `BN end hold|stopped|app`.
- **`ble.c`**: `ble_set_connectable()`, `ble_flush()` (waits for the queue
  and the line being sent), `BENCH POWER` / `BENCH STOP` →
  `ui_post_bench_cmd()`.
- **`phone.c/h`**: protocol **3**; `phone_fmt_bench()`;
  `PHONE_IN_BENCH_POWER` / `_STOP`; `PHONE_EV_BLE_OFF`.
- **The app**: `lib/bench.dart` (the same steps, signals and timetable;
  `benchAt()` says where the session is), `lib/ui/meter_page.dart` (the
  checklist before Start; the step, what it does, what to do, time left,
  "read the meter now" in the second half, next step; the step list with
  the current one highlighted), `BenchRun` in `app_state.dart` (`BN` lines;
  `BN step` resyncs the clock; from 3b on, while not connected, `ble.kick()`
  every 3 s), a banner on the home screen. Tests → Meter session.

### 5A.4 `W`

- `ui.c`: `ble_wanted()` = a phone paired && not `store_load_ble_off()`;
  used at boot, after a Bluetooth-off test, and by `bench_undo()`.
  `ble_switch(want)`: off → `EV BLE_OFF <own ID>`, `ble_flush(500)`,
  `ble_stop()` (and the pretend phone goes), stored; on → only with a paired
  phone (else error: "O pairs a phone"), stored. `W` plays OK + OFF / ON;
  `O` and `phone pair` clear the flag. `status` says `phone: BLE off (W)`.
  Console `phone off` / `phone on`. `cmd_valid('W', "")`.
- `store.c/h`: the NVS key `bleoff` (written only when it changes).
- The app: `EV BLE_OFF` → "Bluetooth switched off on the board (W)", the
  status card says so, `BleLink.slow` (30 s between attempts) until the
  board says HI again; `commandNames['W']`.

### 5A.5 App robustness (the field problems of 16–18 Oct)

- **Base check:** 20 s after Start, if the base phone hasn't said `starting`
  or `running`, `baseAlarm`: a red banner on the running view ("its board is
  NOT running the test") and the Base phone row in red; also when it
  answered `refused` / `no board` / `busy`.
- **Stop that arrives:** `_stopBase()`: the stop is resent every 5 s until
  the base answers a state other than starting / running for that session
  (2 minutes at most, then a notice), and at once when MQTT reconnects. The
  base answers a stop for a session it doesn't run with `stopped` (`not
  running`) and ignores repeats while stopping (a second stop used to close
  its file at once). Also used when the rover's own board stopped by a hold.
- **Offline warning:** a red ("severe") start check when the partner's phone
  hasn't answered for 10 minutes and this phone's MQTT reconnected since.
- **`cleanSession = false`**: not done (optional; mqtt_client's API not
  checked here; the resent stop covers the case that mattered).

### 5A.6 Verification

- Firmware: builds **warning-free** on ESP-IDF v6.1 (binary 0xa7970, 55 %
  of the app partition free). **44,170 host checks** pass (new: the point
  test's parameters, plan, timetable for both roles, airtime; each gap
  against a failing message on the simulated channel at SF9–12; sending
  while lost; the bench steps, ids, signals and total; BN / BENCH / EV
  BLE_OFF lines; a 150-character TEST START); the soak with 150 seeds
  passes (44,890 checks). **63/63 QEMU steps** (new: a 2-step point test with n = 2 that
  switches SF and power, refuses `radio sf`, sends while the link is lost,
  delivers at the edge, puts the setting back and ends `done`; `W` off with
  the pretend phone (EV BLE_OFF, OK + OFF) and on refused without a paired
  phone; the meter session from the pretend phone: BN start / refused /
  step 1 / step 1b / end app). The whole suite ran on the wearable build
  (key on GPIO14). Free stack least ever (QEMU): UI 3.3 KB of 6 KB, net
  3.5 KB of 5 KB.
- The app: tree-sitter syntax check clean (all Dart files and the Kotlin
  service); a careful read of every changed function for types and null
  safety (it found and fixed one: the meter timer not reset after Clear);
  new unit tests in `test/widget_test.dart` (the point timetable equals the
  firmware's numbers; the meter total 543,800 ms; STEP / START / GEN lines;
  W, BN). **Not compiled** (§6.2).
- Not verified yet: anything on the boards (GPIO14 on the real pin, the
  pull-up in deep sleep, non-connectable advertising, the meter session's
  3b reconnect timing on a Samsung phone, the point test between two real
  boards); the app on a phone.

---

## 5B. The update of 21 October as built (v0.6.2 + app v0.2.2)

For the participant sessions (questionnaire JS-04: feeling the vibrations,
keying, learning Morse with the companion display). Exact behaviour:
SPEC §0g, `phone.h` (the contract), MANUAL §4, §9, §11.

### 5B.1 Firmware

- **`hseq.c/h`** (pure; now fully commented): `hjob_item_starts()` = when
  each Morse item of a job starts (the generator's `pushed_ms`, noted at an
  item's first element; `HSEQ_NO_START` for tones, gaps, empty items);
  `hstrength_mv()` (`I` 1-5 → 3000-3400 mV, `HSTRENGTH_DEFAULT` 4).
- **`decoder.c/h`** (pure; now fully commented): `n_pressed` (elements
  accepted, never reset: the UI compares before / after `dec_element()` to
  tell an element from a bounce), `last_ms`, `last_dash`;
  `dec_open_pattern()`.
- **`phone.c/h`**: protocol **4**; `phone_fmt_key` (KY), `_key_char` (KC),
  `_cal_result` (CB), `_speeds` (SP, `phone_speeds_t`), `_play` (PL; the text
  cut to fit the line), `_play_end`.
- **`ui.c`**: `phone_key_press()` after each element (or calibration press),
  `phone_key_char()` in `pump_decoder()` before the state handles the
  event, `phone_cal_result()` in `calib_evaluate()`, `phone_play_start()` in
  `play_job()` (one PL per Morse item; a job without Morse ends the last
  one), `phone_play_end()` in `stop_playback()` and on `UIE_PLAY_DONE`;
  `phone_sync()` sends `KY` alone once nothing is open and `SP` on change,
  at most every `PHONE_SP_GAP_MS` (1 s). **`I`**: `cmd_valid('I', "" / "1"-"5")`,
  `apply_strength()` (at boot only once a level was chosen), the level
  reverted if the chip refuses (error tone), `U.motor_mv` cached (no I2C in
  `phone_sync`); console `haptic strength`; `status` prints it.
- **`pmic.c/h`**: `pmic_set_motor_mv()` (AXP2101 reg 0x82 bits 4:0, 1.5 V +
  0.1 V steps, checked against XPowersLib; AXP192 0x26, 0.7 V + 25 mV),
  limited to `PMIC_MOTOR_MV_MIN/MAX` 3000-3400; `pmic_motor_mv()`.
- **`store.h/.c`**: `settings_t.strength` in the former `reserved[0]` (0 =
  never set; same layout, so no new version); validated on load.
- `config.h`: `MORSELINK_VERSION` "0.6.2", `PHONE_SP_GAP_MS`.

### 5B.2 App

- **`lib/learning.dart`** (new, pure Dart): `morseCode` (= `morse.c`, checked
  against the manual by a test), `playTimeline()` (the firmware's timing, the
  C test's `phone_model()` line for line), `Speeds` (`keyWpm` from the
  person's own timing, `readWpm` with Farnsworth), `Learning` (the KY / KC /
  CB / SP / PL handlers, `onDraft()`'s pattern alignment: common prefix
  kept, the KC's pattern for the letter just added, the table otherwise;
  `posAt()`; `ShowMode`).
- **`lib/ui/learn_widgets.dart`** (new): `MorseLetter`, `KeyedLetters`,
  `LastLetterLine`, `TimingBar` (CustomPaint), `KeyingView`, `SpeedsLine`,
  `SpeedsCard`, `CalibrationCard`, `NowPlayingCard` (40 ms ticker; a window
  of 10 letters on the main screen), `MorseChart`.
- **`lib/ui/learn_page.dart`** (new): the Learn page (app bar: school icon).
- `app_state.dart`: `learn`, the new tags, `ChatItem.masked`,
  `setShowMode()`, `revealPlayback()`, `revealChat()`; the older-firmware
  start warning reworded. `home_page.dart`: the keying box, now playing,
  masked bubbles. `protocol.dart`: `appProtocol` 4, `commandNames['I']`.

### 5B.3 Verification

- Firmware: builds **warning-free** on ESP-IDF v6.1 (binary 0xa86f0, 55 %
  free). **44,212 host checks pass**; the soak with 150 seeds passes (44,932) (new: the PL start times; the app's
  timing model against the generator, element for element, over 400 random
  jobs at every speed and Farnsworth setting behind a tone and an ID; the
  strength table; the open pattern and the element counter; every new line,
  incl. a PL text cut to the line). **65/65 QEMU steps** (new: KY / KC /
  DR per press and letter, the error prosign, E's echo and read-back as PL
  + PL end, `I` without a power chip, `haptic strength`; the recalibration
  as the phone sees it: SP calibrating, PL PARIS, KY cal per press, CB ok).
- App: **type-checked this time.** The Dart SDK 3.13.6 comes from
  **conda-forge** (reachable; pub.dev and storage.googleapis.com are not):
  `https://conda.anaconda.org/conda-forge/linux-64/dart-sdk-<ver>.conda`
  (a zip: unzip, then the `pkg-*.tar.zst` with Python's `zstandard`; add a
  `version` file in the SDK root or the analyzer crashes). Flutter 3.47.7's
  framework from GitHub (sparse: `packages/flutter/lib`,
  `packages/flutter_test/lib`, `engine/src/flutter/lib/ui`), a hand-made
  `sky_engine` package (`lib/ui` = the engine's `lib/ui`, an
  `_embedder.yaml` mapping `dart:*` to the SDK's `lib/` and `dart:ui` to
  `ui/ui.dart`), the framework's deps and the three plugins from GitHub
  (flutter_blue_plus at da4b13c, geolocator_v13.0.4 with its platform
  interface and android packages, mqtt_client 10.9.2; test_api, matcher and
  dart-lang/tools for the tests), and a hand-written
  `.dart_tool/package_config.json`: **`dart analyze lib test`: no errors**
  (one old dead-code warning in `manual_page.dart`). The unit tests ran on
  the Dart VM with a 40-line stand-in for flutter_test's `test` / `expect`:
  **247 checks pass** (the learning view's: the timeline against numbers
  printed by `hseq.c`, PL / KY / KC / CB / SP handling, the patterns above
  the draft, wpm, the table against the manual). Not run: widgets on a
  device (no Flutter engine here).
- Not verified: anything on the boards (`I` on the AXP2101: the register
  write, the feel of 3.0 V against 3.4 V; the BLE line rate while keying
  fast; the app's layout on the S10e's small screen).

### 5B.4 Things to know

- `KY` lines go out on every press; at a fast novice's ~5 presses a second
  that is ~5 short lines a second, well inside the 16-line BLE queue.
- The app's daily log (`ML_log_<date>.jsonl`) records every board line,
  so every `KY` (press length, boundary) and `SP` (speeds) of a session is
  in it: a participant's keying over time can be read back without new
  firmware. `analyse_tests.py` doesn't read them (yet).
- `I` and the meter session: DCDC1's voltage changes the motors' current
  (and the ESP32's); run the meter session at `I4` (the default) unless
  the strength is what is being measured.

---

## 6. Environment

### 6.1 The owner's machine

**Firmware:** Windows, VS Code + Espressif extension, **ESP-IDF v6.1** (IDF
in `C:\esp\v6.1\esp-idf`), board on **COM4** (the other board's port may
differ). **Windows Smart App Control must stay OFF** (it blocked the
compiler). The **ESP-IDF terminal**:
```
idf.py set-target esp32        # once per fresh folder
idf.py build
idf.py -p COM4 flash monitor
idf.py -p COM4 monitor         # monitor only (dumps): Ctrl+T Ctrl+L logs to a file, Ctrl+] exits
```
Copying a new version over the folder and building works. Changing an
option `sdkconfig` already has needs `idf.py set-target esp32`; changing
`board.h`'s switches needs only `idf.py build`.

**App:** Flutter 3.47.7 + Android Studio, in PowerShell. Project
`C:\Users\khanm\dev\morselink_app` (made with `flutter create --org
com.morselink --project-name morselink_app --platforms android
morselink_app`, then the app zip over it). `flutter run --release` with the
phone on USB; `flutter build apk --release` for the other phone. The owner
changed `android\app\build.gradle.kts`: `ndkVersion = "30.0.16248370"`.
**App zips must never contain `build.gradle.kts` or other generated
files**: only `lib/`, `test/`, `pubspec.yaml`, `AndroidManifest.xml`,
`MainActivity.kt`, `README_APP.md`.

**Analysis:** `analyse_tests.py` in a folder of its own with `data\` and
`results\` next to it; run with no arguments or double-clicked. The owner
zips whole folders back (`RundownB\`, `Dynamic 1\` with `data\`,
`results\` and dump folders named by hand, e.g. `DEA- relay- RundownB\`,
`ABD- base- RundownB\` for board ABC: **trust the dump header, not folder
names**). He once ran an old copy of the script from his test folder:
remind him to copy the new one over it with each version.

### 6.2 Claude's sandbox

A new chat starts with an empty sandbox. What worked on 20 Oct (about 18
minutes; put it in a script and run it in the background with `setsid
nohup <script> &`, then read meanwhile):

```
git clone --depth 1 --branch v6.1 --recurse-submodules --shallow-submodules -j 8 \
    https://github.com/espressif/esp-idf.git /root/esp/esp-idf
cd /root/esp/esp-idf && python3 tools/idf_tools.py install --targets esp32
# Python env: create it, but install the packages from PyPI only
# (install-python-env adds dl.espressif.com, which is blocked: every package
# then waits through 5 retries):
timeout 300 python3 tools/idf_tools.py install-python-env --no-constraints   # creates the venv
$(ls -d /root/.espressif/python_env/idf6.1_py*_env | head -1)/bin/pip install \
    --index-url https://pypi.org/simple -r tools/requirements/requirements.core.txt
# QEMU: needs libslirp0 first (apt-get update, or the package isn't found)
apt-get update && apt-get install -y libslirp0
python3 tools/idf_tools.py install qemu-xtensa
```
Environment (a file to `source`; on 20 Oct the env was `idf6.1_py3.13_env`
and QEMU `esp_develop_9.2.2_20260417`):
```
touch /root/.espressif/espidf.constraints.v6.1.txt
export ESP_IDF_VERSION=6.1 IDF_PATH=/root/esp/esp-idf IDF_PYTHON_CHECK_CONSTRAINTS=no
export IDF_PYTHON_ENV_PATH=$(ls -d /root/.espressif/python_env/idf6.1_py*_env | head -1)
export PATH=$IDF_PYTHON_ENV_PATH/bin:$(ls -d /root/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin):$PATH
QB=$(ls -d /root/.espressif/tools/qemu-xtensa/*/qemu/bin 2>/dev/null | head -1); [ -n "$QB" ] && export PATH=$QB:$PATH
```
then `python $IDF_PATH/tools/idf.py set-target esp32 && python
$IDF_PATH/tools/idf.py build` (system cmake and ninja worked).

- Tests:
  ```
  make -C test/host && ./test/host/run_tests      # ~1 min; SOAK_SEEDS=150 for the full soak (~3.5 min)
  python -B tools/qemu_test.py                     # ~24 min, 63 steps; in the background; log in build/qemu_test.log
  python3 tools/gen_manual.py --app ../morselink_app   # after editing docs/MANUAL.md (--check: verify only)
  ```
  `tools/qemu_test.py` must **not** pass a `strap_mode` global. Don't
  rebuild while it runs (it uses `build/`). The sandbox has 2 cores: run the
  long soak after QEMU, not beside it.
- Packaging (from the folder above `morselink/`; the zip's top folder is
  `morselink-vX.Y.Z/`):
  ```
  zip -qr morselink-vX.Y.Z.zip morselink-vX.Y.Z/morselink -x '*/build/*' '*/sdkconfig' \
      '*/sdkconfig.old' '*/managed_components/*' '*/dependencies.lock' \
      '*/test/host/run_tests' '*/__pycache__/*' '*/build_log*'
  ```
  The app's zip: `morselink-app-vX.Y.Z/morselink_app/` with only the files
  listed in §6.1.
- **The app** (no Flutter or Dart SDK; pub.dev, storage.googleapis.com and
  Maven Central blocked; GitHub, raw.githubusercontent.com, npm and PyPI
  work): `pip install --break-system-packages tree-sitter-language-pack`,
  then this script on every Dart file and the Kotlin service (it catches
  syntax only — type errors, wrong names and null-safety slips need a
  careful read: e.g. Dart doesn't promote through a bool variable, so test
  `x != null` directly):
  ```python
  import sys
  from tree_sitter_language_pack import get_parser
  ok = True
  for f in sys.argv[1:]:
      src = open(f, 'rb').read()
      tree = get_parser('kotlin' if f.endswith('.kt') else 'dart').parse(src)
      stack = [tree.root_node]
      while stack:
          n = stack.pop()
          if n.type == 'ERROR' or n.is_missing:
              ok = False
              print(f, n.start_point, src[n.start_byte:n.end_byte][:80])
          stack.extend(n.children)
  print('ALL OK' if ok else 'ERRORS')
  ```
  Check plugin APIs against their sources at the resolved versions: `git
  clone --filter=blob:none --no-checkout
  https://github.com/chipweinberger/flutter_blue_plus.git` then `git
  checkout da4b13c` ("Release 1.36.8"; the package is under
  `packages/flutter_blue_plus/lib/src/`); geolocator tag
  `geolocator_v13.0.4`; Flutter: `git clone --depth 1 --branch 3.47.7
  --filter=blob:none --sparse https://github.com/flutter/flutter.git` and
  `git sparse-checkout set packages/flutter/lib/src/material
  packages/flutter/lib/src/widgets`.
- Field files are the owner's uploads: unpack each into its own folder and
  run Python on them with `-I`, paths as arguments (§7).

---

## 7. Lessons and gotchas

- **Pure modules + host simulation** caught real design flaws; **QEMU**
  caught bugs the host tests couldn't. Keep both green; add tests with each
  change. The connection-lost gaps (§5.1) slipped through because no test
  combined a failed message with a long silence afterwards, or a one-way
  link: those scenarios exist now (host and QEMU); keep adding the field's
  surprises as tests first.
- **v0.6.1:**
  - **A `T,` line's fields are comma-separated**: a value with commas (the
    plan `9:17,9:22`) splits into bogus fields in the app and the analysis.
    T lines write the plan with `/`; the parser takes both.
  - **Size test gaps against the failure time, not the airtime:** the
    engine fails every message queued behind a failing one, so a gap
    shorter than the worst give-up time (11.8–33 s for 25 characters at
    SF9–12) turns one failure into two. `test_point_gaps` measures it on the
    simulated channel; rerun it if `link_timing()` changes.
  - **GPIO4's reset state is a pull-down** (Espressif's pin list): a pin
    with an LED to 3.3 V on it glows faintly and leaks ~35 µA if left alone.
    Give unused pins a harmless state, awake and in deep sleep.
  - **QEMU's play log is off after the early steps** (they end with
    `verbose off`): a step that expects `play:` lines must turn it on and
    off again (cost a QEMU rerun on 20 Oct).
  - **QEMU limits:** no Bluetooth (the meter session from the pretend phone
    is followed only to 1b: 3a–3d need real advertising), no power chip (the
    USB wait can't be tested), no paired phone (`W` on gives the error).
  - **Dart:** an apostrophe ends a single-quoted string: text with "isn't"
    goes in double quotes (tree-sitter catches it).
  - **Sandbox shell:** a Python heredoc whose body has triple quotes broke;
    write patch scripts to files in the scratchpad and run them.
- **v0.6.0 test premises that bit:** proof less than one lost time old
  legitimately keeps a path up (a test that expects "lost" must wait the
  full lost time after the last proof, not after the walk-away); a
  symmetric outage must not read as one-way (one-way needs a negative fresh
  report or a failure, and hearing within 2/3 of the lost time); a
  partner stuck one-way after the phone came back needed a quick keepalive
  on every move to connected; the sims run at 17 dBm (22 changed the
  path-loss sums of old scenarios).
- **QEMU expectations:** with 22 dBm the discovery path losses read 62 /
  100 / 130 dB; the relay of a pretend phone that just came may still be
  "off" in the first ST (the net task sets it on its next loop); a pause
  after a word adds a space to the draft ("OK "). Each cost a rerun.
- **The test pace must stay under ~6 % airtime per board at every SF:**
  the first SF11 / SF12 gaps were too short (fixed: 40–60 s, 90–130 s).
- **The real hardware differed from the spec** (V1.2 / AXP2101 / SX1262).
  When something "isn't found", ask for the boot log first.
- **SX1262 with a TCXO:** right after power-up it always reports "crystal
  didn't start"; clear the error flags before judging the clock.
- **AXP2101:** the radio's rail (ALDO2) starts off; DCDC1 powers the ESP32
  (and the motors), never switch it; there's no battery-current meter.
- **QEMU can't deep-sleep and has no Bluetooth**: the tests use the
  pretend phone and must never pair.
- Harmless boot warnings: `ledc: GPIO 25/13/4 is not usable`.
- **Loopback hides the real radio**; never saved; `status` shows it.
- Logs from two cores interleave: QEMU tests use `expect_all()`. A regex
  that starts with `\n` fails after an `expect()` that already consumed the
  newline (cost a QEMU rerun on 14 Oct).
- NimBLE facts behind `ble.c`: see the file's top comment.
- **Check the work folder before continuing:** diff it against the last
  delivered zip first.
- **App:** NDK install can fail (set `ndkVersion` in quotes); "not
  authorized" = accept the USB debugging prompt; path_provider was dropped;
  the base phone's app must be on screen when a session starts (Android
  foreground-service rule); `TestLine.parse()` keeps ID-like fields as text
  via `_textFields`.
- **The app's Paths chip defaults to `both`**: the relay night ran both
  paths. Hence the start confirmation (§5.3).
- **The base phone's internet** lost three runs (16–18 Oct): it is now
  checked (§5A.5), but the procedure still says Wi-Fi + charger + wait for
  "Base phone: running".
- **Sandbox shell:** `pkill -f <pattern>` / `pgrep -f` kill or match your
  *own* shell when the pattern appears in the command line (exit 144): put
  the work in a script file and kill by PID.
- **Run-downs:** start from full (LED off), stop without USB (phone or 5 s
  hold), then plug in and dump; dump before the next run-down; **don't open
  the serial monitor during one** (it restarts the board: the A night ended
  `end=none` that way); a run-down A takes ~2–2.5 days (estimate).
- **Noise near a PC:** the base logged −101 dBm noise (vs −114) near the
  owner's desktop and speakers; keep boards ~1 m from PCs, speakers and
  chargers.
- **What the desk run and relay night showed** (sanity checks for any new
  test): LoRa round trip ≈ message airtime + receipt airtime (0.39 s for 5
  characters, 0.82 s for 100, +20.5 ms per 4.5 characters); relay ~0.9 s
  median, ~1.7–3 s 95th percentile; desk RSSI −33 to −60 dBm, SNR 11 dB;
  LoRa retries ~0.4 % at 2 m (both boards transmitting at random times).
- **Gen the manual, don't edit `lib/manual.dart`:** `docs/MANUAL.md` is the
  source; `tools/gen_manual.py` fails when its Morse table or command list
  drifts from the firmware.

---

## 8. Where to find more

Earlier chats (search for "MorseLink"):
- "ESP32 BLE setup and configuration" (1 Oct): the original design.
- "Project handover review" (to 7 Oct): v0.1–v0.2.
- The radio chat (7–13 Oct): v0.3–v0.3.4; where TEST_PLAN.md was agreed.
- The chat of 13–14 Oct: v0.3.6, BLE (v0.4.0), test mode (v0.5.0), the
  app (v0.1.0), the desk session, `analyse_tests.py`.
- The chat of 14–15 Oct: the test-suite expansion (power by meter +
  battery-curve nights, phone-IMU motor test), v0.5.1, the relay night and
  mall session, the connection-lost diagnosis, the SF / power discussion
  (airtime table), the specification of v0.6.0.
- The chat of 15–16 Oct: the owner's decisions (§5.0), v0.6.0 and app
  v0.2.0 built and verified, MANUAL.md, the analysis extended.
- The chat that wrote the addendum (16–20 Oct): the field results of
  16–19 Oct (§2.3), the point-test idea, the owner's answers (SPEC §0f),
  the specification of v0.6.1.
- **The chat of 20 Oct (this one):** the key's GPIO (§2.0), v0.6.1 and app
  v0.2.1 built and verified.
- A KiCad chat: the hardware design.
