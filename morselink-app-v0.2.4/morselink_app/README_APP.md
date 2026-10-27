# MorseLink phone app (Android) — v0.2.4

Shows what your MorseLink is doing, relays its packets over the internet
(MQTT), runs the field tests in `docs/TEST_PLAN.md`, and (v0.2.2) shows
Morse as it is keyed and felt, for learning it. Made for firmware v0.6.2;
with v0.6.1 everything but the learning view works (letters only in the
keying box), with v0.6.0 everything but the point test and the meter
session too, and with v0.5.x it shows less (no paths, range level or
command box).

## What's new in v0.2.4 (24 Oct)

- For firmware v0.6.4: the command box names **`K`** (keep the draft past
  60 s) and the new **`I` 1-9**, says "Command mode left (3 s hold); the
  draft is kept" when you leave it with the 3 s hold, and its hint mentions
  that hold. The Learn page's strength line says I1 weakest, I8 normal, I9
  strongest.

## What was new in v0.2.3 (24 Oct)

- For firmware v0.6.3's **`H`** (how dashes feel: 1 = the sweep, 2 = the
  forearm motor only): the command box names it, and the Learn page's
  "Your timing" card says which is on. The SP line carries it, so the day's
  log records which style each participant had.

## What was new in v0.2.2 (21 Oct)

- **The keying box** shows each letter's dots and dashes above it, and the
  letter you are keying as you key it: each press appears as a dot or a
  dash the moment you let go. Under it: what the last letter became
  ("·− → A", or "?" with what you keyed), **where your last press landed**
  on a bar (dot zone, the boundary, dash zone, your average dot and dash),
  and a line with your keying speed and the reading speed.
- **Now playing:** while something plays on the arm (a message, an ID, a
  command's echo, PARIS), its letters with their Morse, the one buzzing
  highlighted and its elements lighting up as they play.
- **Learn** (the school icon at the top): all of it large, for participant
  sessions and practice: the **PARIS helper** while the board asks for it
  (each press green or red against PARIS, and why an attempt wasn't
  accepted), the keying, what is playing (with **Letters + Morse / Morse
  only / Hidden** for practising reading by feel: in the last two, new
  messages stay hidden in the chat until tapped), **your timing** (dot,
  dash, how many times longer your dashes are, your pauses against the
  board's limits, words per minute, how steady), the reading speed, the
  vibration strength, and a Morse chart.
- The command box knows **`I`** (vibration strength, firmware v0.6.2).
- Everything the board says is still written to the day's log
  (`ML_log_<date>.jsonl`), now including every press (`KY`, with its
  length) and the speeds (`SP`): a participant's keying over a session can
  be read back from it.
- New files: `lib/learning.dart` (the model), `lib/ui/learn_widgets.dart`,
  `lib/ui/learn_page.dart`.

## What was new in v0.2.1 (20 Oct)

- **Point test** (Tests > `point`): both boards still, a label for the spot,
  **Single** or **Sweep** (default SF9 / 17, SF9 / 22, SF12 / 22 dBm, about
  25 min; add or remove steps). The running page counts down each step,
  shows each step's counts and how each board hears the other. **Repeat at
  the next spot** starts the same plan with a new label.
- **Meter session** (Tests > Meter session): starts the board's `bench
  power` without USB, runs the same timetable as the board while its
  Bluetooth is off (steps 1-3a), says what to do in each step, reconnects at
  once in step 3b.
- **The base phone checked:** if it hasn't answered 20 s after Start, a red
  notice says its board is NOT running the test. **Stop** is resent every
  5 s until the base phone confirms (and as soon as the internet is back).
  Start warns harder when the base phone looks offline.
- **`W` on the board** (Bluetooth off): the app says so and tries only every
  30 s; the command box knows `W`.
- Session files: the point test's `label` and plan; the end record has each
  step's counts.

## What was new in v0.2.0 (16 Oct)

- **LoRa and Relay, each with its own status** (agreed 15 Oct): connected,
  checking, one-way (who can't hear whom), lost, off. Lost links say why
  (nothing heard, a message without receipt, one-way).
- **Range level:** the board's setting (SF, power) shows on the main screen
  and the test page. The two phones tell each other their boards' settings,
  and Start warns if the boards are on different levels.
- **Command mode in the keying box:** under **COMMAND**, what you key, with
  what the letter does; afterwards, briefly, how it ended. The ? next to it
  opens the manual at the command list.
- **Manual** (the ? at the top): every command, the range levels, the
  tones, the Morse table. The same text as `docs/MANUAL.md` in the firmware.
- **Start asks first**, listing what it found: LoRa down, range levels that
  differ, relay down, silent mode off, low battery.
- **Baseline fix:** when the rover's baseline finishes, the base is no longer
  stopped early (it finishes its own last messages).
- **Run-down L** (LoRa only, Bluetooth off) as a test chip.
- **Bluetooth, Samsung-friendly:** the app waits for Android's pairing to
  finish before it carries on (doing both at once was the likely cause of
  the S10e's drops), backs off when attempts keep failing, and says why a
  connection failed. A board that keeps hanging up right after connecting
  gets the hint to pair again.
- **Keeps running in the background** while a board is chosen, with a small
  permanent notification (Settings can switch it off), so Bluetooth and the
  relay don't stop when the phone sleeps.
- Session files record the real radio setting of each board (`sf`,
  `tx_dbm`, `peer_radio`).

## 1. Set up (once per PC)

The zip holds only the app's own files. Flutter makes the rest (the Android
build files that match your Flutter version):

```
cd C:\dev
flutter create --org com.morselink --project-name morselink_app --platforms android morselink_app
```

Then unzip `morselink-app-v0.2.4.zip` into `C:\dev`, so its `morselink_app`
folder lands on the one Flutter made. Say **Replace** for the files that
exist already. New in v0.2.2: `lib\learning.dart`, `lib\ui\learn_widgets.dart`,
`lib\ui\learn_page.dart` (v0.2.1 added `lib\bench.dart`, `lib\ui\meter_page.dart`;
v0.2.0 added `lib\background.dart`, `lib\manual.dart`,
`lib\markdown_lite.dart`, `lib\ui\manual_page.dart`).

```
cd C:\dev\morselink_app
flutter pub get
```

(Already set up for v0.1.0: just unzip over it, Replace, and `flutter run
--release` as below. `android\app\build.gradle.kts` is never in the zip, so
your NDK line stays.)

## 2. Put it on a phone

Phone plugged in by USB, USB debugging on (Settings > About phone > tap
Build number 7 times, then Developer options > USB debugging). The phone
asks "Allow USB debugging?": tick "Always allow" and tap Allow (until then
`flutter devices` says the device "is not authorized").

```
flutter run --release
```

The first build takes 5–10 minutes (Gradle downloads); later ones about a
minute. The app then stays on the phone as **MorseLink**; you can unplug.
Do the same with the other phone, or build the installer once with
`flutter build apk --release` and send
`build\app\outputs\flutter-apk\app-release.apk` to the phone (allow
"Install unknown apps" when it asks).

Things seen on the first build (14 Oct) and their fixes:
- `WARNING: A restricted method in java.lang.System has been called`:
  harmless (Gradle and the newer Java); ignore it.
- `Android sdkmanager did not install NDK 28.2.13676358`: in Android Studio,
  SDK Manager > SDK Tools > tick Show Package Details > NDK (Side by side):
  install a version, then in `android\app\build.gradle.kts` set
  `ndkVersion = "30.0.16248370"` (the version you installed, in quotes).
  Keep that line: later app zips never replace this file.

First start: allow **Nearby devices** and **Location** ("While using the
app"). On Android 13 and later, also allow **notifications** for MorseLink
(Settings > Apps > MorseLink > Notifications) if you want to see the
background notification; it works either way. Then, on each phone,
Settings > Apps > MorseLink > Battery > **Unrestricted** (Samsung: also
Battery and device care > Battery > Background usage limits: MorseLink must
not be in "Sleeping apps" or "Deep sleeping apps").

If the build fails, copy the first red error lines to me. If it builds but
the app closes as soon as it opens, try `flutter run` (a debug build: the
same app, just slower to start) and tell me.

## 3. Pair each phone with its board

1. On the board: command mode, `O`, hold 2 s (60 s to pair).
2. In the app: **Find my MorseLink**, tap `MorseLink-<ID>`, tap **Pair** when
   Android asks (the app shows "pairing" meanwhile). The board plays OK.
3. From then on the app connects by itself whenever it is open (also in the
   background).

If the app keeps showing "the board keeps hanging up right after
connecting": on the phone, Settings > Connections > Bluetooth >
MorseLink-<ID> > Unpair, Bluetooth off and on; then `O` on the board and
pair again (step 2). One phone per board: pairing a second phone makes the
board forget the first.

**The main screen:**
- **Status:** the board's Bluetooth (and why, when it isn't connected), the
  internet, the partner and the link, the battery, silent mode, then
  **LoRa** and **Relay** with a coloured dot each (green connected, orange
  one-way, red lost, grey checking / off), and the radio setting.
- **Keying:** what you are keying, letter by letter. In command mode the box
  turns to **COMMAND** and shows the command; the draft is kept underneath.
- **Chat:** sent ⏳ → ✓ delivered or ⚠ not delivered; received; events
  (LoRa or the relay changing, lost with the reason, restored). A red banner
  shows an SOS.
- **?** opens the manual.

## 4. Tests (the flask icon)

Before every session (both boards): firmware v0.6.1, linked to each other
(`S`), **the same range level** (`G` alone plays it; the app warns if they
differ), silent mode on (`Z`), loopback off. Power is 22 dBm unless set
otherwise (`radio power`). Both phones: app open, board connected, internet
on, automatic date and time, charged (the base phone on a power bank). Both
phones must use the same topic prefix (Settings; it is `morselink-za7q`
unless you change it).

**Rover** = the phone with the board you carry. On it: Tests → pick the
test and the paths → fill in the set-up → **Start**. The app then shows what
will start and anything it found (red: can't start; orange: warnings, then
"Start anyway"). Both boards start (the base phone follows by itself over
the internet; its screen shows the session too). During the session tap the
**Area** and **Situation** labels as things change (never while driving),
and **Note** for anything else. The Link row counts the losses with their
reason. **Stop** stops both. A baseline stops by itself after its messages
(120; 60 at range level 4), each board after its own last one.

The pace follows the range level (the airtime stays near 5 %): baseline one
message every 20 / 30 / 45 / 90 s at levels 1–4; dynamic a message every
10–20 / 20–40 / 40–60 / 90–130 s.

The **base** phone needs nothing: just leave the app open next to the fixed
board (don't switch the screen off before the session has started: Android
won't start the GPS for an app in the background). Put it **on Wi-Fi and on
its charger**, and don't walk away until the rover's page says "Base phone:
running" (a red notice comes after 20 s if it hasn't answered).

**Point test** (TEST_PLAN §17): both boards 1 m up, at the same height,
antennas upright, phones 1 m away. Rover: Tests → `point` → the **label**
(e.g. `pillar A, behind, 12 m`; the reference at the same distance is
`pillar A, clear, 12 m`; the distance ladder `open, 200 m`) → Sweep or
Single → Start. Don't touch the boards until it ends; then **Repeat at the
next spot**.

**Meter session** (TEST_PLAN §17.7): battery full, the meter in series,
**USB unplugged**; Tests → Meter session → Start. The page says the step,
the time left and what to do; read each step in its second half.

**Files:** each phone writes `ML_<session>_<rover|base>_<boardID>.jsonl`
(and one `ML_log_<date>.jsonl` per day with everything the board said) in
`Download\MorseLink` (or, if Android doesn't allow that,
`Android\data\com.morselink.morselink_app\files\logs`; Settings shows which).
Copy them to the PC over USB after the tests.

**Run-down tests** (battery life): start them from the app too, then leave
the board alone until it switches itself off. A and L turn the board's
Bluetooth off (the app then loses it: that's expected). Afterwards connect
the board to the PC: `test dump` in the serial monitor prints the battery
log as CSV.

## 5. What is where

| File | What |
|---|---|
| `lib/app_state.dart` | everything the app knows and does |
| `lib/ble_link.dart` | Bluetooth: connect (waiting for pairing), reconnect, lines, why it failed |
| `lib/background.dart` | keeping the app running with the screen off (the service is in `MainActivity.kt`) |
| `lib/relay.dart` | MQTT (HiveMQ's public broker, port 1883) |
| `lib/protocol.dart` | the board's lines (firmware `main/phone.h`), and their wording; the point test's timetable |
| `lib/bench.dart` | the meter session's steps and timetable (the same as the firmware's `main/bench.c`) |
| `lib/learning.dart` | the learning view: the board's KY / KC / CB / SP / PL lines, the patterns above the draft, the speeds, the playback's timeline (the firmware's timing) |
| `lib/storage.dart` | settings and log files |
| `lib/manual.dart` | the manual: generated from the firmware's `docs/MANUAL.md` (`tools/gen_manual.py`); don't edit |
| `lib/markdown_lite.dart` | reads the manual's Markdown |
| `lib/ui/` | the screens (`manual_page.dart`: the manual; `meter_page.dart`: the meter session; `learn_page.dart` and `learn_widgets.dart`: the learning view) |
| `android/.../MainActivity.kt` | keeps the screen on; tells the app its folders; the background service |
| `android/.../AndroidManifest.xml` | permissions, and the background service |
| `test/widget_test.dart` | checks of the line protocol and the manual (`flutter test`) |

The relay uses a public broker: anyone could read or send on these topics.
Fine for the prototype (say so in the report).
