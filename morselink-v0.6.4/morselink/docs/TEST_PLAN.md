# MorseLink — Wireless Test Plan and Test Software

**Written:** 13 October 2026, agreed with the owner in the radio chat.
**Updated:** 14 October 2026: §12 the analysis script as built, §13 the
first (desk) results and a known issue; §14 the power and motor tests
(agreed late on 14 October; firmware v0.5.1); 15 October: §15 the relay
night and the mall session, the connection-lost gaps, the range settings
and the revised plan. Additional tests agreed later get
their own sections after §13 (HANDOVER.md §5.1 has the starting menu).
**For:** the chat that builds the test software, once the phone app and the
MQTT relay exist (expected 13–14 Oct). Also the source for the report's
"testing" chapter.

**Attach to that chat:** this file, the latest project zip (with
`docs/HANDOVER.md`, `docs/SPEC.md`), and whatever the app chat wrote about
the app, the BLE protocol and the relay. Read `HANDOVER.md` first (how the
owner works, the environment), then this file, then the app's notes.

**Scope:** the wireless link and battery life only. No haptics and no human
participants: the wearable isn't complete yet (that part is future work,
§9). The moving board is always **held in the hand**.

---

## 0. Summary

| Test | Set-up | What it gives |
|---|---|---|
| **1. Baseline** | both boards fixed, line of sight, at **10 m** and at **~200 m**; a size sweep (5 / 25 / 50 / 100 characters) | the best case, and the effect of message size alone; the reference for everything else |
| **2. Dynamic** | one board fixed, the other carried by hand: in a car, walking outdoors, indoors; rural, suburban, urban, dense urban areas; random sizes every 10–20 s | real-world reliability, latency and range, per environment; LoRa against the relay at the same place and time |
| **3. Battery run-down** | full battery until the board switches itself off, in two modes | battery life in hours |

All of it is **automatic**: the boards generate and log the traffic, the
phones add time, GPS and labels. During a session the only input is tapping
labels in the app. Nothing vibrates.

---

## 1. Aims, and what the figures mean

The tests answer: *how reliable and how fast is MorseLink's messaging in
real conditions, how does it compare with the phone relay, and how long does
a battery last?*

| Measure | What it is | Why it matters |
|---|---|---|
| **Message delivery rate** | % of messages confirmed by a receipt, after all retries | the user's view: did my message arrive? |
| **Packet loss on the air** | % of individual transmissions (each retry copy, each receipt) that didn't arrive | the radio's true performance; retries hide it, so without it LoRa looks better than it is near the edge |
| **Retries per message** | copies needed before the receipt | how hard the system works; rising retries warn before messages start failing |
| **Latency** | send → receipt (round trip, on the sender's clock); approximate one-way from the phones' synced clocks | how "instant" it feels; reported as the **median and 95th percentile**, because retries make a long tail that an average hides |
| **Signal strength (RSSI)** | received power, dBm | falls with distance and obstacles |
| **Signal quality (SNR)** | signal relative to noise, dB | LoRa decodes slightly *below* the noise, down to about **−12.5 dB** at SF9; SNR shows how close each packet came to that limit |
| **Path-loss exponent** | how fast the signal fades with distance, fitted per environment | the standard way to describe an environment (about 2 in open space, 3–4 in built-up areas); comparable with published LoRa studies |
| **Message-size effect** | delivery and latency for 5 / 25 / 50 / 100 characters | airtime grows from ~0.17 s to ~0.6 s; longer packets are more exposed to fading; tells whether to keep messages short |
| **LoRa availability** | % of time the link was up (partner heard in the last 90 s) | how much of the time LoRa is usable at all, per environment |
| **Connection-lost events** | how often the link dropped; how long until noticed and until recovered | how the walkie-talkie link copes with movement |
| **Airtime used** | % of each hour spent transmitting | compliance: the band allows 10 %, and the firmware enforces it |
| **Channel waits** | time held back because someone else was transmitting (listen-before-talk) | interference from other users on 869.525 MHz, likely higher in cities |
| **Noise floor** | background level on the channel, dBm, every minute | explains bad results in noisy places |
| **Relay delivery and latency** | per message, through BLE → phone → broker → phone → BLE | whether the relay fills LoRa's gaps, and its cost in delay and dependence on mobile coverage |
| **Battery life** | hours from full to switch-off, per mode | whether a day of real use is realistic |

**Context figure for the report:** at SF9 / 125 kHz the radio still hears
signals down to about −129 dBm. At 17 dBm transmit power that allows roughly
146 dB of path loss before messages stop (antenna gains aside). The tests
show where each environment uses that up.

---

## 2. The tests

### 2.1 Test 1 — Baseline (both boards fixed)

- Two runs: **10 m** and **about 200 m** apart, both with clear line of
  sight. Both boards on a stand or table at the same height (record it),
  antennas vertical, not held.
- Each board sends **120 messages**: 30 each of 5, 25, 50 and 100 characters,
  in a **shuffled order** (seeded from the session ID, so a run can be
  reproduced).
- Fixed timing: each board sends every **20 s**, the base starting 10 s
  after the rover, so the two alternate and don't collide.
- **Paths: both at once** (default; see the note below). Each message goes
  over LoRa and over the relay at the same moment.
- Duration: 120 × 20 s = **40 minutes per run**.

> Note: in the discussion the baseline was "once over LoRa, once over the
> relay". Sending both at the same moment, as in the dynamic test, gives the
> same data under identical conditions in half the time (the relay uses no
> LoRa airtime). The software keeps a `paths` setting (`both`, `lora`,
> `relay`), so separate runs remain possible if the owner prefers.

### 2.2 Test 2 — Dynamic (one fixed, one moving)

- The **base** board stays at a fixed, recorded place (description, rough
  height above ground, indoors/outdoors). Its phone stays with it,
  unattended, on charge.
- The **rover** board is carried in the hand by the owner, with its phone:
  in a car, then on foot outdoors, then indoors, across areas.
- Each board sends a message every **10–20 s (uniform random)**, size
  **5–100 characters (uniform random)**. 5 is the minimum because each
  message carries a 4-character marker (§4.4).
- **Each message goes over LoRa and the relay at the same moment**, logged as
  a pair.
- Runs until stopped from the rover's phone. One session can cover several
  areas and situations; the labels split the data.
- Suggested sessions: one rural, one suburban, one urban, one dense urban
  (a mall), 30–60 minutes each, each including car, walking and indoor
  stretches where possible.

### 2.3 Test 3 — Battery run-down

Full charge (charge LED off), unplug, run until the board switches itself
off. The board logs battery voltage every minute, **to its own flash**, so
the log survives even without a phone. Two modes:

| Mode | What runs |
|---|---|
| **A. Listening** | linked to the other board, keepalives only, **Bluetooth off**, no messages |
| **B. Busy** | linked, Bluetooth connected to its phone, test traffic as in the dynamic test (both boards) |

Each run probably takes a day or more, so start runs overnight, in parallel
with the other tests. Use the same battery for both modes, or record which
battery was used. Record its rated capacity.

The "switched off" drain would take months to run down, so it moves to
future work (§9), together with the current-per-state measurement.

### 2.4 Labels and session details

**Set once per session (in the app, before starting):** test type, paths,
transmit power (17 dBm unless stated), base description and height, rover
carry = **hand**, battery IDs, free note.

**Tapped during the session (two rows of buttons; the current choice stays
highlighted until changed; every tap is logged with time and GPS):**

| Area | Means |
|---|---|
| Rural | open land, farms, few buildings |
| Suburban | houses and gardens, low buildings |
| Urban | town centre, shops, buildings up to a few floors |
| Dense urban | city centre, tall buildings, malls |

| Situation | Means |
|---|---|
| Car | the rover inside a vehicle |
| Walking outdoors | on foot, outside |
| Indoors | inside a building (add "mall", "house", ... as a note) |
| Stationary | stopped at one spot for a clean reading |

Plus a free-text note button. **Safety:** labels are not tapped while
driving: set "Car" before setting off, change it after parking.

---

## 3. How each figure is computed

Done by the analysis script (§4.6) from the logs (§4.4).

- **Delivery rate:** `delivered ÷ sent` per path, where "sent" excludes
  messages the test deliberately skipped (§5). Reported with a 95 % Wilson
  confidence interval and the count *n*.
- **First-try rate:** delivered with `tries = 1`.
- **Packet loss on the air** (no change to the packet format is needed): for
  each direction and packet type, `1 − received ÷ transmitted`, counting
  every transmitted copy on the sender (TX events) and every received copy,
  duplicates included, on the receiver (RX events). Retry copies share a
  message ID, so this is a count per message, not a copy-by-copy match.
  Keepalives give extra samples between messages.
- **Latency:** round trip = `receipt time − first transmission` (radio
  latency) and `receipt time − generation` (what a user would feel, including
  queueing). One-way ≈ `receive time − send time` after aligning both
  boards' clocks to phone time (§4.5): accurate to roughly ±50 ms, which is
  fine for the relay (seconds) and indicative for LoRa (hundreds of ms).
- **RSSI / SNR:** per received packet. Reported as medians per bin; never
  average dBm values as if they were linear.
- **Path loss:** `PL = tx_dbm − RSSI`. The log-distance fit
  `PL(d) = PL(d0) + 10·n·log10(d / d0)` is a linear regression of PL on
  log10(distance), per area, using only points with GPS accuracy ≤ 20 m and
  distance ≥ 20 m. Report *n*, the intercept and R².
- **Distance:** haversine between the rover's GPS position (interpolated to
  the event time) and the base's position (median of the base phone's fixes
  over the session). Indoors, or with GPS accuracy > 30 m, distance is marked
  unreliable and those points are compared by label only.
- **LoRa availability:** time in link state "up" ÷ session time, from LINK
  events.
- **Loss detection:** time from the last packet heard to the "lost" event
  (should be about 90 s); recovery = time from the first packet heard again
  to "restored".
- **Relay rescue:** messages whose LoRa copy failed or was skipped but whose
  relay copy was delivered; and both failed.
- **Battery life:** time from unplugging to the last logged sample; the
  voltage-over-time curve per mode.

---

## 4. Design of the test software

### 4.1 Overview

```
 rover phone (owner)                                  base phone (unattended)
 ┌───────────────────┐   MQTT: control + relay   ┌───────────────────┐
 │ app: test screen,  │◄────────────────────────►│ app: "base mode",  │
 │ labels, GPS, log   │                          │ GPS, log           │
 └───────▲───────────┘                          └───────▲───────────┘
         │ BLE: commands, relay packets, log lines      │ BLE
 ┌───────┴───────────┐        LoRa 869.525 MHz       ┌───────┴───────────┐
 │ rover board        │◄────────────────────────────►│ base board         │
 │ test mode          │                              │ test mode          │
 └───────────────────┘                              └───────────────────┘
```

- The **rover phone controls** the session: it sends start/stop to its own
  board over BLE and to the base phone over MQTT, which passes them to the
  base board.
- **Each board generates its own traffic** and logs every event. Log lines
  go over BLE to its phone, which adds wall-clock time and GPS and writes the
  session file.
- After a session, both phones' files are copied to a laptop, where the
  analysis script merges them.

### 4.2 Firmware: test mode

**Start / stop:** a BLE command from the phone (`TEST START` with the session
parameters, `TEST STOP`), plus console commands for bench use without phones
(`test start baseline|dynamic|rundown-a|rundown-b [paths] [role]`, `test
stop`). The board also leaves test mode on a **5 s hold** of its key (safety
valve if the phone dies). A run-down ends when the battery does.

**Preconditions** (refuse to start, with the reason logged): the two boards
are linked to each other and the link is up; loopback is off; our own SOS is
not active.

**While in test mode:**
- **No haptics** and no LED: nothing is played, alerts are dropped, test
  messages aren't stored for `R` and don't count for the quiet-reply rule.
  Lost / restored are logged, not played.
- The key is ignored except the 5 s hold (which exits; SOS works again after
  that).
- An incoming normal (non-test) message is logged and dropped.
- Traffic generation (§2): a pure, host-testable scheduler decides when, what
  size and which paths. Its inputs are the parameters and a seed; its outputs
  are the generation times, sizes and sequence numbers.
- **LoRa copy:** sent through the normal path (`net_send_text` → the link
  engine), so it gets the real retries and receipts. Skipped, and logged as
  such, when the link is lost or the hour's airtime is above 8 % (§5).
- **Relay copy:** must travel the real relay path (board → BLE → phone →
  MQTT → phone → BLE → board) and come back with its own receipt over the
  same path. **It must not wait behind the LoRa copy**: the engine has only
  one message in flight, so if the relay was built as a transport inside the
  engine, the test sends the relay copy through the relay transport directly
  (same packet format, flag `PF_VIA_PHONE`, its own message ID and receipt).
  Read the app chat's relay design first and pick the variant that measures
  the real relay hops.
- **Status line every 60 s:** battery, airtime, noise floor, queue, free
  heap, Bluetooth state.
- **Run-down modes:** mode A turns Bluetooth off for the run. Both modes
  append a sample (minute, battery mV, charging flag) to flash every minute,
  saved in blocks every 10 minutes (NVS blobs are fine: about 2 KB per day),
  and `test dump` prints them as CSV over the console afterwards.

**Log buffer:** if Bluetooth drops, keep log lines in a RAM ring (for
example 128 lines) and flush on reconnect. If it overflows, log `GAP` with
the number lost. Every line has a per-board counter, so gaps are visible.

### 4.3 App: test screen, base mode, logging

- **Test screen (rover):** session set-up (§2.4), start / stop, the two label
  rows and a note button, a live view (messages sent / delivered per path,
  last RSSI / SNR, distance to base, battery). Keeps running with the screen
  off.
- **Base mode:** shows "base, session X running", follows start / stop from
  the rover over MQTT, logs GPS and the board's lines. It must survive
  hours with the screen off. On Android, run a **foreground service** (BLE,
  MQTT, GPS, file writing) and exempt the app from battery optimisation;
  background limits are much tighter on iOS. Keep the base phone on a power
  bank.
- **GPS:** high accuracy, 1 fix per second, logging latitude, longitude,
  accuracy, altitude, speed and heading. The phone clock should be on
  automatic (network) time; log which.
- **Network:** log the connection type (Wi-Fi / LTE / none), and the
  cellular signal level if the platform allows it, plus every broker
  connect / disconnect.
- **Files:** one JSON-lines file per phone per session (§4.4), on the
  phone's storage, exported afterwards (share sheet or USB). Expect about
  1 MB per hour.
- **Control over MQTT:** a control topic per board, e.g.
  `morselink/<ID>/ctl`, carrying JSON (`start` with the parameters and the
  session ID, `stop`). The relay keeps its own topics.

### 4.4 Log formats

**Board → phone (and the serial console): one text line per event**

```
T,<n>,<board_ms>,<EVENT>,<key>=<value>,...
```

`n` counts the board's log lines (gaps show losses); `board_ms` is the
board's time since start. Events:

| Event | Fields |
|---|---|
| `START` | `session`, `role` (base/rover), `test`, `paths`, `seed`, `tx_dbm`, `sf` (v0.6.0), `fw`, `own`, `peer`; a point test (v0.6.1) adds `plan` (e.g. `9:17/9:22/12:22`), `n`, `size` |
| `STOP` | `reason` (app / hold / error / battery), totals |
| `GEN` | `seq`, `size`, `lora_id` or `lora_skip` (`lost` / `duty` / `queue`; a point test also `late` / `stopped`), `relay_id` or `relay_skip` (`nophone`); a point test adds `step` (and `late_ms`) |
| `STEP` | (v0.6.1, point test) a step begins: `i` (1-based), `sf`, `dbm` (what this board uses), `n`, `step_ms`; at the end `i=0`, the board's own `sf` and `dbm`, `restored=1` (§17.2) |
| `TX` | `type`, `dst`, `id`, `copy`, `bytes`, `air_ms`, `dbm`, `lbt_ms` (time waited for a clear channel), `duty_pm` (airtime in the last hour, per mille) |
| `RX` | `type`, `src`, `id`, `bytes`, `rssi`, `snr` (dB), `dup` (0/1) |
| `RXBAD` | a damaged packet (CRC error) |
| `ROUT` / `RIN` | relay packet handed to / received from the phone: `id`, `bytes`, (`src`, `dup` for RIN) |
| `ACK` | `path` (L / R), `seq`, `id`, `rtt_ms`, `rtt_gen_ms`, `tries` |
| `FAIL` | `path`, `seq`, `id`, `tries`, `why` (no-receipt / timeout / stopped) |
| `LINK` | `state` (lost / restored / ended), `why` with lost (silence / no-receipt / one-way; v0.6.0), `heard_ago_ms` |
| `PATH` | (v0.6.0) a path changed: `path` (L / R), `state` (checking / connected / they-cant-hear / cant-hear-them / lost / off), `heard_s` (since we heard them on it), `they_s` (since they heard us, from their report); -1 = never |
| `STAT` | `bat_mv`, `bat_pct`, `chg`, `duty_pm`, `noise_dbm`, `heap`, `queue`, `ble` |
| `GAP` | `lost` (log lines dropped) |

**Test message text:** a 4-character marker, then filler to the exact size:
3 characters of sequence number in base 36 (`000`–`ZZZ`, 46,656 per
session), then `L` or `R` for the path, then filler from a fixed pattern
(e.g. repeating `PARIS `). Example, 25 characters:
`01CLPARIS PARIS PARIS PAR`. Uppercase letters, digits and spaces only, so
nothing unusual reaches the protocol. The receiver logs the marker, so the
analysis can pair copies even when a log line is missing.

**Phone files: JSON lines**, one object per line, every line with `t` = UTC
wall-clock milliseconds:

```
{"t":…,"kind":"session","session":"20261017-101500","role":"rover","test":"dynamic","paths":"both","tx_dbm":17,"base":"garden wall, 1 m","carry":"hand","battery":"B1 3000 mAh","phone":"…","app":"…","fw":"…","clock":"network"}
{"t":…,"kind":"board","n":1042,"ms":5123456,"ev":"TX","type":"MSG","dst":"ABC","id":4123,"copy":1,"bytes":60,"air_ms":370,"dbm":17,"lbt_ms":0,"duty_pm":31}
{"t":…,"kind":"gps","lat":-26.1,"lon":28.0,"acc":4.0,"alt":1650,"spd":13.2,"hdg":270}
{"t":…,"kind":"label","area":"urban","sit":"car","note":""}
{"t":…,"kind":"net","type":"lte","dbm":-95}
{"t":…,"kind":"relay","dir":"pub","topic":"morselink/ABC","id":4124,"bytes":60}
{"t":…,"kind":"ble","state":"connected","mtu":185}
```

File name: `ML_<session>_<role>_<boardID>.jsonl`. The session ID is the rover
phone's start time, `YYYYMMDD-HHMMSS`.

### 4.5 Clock alignment and distance

- Each `board` record has the board's time (`ms`) and the phone's arrival
  time (`t`). Per board and session, the offset `t − ms` is estimated as
  the **lowest** values (the lines that crossed BLE fastest), e.g. a rolling
  5th percentile over 10-minute windows, which also follows slow drift. Then
  board time + offset gives wall time for every event.
- Both phones are on network time, so both boards end up on the same clock
  to within tens of milliseconds.
- Distance: §3.

### 4.6 Analysis script

`tools/analyse_tests.py <folder of session files>` in Python 3 (pandas,
numpy, matplotlib; optional folium for maps). Steps:
1. Load all files; group by session.
2. Align clocks (§4.5); attach to every event the labels in force, the
   rover's interpolated GPS and the distance.
3. Join sender and receiver events: messages by (source, message ID), pairs
   by sequence number, receipts, raw TX / RX counts.
4. Write CSV tables per session and overall, split by area × situation ×
   path × size bucket: delivery (with CI), first-try rate, retries, packet
   loss per type and direction, latency median / 95th percentile (round
   trip and one-way), RSSI / SNR medians, LoRa availability, lost events
   per hour, detection / recovery times, airtime %, channel waits, noise
   floor, relay rescue rate.
5. Graphs (PNG):
   - delivery rate against distance, per area;
   - RSSI against log-distance with the fitted path-loss lines (n per area);
   - SNR against distance with the −12.5 dB limit drawn in;
   - latency distribution (cumulative) per path, LoRa against relay;
   - latency and delivery against message size (baseline; dynamic near the
     edge, binned by SNR margin);
   - a map of the rover's track coloured by delivery;
   - a timeline per session: label bands, RSSI, deliveries and failures,
     link lost / restored;
   - battery voltage against time per run-down mode.
6. Write `summary.md` with the headline numbers, for the report.

---

## 5. Rules and limits

- **Airtime (10 % per rolling hour, enforced by the firmware).** Airtime at
  SF9: a receipt 0.14 s; messages of 5 / 25 / 50 / 100 characters take
  0.17 / 0.25 / 0.37 / 0.60 s. Dynamic traffic per board, on average:
  messages ≈ 0.38 s every 15 s (2.5 %) plus receipts for the other board
  (≈ 1 %). Near the edge, five copies per message would reach about 13 %,
  so:
  - the LoRa copy is **skipped while the link is lost** (the product does
    the same: you can't send while lost). That's counted as "LoRa
    unavailable", not as a failure. The relay copy still goes, and
    keepalives keep probing, so recovery is still measured;
  - the LoRa copy is **skipped when the hour's airtime passes 8 %**, which
    leaves room for receipts and keepalives. Every skip is logged.
- **One LoRa message in flight** (the engine's rule): a message generated
  while the previous one still awaits its receipt queues behind it (up to
  three). The queueing shows in `rtt_gen_ms`. Beyond that, the copy is
  skipped with `queue`.
- **Duplicates:** the LoRa and relay copies have different message IDs, so
  neither is dropped as a duplicate of the other.
- **Discovery:** nobody runs `D` during tests, so discovery traffic doesn't
  mix in. Set silent mode (`Z`) on both boards to be sure.
- **Broker:** a public broker is fine. Use a topic prefix unique to the
  project; the messages are filler text only.

---

## 6. Building it

**Suggested order** (each step testable on its own):
1. The pure scheduler and marker (new module, host tests: sizes shuffled
   with equal counts, intervals in range, marker round trip, same seed gives
   the same sequence).
2. Firmware test mode with console start / stop and the log lines on the
   serial port; QEMU steps with loopback (traffic flows, nothing vibrates,
   the 5 s hold exits, normal use resumes afterwards).
3. BLE: test commands and the log stream (needs the app chat's BLE
   protocol).
4. The relay copy (needs the relay design).
5. App: test screen, base mode, labels, GPS, files, MQTT control.
6. Run-down logging to flash and `test dump`.
7. The analysis script, first on synthetic logs, then on a desk run.

**Where to hook in** (as of v0.3.5; discovery and the app have changed files
since, so check):
- `net.c`: `log_pkt()` already sees every packet sent and received, with
  airtime and RSSI / SNR; `try_transmit()` knows the listen-before-talk wait;
  the link engine gives the airtime used (`link_duty_used_ms`) and the noise
  floor (`radio_noise_dbm`).
- Link events (`LEV_MSG_SENT` / `LEV_MSG_FAILED` / lost / restored) arrive
  in `ui.c`'s `on_net_event()`; `on_net_msg()` is where incoming messages
  arrive (a test-mode check goes there).
- Battery: `pmic_read_status()`.
- The UI owns the user's state, so test mode is a UI state (or flag) that
  suppresses playback and key handling. The pure parts go in their own
  module with host tests, per the house style (HANDOVER §3.3).

**Definition of done** (before any field session):
- Starting from the rover phone starts both boards within 5 s; stopping
  stops both.
- A 10-minute desk run with the boards 1 m apart: no log gaps; every `GEN`
  has an `ACK` or `FAIL` per enabled path; delivery 100 %; LoRa one-way
  latency between the airtime and the airtime + 0.3 s (if negative or huge,
  the clock alignment is wrong).
- Nothing vibrates or lights during test mode; the 5 s hold exits; normal
  use works afterwards.
- Host tests, QEMU and the ESP-IDF build pass; the analysis script produces
  every table and graph from the desk run.

---

## 7. Running the tests: checklists

**Before every session:**
1. Both boards charged, same firmware, IDs set, linked to each other (`S`),
   silent mode on (`Z`), **the same range level** (`G` alone plays it;
   v0.6.0) and the power planned for the session (22 dBm unless `radio
   power` says otherwise).
2. `status` on both: the `radio:` line says `SX1262` (not LOOPBACK) with the
   SF / range level and power planned; the `paths:` line says LoRa
   connected. The app's Start then checks what it can (LoRa up, both range
   levels the same, the relay, silent mode) and asks.
3. Both phones charged (the base phone on a power bank), BLE connected,
   broker connected, GPS fix, clock on automatic time, app updated.
4. Antennas on and vertical. Rover board in the hand.
5. Session details filled in on the rover phone; the base phone in base mode.

**Baseline (twice: 10 m, then ~200 m):** place both boards (same height,
line of sight), measure the distance (map or tape), start "baseline", leave
everything untouched for 40 minutes, then stop and check the counts on the
screen.

**Dynamic:** base at its recorded place; start "dynamic"; set the area and
situation labels; drive, walk, go indoors, tapping labels as things change
(never while driving); stop at the end. Export both phones' files.

**Run-down:** charge fully, unplug, start mode A, B or L (v0.6.0: LoRa only,
Bluetooth off), leave until the board switches itself off, recharge, then
`test dump` over USB and save the text as a file next to the session files
(the analysis finds it, §16.5).

---

## 8. Expected results and limitations

**Hypotheses (to confirm or reject):**
1. Range is longest in rural areas and much shorter in dense urban areas and
   indoors; the path-loss exponent rises from about 2 towards 3–4.
2. Delivery stays near 100 % and then drops off quickly near the edge (a
   "cliff"), rather than fading gradually; retries rise just before it.
3. Long messages fail first near the edge.
4. LoRa latency is under a second on the first try; the relay is slower
   (seconds) but doesn't depend on distance, only on mobile coverage, so it
   rescues messages where LoRa is unavailable.
5. Continuous listening dominates battery use; Bluetooth adds a smaller
   share.

**Limitations to state in the report:**
- Indoor GPS is poor, so indoor results are compared by label, not distance.
- One pair of boards and one carrier: results show trends, not general laws.
- A car body weakens the signal, so car results are a harder case than
  walking.
- Battery figures are for one battery of unknown age.
- The public broker is not secure (fine for a prototype).
- The one-way latency is approximate (±50 ms).

---

## 9. Future recommendations

- **Current per state** with a USB power meter (or an INA219 board): idle
  listening, transmitting, Bluetooth connected, motors running, switched
  off. This gives battery life for any usage pattern, and the switched-off
  drain, which a run-down can't measure in the time available.
- **Human participants**, once the wearable is complete: reading accuracy
  per speed, telling dots from dashes on the arm (the dash sweep), keying
  accuracy, learning curve.
- **Radio settings:** compare spreading factors (SF7 / SF9 / SF12) and
  transmit powers for range against airtime.
- **Low-power listening (CAD)**, already prepared in the protocol, and its
  battery gain.
- **More devices:** interference and the BUSY queue with three or more
  boards; SOS relaying across a chain.
- **Long-term runs:** reliability over days, and in bad weather.

---

## 10. Timeline (deadline 26 October)

| Dates | Work |
|---|---|
| 13–14 Oct | app and relay (another chat) |
| 14–16 Oct | test software (§6), desk run |
| 16–17 Oct | baselines; first run-downs overnight |
| 17–21 Oct | dynamic sessions in each area; remaining run-downs |
| 21–23 Oct | analysis, graphs |
| 23–26 Oct | report |

---

## 11. As built (firmware v0.5.0, app v0.1.0, 13 October)

Everything above is implemented, with these specifics (they win where the
text above differs):

**Paths during a test.** The product's relay (every packet both ways) is
switched off while a test runs. Each test message goes as up to two
copies with the same sequence number: the **L copy** through the link
engine over LoRa only (its receipt over LoRa only), and the **R copy**
straight to the phone (flag `PF_VIA_PHONE`, its own message ID from the
engine's counter), answered by the other board's test mode with a receipt
that also goes back through the phones. Packets from the phone never reach
the link engine during a test, so the LoRa link state and signal readings
are LoRa's alone (relayed keepalives can't hide a LoRa dropout).

**R copies** are sent once (`tries=1`); no receipt within **30 s** = FAIL
(`why=timeout`). At most 8 wait at a time (`relay_skip=queue` beyond).

**Preconditions** (`REFUSED,reason=`): `running`, `no-id`, `loopback` (from
the phone only: the console may test with loopback on), `no-partner`,
`link-lost`, `busy` (a message of the user's still waits for its receipt),
`sos` (our SOS is active), `no-memory`, or a parameter error. Both boards
must be in test mode: a test message reaching a board that isn't would be
played.

**Timing.** The rover's first message 5 s after the start, the base's 15 s
(so the baseline's two boards alternate 10 s apart on a 20 s grid). The
dynamic gap is drawn from when the previous message was due.

**Log lines as built** (`T,<n>,<ms>,<EVENT>,...`):

| Event | Fields |
|---|---|
| `START` | `session, role, test, paths, seed, tx_dbm, fw, own, peer, lb` (1 = loopback) |
| `REFUSED` | `reason` (line number 0: outside a session) |
| `GEN` | `seq, size`, then `lora=queued` / `lora=off` / `lora_skip=lost\|duty\|queue\|refused`, then `relay_id=<id>` / `relay=off` / `relay_skip=nophone\|queue` |
| `TX` | `type, dst, id, copy, bytes, air_ms, dbm, lbt_ms, duty_pm` (+ `seq, path` for a test message) |
| `RX` | `type, src, dst, id, bytes, rssi, snr, dup` (+ `seq, path`) |
| `RXBAD` | (a damaged packet) |
| `ROUT` | `type, dst, id, bytes` (a packet handed to the phone) |
| `RIN` | `type, src, id, bytes, dup` (+ `seq, path`) |
| `ACK` | `path, seq, id, size, rtt_ms` (from the first transmission), `rtt_gen_ms` (from generation), `tries` |
| `FAIL` | `path, seq, id, size, tries, why` (`no-receipt`, `timeout`, `stopped`) |
| `LINK` | `state` (lost / restored / ended), `heard_ago_ms` |
| `STAT` | `bat_mv, bat_pct, chg, duty_pm, noise_dbm, heap, queue, ble` (every 60 s) |
| `USERMSG` | `src, bytes` (a normal message that arrived during the test: dropped) |
| `GAP` | `lost` (lines dropped from the 128-line ring while the phone was away) |
| `STOP` | `reason` (app / console / hold / done), `gen, ack_l, fail_l, skip_l, ack_r, fail_r, skip_r` |

After a stop, copies still out get up to 35 s for their results before
STOP is written. A baseline stops by itself (`done`) once all 120 are
settled.

**Board console:** `test start TEST [PATHS] [ROLE]` (or the key=value form),
`test stop`, `test dump` (run-down CSV: `minute,mv,charging`). The 5 s hold
on the key stops a test; nothing plays during one; the cancel tone plays
when it ends. Run-down samples: NVS namespace `rundown`, one blob of 60
samples per hour, written every 10 minutes; kept until the next run-down
starts. Mode A switches Bluetooth off 3 s after the start (so the START
line reaches the phone) and back on at the end.

**Phone → board:** `TEST START test=... paths=... role=... session=...
seed=...` and `TEST STOP` (phone.h).

**App:** MQTT on `broker.hivemq.com:1883`, topic prefix `morselink-za7q`
(Settings; the same on both phones): `<prefix>/<ID>` relay packets (base64),
`<prefix>/<ID>/ctl` JSON control: `start` (session, test, paths, seed, from,
meta), `stop`, and from the base phone `status` (state: starting / running
/ refused / stopped / no board / busy) and `pos` (lat, lon, acc, every
10 s). The session ID is the rover phone's local start time; the seed is
that number modulo 2^32. Not built: the cellular signal level and the
network type (the MQTT connection state is logged instead: `kind:"mqtt"`).
Other record kinds besides §4.4's: `dev` (every non-test line from the
board), `ctl` (control messages in and out), `session_end` (the app's
counts), `app`. The screen stays on while the app is open; during a session
GPS runs as a foreground service with a wake lock.

---

## 12. Analysis script as built (14 October)

`tools/analyse_tests.py`, Python 3.8+ with the **standard library only**
(no pandas / numpy / matplotlib, so it runs on any PC with Python, including
the ESP-IDF terminal's).

**Where things go:** the owner keeps the script in a folder of its own, with
`data\` next to it (both phones' `ML_<session>_<role>_<ID>.jsonl` files, any
subfolders; the daily `ML_log_*` files are skipped) and `results\` (made by
the script, replaced on every run). Run with no arguments, from any folder,
or by double-clicking (the window then waits for Enter). Folders can still
be given: `analyse_tests.py <folders or files> -o <results>`. A results file
open in Excel is reported, not crashed on.

**What it computes:**
- Pairs the rover's and the base's file of each session by the session ID
  in the file names (one file alone still works; receive-side numbers are
  then missing).
- One row per message copy (each `GEN` × each enabled path) in
  `messages.csv`: `outcome` delivered / failed / skipped / unknown (+ `why`),
  `rtt_ms`, `tries`, the receiver's `rssi` / `snr` (its `RX` of that
  `seq` + path), `distance_m`, `area`, `situation`, the phone time.
- Distance: the rover's GPS fix nearest in time (within 10 s) to the base's
  position = the median of the base phone's fixes with accuracy ≤ 30 m (or,
  without a base file, the last `pos` the base phone sent).
- Labels: the rover's last `label` record before the message.
- `summary.txt` per session: test / paths / power / firmware, how each board
  stopped, battery start → end (STAT) and the noise floor, distance range,
  then tables of made / delivered / failed / skipped / rate / round-trip
  median and 95th percentile: by direction and path, by size, by area /
  situation (when labelled), by 250 m distance band; LoRa RSSI / SNR medians
  per direction; why copies weren't delivered; the notes.
- `sessions.csv`: one row per session, direction and path (for the report's
  tables).
- Rate = delivered ÷ (delivered + failed): skips are excluded, as §3 says.

**Not built yet** (from §4.6): clock alignment and one-way latency; Wilson
confidence intervals; the first-try rate as a column (`tries` is in
messages.csv); packet loss per type from TX / RX counts; LoRa availability,
lost / restored counts and timings (LINK); relay rescue; the path-loss fit;
airtime, channel-wait and noise tables; every graph and the map; the
run-down curves (from `test dump`); `summary.md`.

## 13. First results and known issues (14 October)

**Desk run** (session 20261014-163618): baseline, both paths, 17 dBm, the
boards about 1 m apart indoors (rover DEA, base ABC).
- Delivered: LoRa 239 / 239, relay 239 / 239, every one on the first try.
- LoRa round trip (median): 383 / 475 / 599 / 825 ms for 5 / 25 / 50 / 100
  characters, i.e. the two airtimes (message + receipt) and little else.
- Relay round trip: median about 1.0–1.1 s, 95th percentile 2.2–3.0 s, the
  same for every size.
- RSSI median −60 dBm, SNR 11 dB; noise floor −113 / −115 dBm.
- Battery over 38 minutes: rover 4094 → 4091 mV (91 → 90 %), base 4148 →
  4130 mV (95 → 94 %).

**Known issue (app; fix before the next baselines if possible):** the base
made 119 messages, not 120. The rover finished first (`STOP reason=done`),
and the rover phone then sent `stop` to the base phone, so the base board
stopped 10 s before its 120th message (its 100-character count came out 29
instead of 30). Fix in `app_state.dart` `_onTestLine` (case `STOP`): don't
forward the stop to the base when the reason is `done` (in a baseline each
board finishes by itself).

**Also noted:** the app's `carry` is fixed to `hand` (§2.4); the
transmit-power field in the app is only a note: the board's real power is
whatever `radio power` set, and the board writes it in its `START` line
(`tx_dbm`).

## 14. Power and motor tests (agreed 14 October; firmware v0.5.1)

Added at the owner's request: what each part of the board costs in power,
and how the vibration motor actually behaves (an idea from the owner's
professor). Two independent methods give the power figures, so each checks
the other:

| Test | What | When | Needs |
|---|---|---|---|
| **P1. Relay night** | run-down B, paths = relay, about 10 h from full | night of 14 → 15 Oct | v0.5.1 |
| **P2. Motor night** | the motor board in `rundown-m`, the other board in `rundown-a`, about 10 h from full | night of 15 → 16 Oct | v0.5.1 + the temporary motor circuit |
| **P3. Current per state** | a multimeter in series with the battery while the board steps through its states | Fri 16 Oct, campus | v0.5.2 (`bench power`, to build) |
| **P4. Motor vibration** | the phone's accelerometer (phyphox) on the motor | Fri 16 Oct | v0.5.2 (`bench motor`, to build) and the motor circuit |
| **Run-down A** | both boards listening, Bluetooth off, until they switch off (§2.3) | Fri 16 Oct evening → Mon 19 Oct | v0.5.1 |

No LoRa-only night: at test rates LoRa adds roughly 2 mA to a board drawing
roughly 55 mA (estimates; P3 measures both), under 1 % of the battery in a
night, which the battery-curve method can't resolve. P3 measures it
directly.

**Every run-down from now on runs on v0.5.1** (both boards), so all logs
have the same format (§14.4).

### 14.1 Method 1 — current per state (P3)

**Why a meter.** A laptop can't measure what a USB device draws: it only
knows the figure the device *declares*. The board can't either: the
AXP2101 reads voltages only (§7 of HANDOVER).

**Set-up.**
- The meter goes **in series with the battery**: battery out of the holder,
  clip leads from the battery to the holder's contacts with the meter in
  one lead. Polarity checked twice (+ to +); the clips must never touch
  each other (an 18650 short is dangerous).
- Meter on DC current, the **mA socket** if its label allows at least
  400 mA, otherwise the A socket (coarser, still fine for the motor).
- **USB unplugged.** With USB in, the board runs from USB and the battery
  (and the meter) see nothing. So the board steps through the states by
  itself (`bench power`, v0.5.2): type the command, unplug USB, watch.
- **Don't turn the meter's dial while the board runs**: switching ranges
  breaks the circuit for a moment and the board restarts.
- Before starting, note the battery voltage once (`status` just before
  unplugging, or the meter on volts). It changes by a few mV in a 10-minute
  session, so one reading serves all states.

(v0.6.1: the meter session is started from the app, with USB never
connected, and has more steps: §17.7 replaces this procedure and the sheet
below.)

**The states** (about a minute each; the board LED blinks the step number
before each step and is dark during it):

| Step | State | Isolates |
|---|---|---|
| 1 | processor only: radio paused (standby), Bluetooth off | the cost of being on at all |
| 2 | + LoRa listening (the normal idle; = run-down A) | LoRa receive |
| 3 | + Bluetooth connected to the phone (app open nearby) | Bluetooth / the relay |
| 4 | LoRa transmitting: a few seconds of continuous carrier at 17 dBm, then 20 dBm (counted in the 10 % airtime budget) | transmitting, per power level |
| 5 | motor M1 at 100 %, then 50 %; M2; both together | the motors |
| 6 | switched off (`F`); wake with the 2 s hold afterwards | the drain while off (§2.3 had this as future work) |

**Recording.** By hand on a fill-in sheet (one row per step: typical value,
lowest, highest). Steady states show one number. For states that flicker
(Bluetooth, keepalives) note the typical value and the range, or film the
display for 30 s with the phone; use the meter's MIN/MAX or AVG button if it
has one. A photo of the sheet is enough to hand over.

| Step | State | Typical mA | Low | High | Notes |
|---|---|---|---|---|---|
| 1 | processor only | | | | |
| 2 | + LoRa listening | | | | |
| 3 | + Bluetooth connected | | | | |
| 4a | transmitting, 17 dBm | | | | |
| 4b | transmitting, 20 dBm | | | | |
| 5a | M1 100 % | | | | |
| 5b | M1 50 % | | | | |
| 5c | M2 100 % | | | | |
| 5d | both 100 % | | | | |
| 6 | switched off | | | | |

Battery voltage at the start: ______ mV. Meter model and range: ______.

**Computed.**
- Power per state: `P = V_battery × I`. Power is the figure to report: as
  the battery runs down its voltage falls and the board draws slightly more
  current for the same work (its main loads sit behind a switching
  regulator), so power stays nearly constant while current doesn't.
- Each part's cost: the difference between consecutive steps (2 − 1 =
  LoRa receive, 3 − 2 = Bluetooth, 4 − 2 = transmitting, 5 − 2 = a motor).
- Energy per message: transmit power × airtime (§5: 0.17–0.60 s per message,
  0.14 s per receipt).
- Battery life for any usage mix: `hours = E_battery ÷ P_average`, with
  `P_average` built from the states and the time spent in each (e.g. a day
  of listening with Bluetooth, N messages, M minutes of motor). `E_battery`:
  rated 3000 mAh × 3.7 V ≈ 11.1 Wh, or better the measured value from
  run-down A: `E = P_step2 × T_A` (§14.2), which also tells whether the
  cells still hold their rated capacity.

### 14.2 Method 2 — battery curves (P1, P2, run-down A)

**Idea (the owner's).** Every run starts from a full battery. Run-down A
runs to empty, so its voltage curve, with time as a stand-in for charge
used (its draw is nearly steady), maps "voltage" to "fraction of the
battery used". Each shorter night's voltage is placed on that curve: how
fast it moves along it gives that night's draw relative to listening, and
stretching A's curve by that ratio extrapolates the full curve for that
case.

**Rules for every run.**
- Start from full: charge LED off on both boards, then unplug USB.
- **Each battery stays in its own board all week** (DEA, ABC): each night is
  compared with that battery's own A curve.
- Usual checklist (§7): same firmware, silent mode on, `radio power 17`.
- Stop **without USB** (USB starts charging): from the rover phone (P1), or
  the 5 s hold on the board's key (P2), then plug in.
- Then `test dump` on each board and save the text (copy it from the
  serial monitor into a `.csv` file named after the board and the night).
  **Starting a run-down wipes the last log**: dump before the next one.
- Charge both to full before the next run.

**P1. Relay night** (14 → 15 Oct). Both phones on chargers next to their
boards, app open; the base phone's app on screen when the session starts
(§7 of HANDOVER). On the rover phone: Test **run-down B**, Paths **relay**,
Start. About 10 h, then Stop on the rover phone. Isolates Bluetooth
connected + relay traffic (LoRa keepalives as in A). Bonus: about 2,400
relay messages per board over the night (each board sends every 10–20 s),
a long reliability check of the relay; the phones' session files hold the
messages and a battery reading every minute (STAT).

**P2. Motor night** (15 → 16 Oct). The temporary motor circuit on one board
(§14.3); motor intensity and reading speed left at their defaults (100 %,
speed 5) unless noted. Over USB, before unplugging: on the motor board
`test start rundown-m`, on the other `test start rundown-a` (both boards
linked, link up). Unplug both. The motor board plays "PARIS" on the motors
over and over (one round = 50 units = 10 s at speed 5; on about 44 % of the
time), Bluetooth off after 3 s, as in A. The other board gives a same-night
listening curve (a repeatability check against the weekend's A). The motor
buzzes all night: put it on something soft, away from where anyone sleeps.
Note how many motors are wired. Stop each board with the 5 s hold.

**Run-down A** (Fri 16 → Mon 19 Oct). Both boards, from the rover phone
(Test run-down A) or the console; until both switch themselves off; then
charge and `test dump` both. Two curves (two boards, two batteries).

**Computed.**
- Use samples with `charging = 0` only.
- A's curve per board: `f(t) = t ÷ T_A` against the voltage (smoothed: a
  running median over 15 minutes, since the reading moves in single mV).
- Each night: map each sample's voltage to `f` on the same board's A curve;
  the slope of `f` against time (a straight-line fit, after the first 30
  minutes, when a fresh charge has settled) is the draw as a fraction of
  the battery per hour. Relative to listening: `r = slope × T_A`.
- In mA / mW: `r × P_step2` from P3. Battery life in that case: `T_A ÷ r`.
- The motor's share per hour of buzzing: the extra draw divided by the
  measured on-time (`m1_ms`, `m2_ms`, §14.4), to compare with P3's motor
  current.
- Resolution: roughly ±1–2 % of the battery per night (voltage readings,
  temperature, the flat middle of the curve). Expected to show Bluetooth
  and the motor clearly; LoRa's share is below it (method 1 only).
- Limitations for the report: the nights cover only the top ~20 % of the
  battery, so their extrapolation assumes the rest behaves like A; one
  battery per board, of unknown age; indoor temperature, not controlled.

### 14.3 Motor vibration (P4)

**Aim.** The spec assumes the motor (Pololu #1638, ERM) takes 30–50 ms to
spin up and 50–80 ms to coast down, so that units much below ~120 ms smear;
the fastest reading speed is a 120 ms unit (speed 9). P4 measures: spin-up
and coast times, strength at each intensity setting, M1 against M2, the
buzz frequency, and the fastest speed at which dots and gaps stay distinct
**for the motor** (a person may need more: future work, §9).

**The temporary circuit** (as in the hardware design): an NPN transistor
per motor, 1 kΩ from **GPIO25** (M1; GPIO13 for M2) to its base, the motor
from the board's 3.3 V pin to the collector, a diode across the motor
(cathode to 3.3 V). Never the motor straight from a pin (60 mA running,
120 mA starting). **No firmware change is needed:** in the test setup the
motor outputs are driven as well as the LED (`board.h`). First check:
`speed 5`, then `play 5` (five short buzzes).

**Set-up.** The phone flat on a folded towel (so it can move; a hard table
damps it), the motor taped firmly at the middle of the phone's back (thin,
hard tape: foam tape damps), its wires loose so they don't pull. The same
spot and tape every time. phyphox (free, Android), experiment
"Acceleration (without g)"; note the sample rate it shows. For the
frequency: phyphox "Audio Spectrum" with the phone's microphone next to
the motor.

**Procedure.** `bench motor` (v0.5.2) plays a fixed pattern: a 1 s start
buzz (to line the recording up), steady 3 s buzzes at 25 / 50 / 75 / 100 %
with 2 s of silence between, then for each speed 9 down to 1, and three
faster units (100, 80, 60 ms), ten dots (1 unit on, 1 unit off) and ten
dashes (3 units on, 1 off) on one motor, 2 s apart. Start the phyphox
recording, run the command, stop the recording after the end buzz, export
as CSV. Once per motor (M1, then M2 moved under the phone). Optional: with
the meter connected (P3 set-up), note the current during each steady buzz.
Fallback without v0.5.2: `motor 255 0` / `motor 128 0` / `motor 0 0` for the
steady buzzes and `speed N` + `play 55` for dot trains, typed with pauses
of a few seconds (the script then finds the bursts by silence, in order).

**Computed** (analysis script, from the phyphox CSV):
- The envelope: the size of the acceleration vector, as an RMS over 10 ms
  windows; the noise level from the silent stretches.
- Spin-up: from the command (known from the pattern, aligned on the start
  buzz) to 90 % of the steady level; coast: from the "off" to 10 %.
- Strength per intensity: the steady level (median of the middle second),
  relative to 100 %.
- Per speed: does a 1-unit dot reach 90 % of the steady level, and does a
  1-unit gap fall below 25 %? The fastest speed passing both is the motor's
  limit.
- Frequency: the audio spectrum's peak (× 60 = rpm).
- Limitations: the accelerometer samples at a few hundred per second, so it
  sees the envelope, not the buzz itself; the strength is "motor on a
  phone", not on a forearm, so only relative values are reported.

### 14.4 Run-down log format (v0.5.1)

`test dump` prints, for every run-down (A, B, M):

```
# morselink-rundown board=DEA peer=ABC test=rundown-m paths=both role=rover session=console fw=0.5.1 tx_dbm=17 speed=5 unit_ms=200 m1_pct=100 m2_pct=100 motor_log=1 end=hold samples=612
# one sample a minute; m1_ms/m2_ms = motor on-time in that minute; ended: hold
minute,mv,charging,m1_ms,m2_ms
0,4168,0,1250,500
1,4160,0,26750,10750
...
```

- The first line holds the run's details as `key=value` words (stored when
  the run starts, so they are the settings during the run): the board and
  its partner, the test and its paths, the session (`console` when started
  from the console, otherwise the phone's session ID), the firmware, the
  transmit power, the reading speed and the motor intensities, whether the
  motor on-time was kept (`motor_log=1`: rundown-m), how the run ended and
  the number of samples. `end=none`: it never stopped: the battery ran out,
  the board restarted, or it is still running.
- One row a minute from 5 s after the start (the first row is the full,
  unloaded battery). `mv`: battery voltage; `charging`: 1 while charging.
- `m1_ms`, `m2_ms`: how long each motor output was on during that minute,
  in 250 ms steps (the remainder carries into the next minute, so the
  total is exact). Counted at the output pin, so they say what was
  commanded: a motor that isn't wired still counts. 0 in every run-down
  that plays nothing.
- In rundown-m the battery is read only while both motors are still (a
  running motor pulls the voltage down a little).
- A log recorded by older firmware (v0.5.0) still dumps, with
  `fw=older end=unknown` and motor columns of 0 (nothing played then).
- Storage: NVS namespace `rundown`; `m###` blobs beside the `b###` ones for
  rundown-m; a 3-day run uses about 430 of the ~630 NVS entries available,
  a 12-hour rundown-m night about 145 (store.h).

### 14.5 Built so far

- **v0.5.1 (14 Oct, late):** `rundown-m` (console start; the app's test
  chips are unchanged), the dump header, the motor on-time columns, the
  run-down end reason. No pin or `board.h` change. Host checks 44,004
  (+5 for `rundown-m`); QEMU step "rundown-m plays PARIS; the dump has the
  header and each motor's on-time".
- **To build (now part of v0.6.0, HANDOVER §5.4–5.5):** `bench power` and
  `bench motor` (console only; they don't touch the run-down code), and the
  analysis: the dump reader, the A-curve mapping, the meter table and the
  phyphox reader. The dates in the table at the top of §14 are superseded
  by §15.4.

## 15. Field results of 14–15 October, and the revised plan

All at SF9, 17 dBm, firmware v0.5.1, app v0.1.0. The owner's files are in
the handover package (`field-data/`).

### 15.1 P1 "relay night" (session 20261015-002416)

- **What ran:** run-down B with **paths = both** (the plan said relay; the
  app's default chip was left on). So it is the original "busy" run-down
  B: Bluetooth connected, LoRa and relay test traffic. Boards 2 m apart on
  the owner's desk, 00:24 → 12:25 (721 minutes), stopped from the phone.
  A desktop PC and speakers were in use nearby part of the time.
- **Dumps:** complete: 721 samples per board, minutes contiguous, no
  charging rows, `end=app`, motor columns 0.

| | DEA (rover) | ABC (base) |
|---|---|---|
| battery, first → last sample | 4157 → 3965 mV | 4156 → 3970 mV |
| board's own % (voltage table) | 97 → 73 % | 97 → 75 % |
| slope after the first 30 min | −14.0 mV/h | −13.2 mV/h |
| messages made | 2892 | 2868 |

- Roughly a quarter of the battery in 12 h of busy use (from the generic
  voltage table; run-down A will give the real curve): about 2 days on a
  full charge in this mode, ~60 mA average (estimate). The two boards
  tracked each other within a few mV all night.
- **Delivery:** 5,760 / 5,760 per path, both directions. LoRa first try
  99.6 % (11 of ~2,880 messages per direction needed a retry: 9 two tries,
  2 three; both boards transmit at random times, so occasional overlaps are
  expected); relay: every copy on its only try. 1 damaged packet (RXBAD)
  in 12 h.
- **Latency:** LoRa round trip 392 ms (5 characters) to 824 ms (100), in
  steps of ~20.5 ms every 4.5 characters: exactly SF9's coding blocks (5
  symbols of 4.1 ms carry 4.5 bytes). Median 618 ms, 95th percentile
  805 ms. Relay: median 853 / 914 ms, 95th percentile ~1.7 s (better than
  the desk run).
- **Signal:** RSSI median −33 / −32 dBm, SNR 11 dB. Noise floor −114 /
  −115 dBm, but the base read about −101 dBm in 44 of 720 minutes (and
  above −108 in 249): local interference, likely the PC / speakers.
- **Stability:** Bluetooth connected in all 720 STAT lines on both boards,
  no MQTT drop, no log gaps, free heap flat at ~114 KB, airtime peak 3.6 %.

### 15.2 Dynamic session in a mall (session 20261015-185651)

- Base at a fixed spot; rover carried ~400 m to and into a mall (labels
  urban / walking, urban / indoors, dense urban / indoors), 65 minutes.
  Batteries started at ~3.93 V.

| Direction / path | made | delivered | failed | skipped | rate |
|---|---|---|---|---|---|
| base → rover LoRa | 262 | 76 | 4 | 182 | 95.0 % |
| rover → base LoRa | 263 | 76 | 8 | 179 | 90.5 % |
| base → rover relay | 262 | 259 | 3 | 0 | 98.9 % |
| rover → base relay | 263 | 261 | 2 | 0 | 99.2 % |

(Rate excludes skips. LoRa skips were all `lost`.)

- **Timeline (rover):** walking outdoors, every LoRa message delivered;
  from minute 6 retries rose (3, 4, 5 tries): the "cliff" warning of §8
  hypothesis 2. From minute 8.5 failures; indoors LoRa was down for ~13
  minutes (12.7 → 25.3), came back while closer (25–30 min, deliveries
  with retries), then down ~30 minutes (30 → 59.5); back outdoors it
  recovered within a minute.
- **Signal at the edge:** RSSI median −110 / −112 dBm (min −115 / −116),
  SNR median −1.5 / −0.2 dB, min −14 dB, below SF9's −12.5 dB limit. 33
  damaged packets (RXBAD) per side: the signal reached the boards but too
  weak to decode. Of 96 keepalives each board sent, the rover heard 4 and
  the base 1.
- **By label:** dense urban / indoors: LoRa 37 delivered of 388 made (346
  skipped); urban / walking: 103 of 112. Relay: 100 % indoors.
- **The reporting gap:** no `LINK state=lost` line was ever logged (only
  `restored`), and the app showed "Partner: connected" throughout:
  HANDOVER §5.1 has the cause and the fix. For this session the lost
  periods are rebuilt from the failed L copies and `restored` lines.

### 15.3 Range settings: comparison sessions (after v0.6.0)

Agreed 15 Oct: SF becomes a setting (9 default, 10, 11, 12) and transmit
power goes up to 22 dBm (HANDOVER §5.2). For the report, the gain is
measured, not assumed:

- **Mall walk again**, same base spot (record it; higher and by a window
  helps but must then stay the same for all settings), same route and
  labels, dynamic test: SF9 / 17 dBm (done, §15.2), then **SF11 / 22 dBm**
  and **SF12 / 22 dBm** (the owner's "full capability"); optionally SF9 /
  22 dBm to separate power from SF. The test pace scales with SF
  (HANDOVER §5.2), so expect fewer messages per half hour; compare LoRa
  availability, delivery, RSSI / SNR margin and latency per area.
- **Edge point (optional):** a fixed spot where SF9 just fails; a short
  baseline (fewer messages at high SF) at each setting.
- Report: range / availability gained per dB, against airtime and latency
  lost (a 100-character message: 0.6 s at SF9, 4.3 s at SF12), and the
  duty-cycle limit (~80 long messages an hour at SF12).
- Before every session: both boards on the same SF (the app will warn),
  `status` shows SF and power.

### 15.4 Revised schedule (proposal, 15 Oct)

- **Nights** (each adds one thing to A; TEST_PLAN §14.2): A (listening
  only) short night when possible; **long run-down A to empty moved to next
  week** (owner: run while writing the report); BL = run-down B paths =
  lora (Bluetooth + LoRa traffic) for Bluetooth's cost; M = `rundown-m`
  with the motor; L (LoRa only, Bluetooth off) optional, needs the v0.6.0
  option. LoRa traffic and relay traffic are each < 1 % of the battery per
  night: below what a voltage curve resolves, so the meter measures them.
  B (busy) is done (§15.1).
- **Meter session** (§14.1) once `bench power` exists; **phyphox motor test**
  (§14.3) once the motor circuit and `bench motor` exist.
- **Baselines** at 10 m and ~200 m still to do (40 min each at SF9).
- Keep boards ~1 m from PCs, speakers and chargers during runs.

## 16. Version 0.6.0 and app v0.2.0 (16 October): what changed for the tests

Both boards must run v0.6.0 (and both phones app v0.2.0) from now on: the
path reports, the range level and the new log lines need both sides.

### 16.1 Losses are logged

The mall session's gap (§15.2) is fixed (HANDOVER §5.1): every loss of the
link is one `LINK state=lost` line with its reason (`why=silence`,
`no-receipt` or `one-way`), and every change of a path a `PATH` line (§4.4).
During a test the link is LoRa alone (the test sends its own relay copies),
so the `LINK` lines are LoRa's outages. A test that sends over LoRa is
refused (`REFUSED reason=lora-lost`) while LoRa itself is down, even if the
relay keeps the link up in normal use.

### 16.2 Range level and power in the logs

`START` has `sf`; the dump header has `sf=` (a v0.5.1 log reads as 9). The
phones' session records have the board's real `tx_dbm`, `sf` and `bw_khz`
(from the board, no longer typed) and `peer_radio` (the other board's, as
its phone reported). The app warns before Start if the two boards' levels
differ, and the base phone says so too.

### 16.3 The pace follows the SF

So that the airtime stays near 5 % (the host checks: 3.9 / 4.3 / 5.4 /
5.5 % per board at SF9 / 10 / 11 / 12, dynamic):

| SF | Baseline: one message every | Baseline messages per board | Dynamic / run-down B / L: a gap of |
|---|---|---|---|
| 9 | 20 s | 120 (40 min) | 10–20 s |
| 10 | 30 s | 120 (60 min) | 20–40 s |
| 11 | 45 s | 120 (90 min) | 40–60 s |
| 12 | 90 s | 60 (90 min) | 90–130 s |

The base starts half a grid after the rover, as before. A baseline at SF12
has 15 messages of each size instead of 30: enough for a delivery rate per
size, coarser for its round trip.

### 16.4 rundown-l and the bench tests

- **`rundown-l`** (the app's chip, or `test start rundown-l`): test messages
  over LoRa only, at the dynamic pace, with Bluetooth off (as run-down A).
  Against A it gives LoRa traffic's cost; against B, Bluetooth's. Expected
  to be small (§14), so the meter (P3) remains the main figure.
- **`bench power`** (P3, §14.1) as built in v0.6.0 (v0.6.1 changed it:
  §17.7): type it, unplug USB; 5 s after
  USB is out (or after 2 minutes anyway) the steps run: 1 processor only,
  2 + LoRa listening, 3 + Bluetooth (the phone reconnects by itself) for
  60 s each; 4 a carrier at 17, 20 and 22 dBm (22 only on the SX1262),
  10 s each; 5 the motors (M1 100 %, M1 50 %, M2 100 %, both) 15 s each;
  6 switched off (hold the key 2 s afterwards). Before each step the LED
  blinks its number (0.3 s on, 0.3 s off, then 2 s dark); between the parts
  of a step one short blink. About 5 minutes in all (the console prints the
  plan with the exact total). A 5 s hold or `bench stop` ends it early and
  puts everything back. The carrier counts in the 10 % airtime.
- **`bench motor [m1|m2]`** (P4, §14.3) as built: on one motor, a 1 s start
  buzz, 2 s silence; 3 s buzzes at 25 / 50 / 75 / 100 % with 2 s between;
  then for the units 120 ... 320 ms (speeds 9 to 1) and 100, 80, 60 ms:
  ten dots (1 unit on, 1 off), 2 s, ten dashes (3 on, 1 off), 2 s; a 1 s
  end buzz. About 3.2 minutes. A press or `bench stop` ends it.
- Run-down meta and `test dump` are unchanged otherwise (§14.4).

### 16.5 The analysis (`tools/analyse_tests.py`)

- **Lost periods and availability** per board ("LoRa link" in
  summary.txt, `lost.csv`): from the `LINK` lines since v0.6.0; for older
  files rebuilt as HANDOVER §5.1f says (down from a failed L copy, or the
  lost time after the last packet heard, until the next `restored`). The
  mall session gives LoRa up 32 % (rover) / 31 % (base) of 66 min, the two
  long outages 12.7–25.3 and 30.0–59.5 min, as §15.2 found by hand. Time
  per path state from the `PATH` lines; availability by the rover's labels.
- **By range setting:** every session's figures grouped by test, SF, power
  and path at the end of summary.txt (for §15.3's comparison walks); `sf`
  and `tx_dbm` columns in messages.csv and sessions.csv.
- **Sizes in four bands** (5–25 / 26–50 / 51–75 / 76–100) for the dynamic
  tests; a **first-try** column (delivered on the first copy, of delivered
  + failed); damaged packets (`RXBAD`) per board.
- **Run-downs:** the `test dump` text, saved as a file among the session
  files, is found by its header line (the monitor's own text and escape
  codes around it don't matter); per dump the start and end voltage, the
  slope after the first 30 minutes (the relay night: −13.2 / −14.0 mV/h),
  hourly medians and the motor on-time; `rundowns.csv` has every sample.
  Once a board has a run-down A to empty (`end=none`, last reading below
  3.65 V), each of its other runs is placed on that curve (§14.2): % of the
  battery used per hour, the draw relative to listening, and the battery
  life it implies.
- Still to come: graphs, the meter table (P3) and the phyphox reader (P4),
  once their data exist.

## 17. Point tests and the meter session (v0.6.1 + app v0.2.1, agreed 20 October)

### 17.1 Why point tests

The dynamic walks and drives (§15, 18 Oct) found the edge of range (in
town LoRa went from 97 % delivered within 250 m to 0 % from 500 m) but mix
distance, obstacles and settings together, and one run at a new setting
"didn't feel like it did much". Point tests give fixed, repeatable numbers
behind them: what a concrete pillar, a brick wall, glass, a car body and a
floor take off the signal; how the signal falls with distance in the open
(the path-loss exponent); and at every spot what SF and power change.
Both boards stand still, so the only thing that changes during a test is
the setting.

### 17.2 What a point test does

- **Started from the rover phone** like the other tests (Tests > `point`);
  the base phone follows over MQTT and starts its board.
- **The plan:** *Single* (one range level and power) or a *Sweep* (a list
  of them, up to 8). The default sweep (agreed 20 Oct) is **SF9 / 17 dBm**
  (the setting of the earlier field tests), **SF9 / 22 dBm** (what the power
  alone gains) and **SF12 / 22 dBm** (the most range).
- **Each step**, on each board: the setting is switched (nothing sent for a
  *settle* time of 10 s at SF9-10, 20 s at SF11-12, which absorbs the 0.1-2 s
  by which the two boards' starts differ), then **20 messages of 25
  characters** from each board over LoRa, one *gap* apart, the base's half a
  gap after the rover's, then one more gap so the last message has its
  result before the step ends.
- **The gaps** are 15 / 20 / 25 / 40 s at SF9 / 10 / 11 / 12: longer than a
  message that fails takes to give up (5 copies and the receipt wait; in the
  simulator at most 11.8 / 13.5 / 19.8 / 33.0 s). So every message is a try
  of its own: a failing message never takes the next one down with it (the
  board fails every message waiting behind one that failed). The handover's
  10 / 15 / 20 / 30 s were shorter than that and would have made the edge
  look worse than it is.
- **Step lengths:** SF9 5 min 25 s, SF10 7 min 10 s, SF11 9 min 5 s, SF12
  14 min 20 s. **The default sweep takes 25 min 10 s.**
- **No packets coordinate the boards:** both get the whole plan in their
  START and compute the same timetable from it (`ttest.c`, host-tested).
- **Settings for the test only:** each board switches by itself, never
  saves, and goes back to its own setting at the end, at a stop of any kind,
  and (stored settings) at a restart. Nothing to set by hand; `radio sf` and
  `radio power` are refused while a test runs.
- **It keeps sending while the link is lost:** the other tests skip LoRa
  copies as `lost` while the link is down; a point test sends its 20
  regardless (the delivery at the edge is the measurement). The user's
  messages still can't be sent while lost.
- **LoRa only:** no relay copies (the relay doesn't measure anything here).
  Bluetooth stays on (the phones log).
- **Airtime:** a default sweep, every message delivered at the first try,
  uses about 95 s of airtime per board (most of it the SF12 step): two
  sweeps an hour stay near 5 %. At the edge, with retries, more: the 8 %
  skip rule may then skip messages (`lora_skip=duty`, counted in the
  analysis). So **at most two sweeps an hour**, which back to back is what
  25-minute sweeps give anyway.
- **Logs (§4.4):** START adds `plan=9:17/9:22/12:22,n=20,size=25` (`/`
  between the steps: the fields are comma-separated); each step a
  `STEP,i=<n>,sf=..,dbm=..,n=..,step_ms=..` (the power this board really
  uses), and at the end `STEP,i=0,sf=..,dbm=..,restored=1`; GEN adds
  `step=<n>` (and `late_ms` in the rare case a message had to wait for the
  one before). The label lives in the app: the session record (`label`),
  the base's copy (from the start message), and a `label` record if it is
  edited during the test.

### 17.3 At each spot

1. Both boards **1 m above the ground, at the same height, antennas
   upright**; each phone at least 1 m from its board.
2. **The base phone on Wi-Fi and on its charger**, MorseLink's battery use
   Unrestricted, the app open (the three lost runs of 16-18 Oct were all the
   base phone's internet).
3. Both boards linked (`S`), the link up, silent mode on (`Z`), both on the
   same range level (`G`; the steps then set their own).
4. Rover phone: Tests > `point`, type the **label** (§17.4), keep *Sweep*
   (the owner, 20 Oct: a sweep at every spot, not just at the edge) or pick
   *Single*, Start, read the confirmation, Start.
5. **Wait for "Base phone: running"** before walking away. If it hasn't
   answered within 20 s the app shows a red notice: the base board is NOT
   running the test. Fix the base phone (internet), Stop, start again.
6. Don't touch or move the boards until it ends. The page counts down each
   step, shows its counts, how this board hears the other ("We hear them")
   and how the other hears this one ("They hear us", from the base phone).
7. At the end both boards go back to their own setting by themselves.
   **Repeat at the next spot** asks for the next label and starts the same
   plan.

### 17.4 The spots and their labels

The analysis reads the labels (`tools/analyse_tests.py`):

- **An obstacle and its reference** share the words before the first comma;
  the reference has **clear** in it. Same distance between the boards in
  both. The loss = the reference's RSSI − the obstacle's, per setting and
  direction.

  | Obstacle | Reference (clear) | Obstacle |
  |---|---|---|
  | concrete pillar | `pillar A, clear, 12 m` | `pillar A, behind, 12 m` |
  | brick wall | `wall A, clear, 10 m` | `wall A, behind, 10 m` |
  | window / glass | `glass A, clear, 8 m` | `glass A, window between, 8 m` |
  | a car body | `car A, clear, 5 m` (next to the car) | `car A, inside, 5 m` (rover in the closed car) |
  | one floor | `floor A, clear, 6 m` (both on the same level, 6 m apart) | `floor A, one floor up, 6 m` (as far apart, one floor between) |

  A second pillar is `pillar B, ...`. One clear reference serves every
  obstacle with the same prefix (e.g. `wall A, behind, 10 m` and
  `wall A, two walls, 10 m`).
- **The distance ladder in the open:** labels with **open** and the
  distance: `open, 50 m`, `open, 100 m`, `open, 200 m`, `open, 400 m`. Pace
  the distance out or use a map; without a number in the label the analysis
  takes the two phones' GPS (both still: each one's median fix).
- **No hand-versus-pocket test** (the owner, 20 Oct): the device is a
  wearable.

### 17.5 Time budget

Each default sweep is 25 min, plus moving and setting up (about 5 min); at
most two an hour. Five obstacle pairs (10 spots) and four ladder spots are
14 sweeps, about 7 hours with the moving: **two sessions**. To save time: one
clear reference per group (several obstacles at one distance), or, if the
day runs short, a shorter plan at the obstacles (e.g. SF9 / 22 and SF12 / 22:
19 min 45 s), keeping the full sweep on the ladder.

### 17.6 The analysis (points.csv, summary.txt)

- One row per session, step and direction: label, setting (the power the
  board says it used), sent / delivered / failed / skipped, rate and first
  try, round trip, the RSSI and SNR (median and minimum) of everything the
  receiver heard from the sender during its own step, the **margin** = the
  median SNR − the SF's decoding limit (−12.5 / −15 / −17.5 / −20 dB), the
  distance (label or GPS) and the skew between the two boards' step starts
  (by the phones' clocks; it should stay well under the settle time).
- **Attenuation:** each obstacle against its reference, per setting and
  direction, and the **check**: the loss should come out the same at 17 and
  22 dBm (power moves both RSSIs alike); a spread of more than 2-3 dB means
  something else changed between the two runs (people walking past, the
  board moved).
- **The ladder:** RSSI against log10(distance), per setting and direction,
  fitted to RSSI = A − 10 n log10(d): n is the path-loss exponent (2 in free
  space; about 2.7-3.5 in town), with R².
- Graphs for the report (still to do): RSSI against distance per setting,
  and the loss per obstacle.

### 17.7 The meter session (P3; replaces the procedure of §14.1)

**Why from the app (the owner, 20 Oct):** to type `bench power` the board
has to be on USB, but on USB it runs from USB and the battery charges
*through the meter* (a few hundred mA: it can blow the meter's mA fuse). So
the phone starts it, and **USB is never connected**:

1. Battery full (the charge LED off), then USB out.
2. The meter in series with the battery (§14.1's set-up), on DC mA.
3. The motors and the key connected (the wearable build: the LED is off and
   adds nothing).
4. App: Tests > **Meter session** > Start. The phone stays next to the
   board with the app open (it reconnects in step 3b).

The board then steps through these states by itself (`bench.c`, the app
has the same list); each adds to or changes the one before. Before each new
step number, motor M1 buzzes it (0.3 s on, 0.3 s off; then 2 s still);
between the parts of one step one short buzz. The buzzes come between the
readings, never during one.

| Step | State | s | Isolates |
|---|---|---|---|
| 1 | processor only: radio standby, Bluetooth off | 60 | the base |
| 1b | + keying: you key PARIS over and over at your usual speed | 45 | keying (decoding, the key's pull-up) |
| 1c | + the key held: three holds of about 3 s (**not 5 s**: that stops the session) | 15 | the key held down |
| 2 | + LoRa listening at SF9 | 60 | receive |
| 3a | + Bluetooth advertising, not connectable | 60 | advertising |
| 3b | Bluetooth connectable: the paired phone reconnects | 60 | connected |
| 3c | LoRa to standby, Bluetooth still connected | 60 | cross-check: 3c − 1 should equal 3b − 2 |
| 3d | LoRa listening at SF12, Bluetooth connected | 60 | does receive depend on the SF? (expected: no) |
| 4a-4c | back to SF9; a carrier at 17, 20, 22 dBm (22 SX1262 only) | 10 each | transmit, per power (on top of 3b's state) |
| 5a-5d | motors M1 100 %, M1 50 %, M2 100 %, both 100 % | 15 each | the motors (on top of 3b's state) |
| 6 | switched off (as F); hold the key 2 s afterwards | - | the drain while off |

About 9 minutes (543.8 s from step 1's signal to switching off, plus 5 s
to let the meter settle). The app shows "now: step 3b, 32 s left" and what
to do; in steps 1-3a the board's Bluetooth is off (or not connectable), so
the app keeps time by itself, and from 3b on the board says each step (BN
lines, phone.h). **Read each step in its second half** (the meter settles,
and in 3b the phone needs a few seconds to connect). A 5 s hold stops the
session at any time; `Stop` in the app does too once the board is
connected. At the end the board puts back its own Bluetooth setting and its
stored range level.

`bench power` on the console still works (for a dry run): it prints the
plan and waits until USB is unplugged, however long that takes (v0.6.0
started anyway after 2 minutes; agreed 20 Oct: no time limit). A 5 s hold
or `bench stop` cancels it.

**The fill-in sheet** (replaces §14.1's):

| Step | State | Typical mA | Low | High | Notes |
|---|---|---|---|---|---|
| 1 | processor only | | | | |
| 1b | + keying PARIS | | | | |
| 1c | + key held | | | | |
| 2 | + LoRa listening (SF9) | | | | |
| 3a | + Bluetooth advertising | | | | |
| 3b | + Bluetooth connected | | | | |
| 3c | LoRa standby, Bluetooth connected | | | | |
| 3d | LoRa listening SF12, Bluetooth connected | | | | |
| 4a | carrier 17 dBm | | | | |
| 4b | carrier 20 dBm | | | | |
| 4c | carrier 22 dBm | | | | |
| 5a | M1 100 % | | | | |
| 5b | M1 50 % | | | | |
| 5c | M2 100 % | | | | |
| 5d | both 100 % | | | | |
| 6 | switched off | | | | |

Battery voltage at the start: ______ mV. Meter model and range: ______.

**Computed** (as §14.1, plus): keying = 1b − 1 (and the key held = 1c − 1:
the internal pull-up's 0.07 mA, if the meter shows it at all); receive =
2 − 1; advertising = 3a − 2; connected = 3b − 2; the cross-check 3c − 1 ≈
3b − 2; SF12 receive = 3d − 3b (≈ 0 expected); transmit per power = 4x −
3b; a motor = 5x − 3b.

### 17.8 What else changed in v0.6.1

- **The key is on GPIO14** (agreed 20 Oct): a push button to GND, the
  ESP32's internal pull-up only. GPIO4 is the board LED's pin (the LED lit
  on every press, ~2 mA). The default build is now the wearable one (the
  external key, the LED off); `board.h`'s switches bring back the board's
  button and the LED mirror, independently of each other. The motor driver's
  base resistor is 330 Ω (18 Oct).
- **`W`**: Bluetooth off / on from command mode (OK + OFF / ON), stored
  through restarts; the phone is told first (`EV BLE_OFF`) and the app tries
  only every 30 s meanwhile.
- **App:** a red notice when the base phone hasn't answered 20 s after
  Start; the rover's Stop is resent every 5 s until the base confirms it (for
  up to 2 minutes, and at once when the internet comes back); a stronger
  warning before Start when the base phone looks offline (silent for 10
  minutes and this phone's internet reconnected since).
- Phone protocol 3 (`phone.h`): the point test, `BENCH POWER` / `BENCH STOP`,
  the `BN` lines, `EV BLE_OFF`.
