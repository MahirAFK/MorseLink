# MorseLink — Design Specification v1.9

**Date:** 21 October 2026 (v1.0 handover: 1 October; v1.1: 2 October; v1.2: 6 October; v1.3: 7 October; v1.4: 8 October; v1.5 and v1.6: 13 October; v1.7: 16 October; v1.8: 20 October)
**Status (firmware v0.6.2, app v0.2.2):** steps 0–9 implemented, the field
tests running; see README. The user manual is `docs/MANUAL.md`.
**Deadline:** full project and report due 26 October 2026.

Items marked **[PENDING]** await a decision. Items marked **[impl]** are
implemented and tested.

---

## 0i. v0.6.4 (24 October): leaving command mode, `K`, `I` 1-9, the dash's shape

| Area | Decision | Why |
|---|---|---|
| Leaving command mode | in command mode the **3 s hold leaves it** (OK, draft kept, `CE left`); it used to restart the command | the owner: there was no way out but the 5 s hold, which also deletes the draft, or waiting 30 s |
| `K` | keep the draft: no 60 s discard until it is sent or cancelled (or emptied); `K` again restores the limit; OK + ON / OFF; error without a draft; in memory only (not across switching off) | drafts written during the sessions were lost after 60 s |
| `I` 1-9 | `I8` full drive at 3.3 V (default, as the board starts); `I9` full at 3.4 V (the old `I5`); `I1`-`I7` PWM at 45 / 52 / 59 / 66 / 74 / 82 / 91 % (each level ≈ a quarter less force; `I1` ≈ a fifth of `I8`), 3.3 V; **a 30 ms full-power kick** whenever a motor comes on below full drive; settings version 4 converts the stored level | a participant wanted it gentler than the old `I1`; DCDC1 can't go below 3.0 V (the ESP32), PWM can; the motor's recommended range is 2.5-3.5 V (Pololu #1638), so a gently driven motor may not start from rest without the kick. Timing unchanged (the kick is inside the element) |
| The dash | M1 **1.4U**, M2 **1.9U**, together **0.3U** (`dash_m1_cu` 125, `overlap_cu` 30); the overlap in hundredths of U (was 20 ms) | the owner's proposal: a longer, clearer handover from wrist to forearm, scaling with the reading speed |

---

## 0h. v0.6.3 (24 October): how dashes feel (`H`)

| Area | Decision | Why |
|---|---|---|
| Dash style | **`H1`** (default) the sweep M1 → M2 as before; **`H2`** a dash on **M2 alone** for 3U, dots on M1 alone; same timing; stored in the last former reserved settings byte (0 = never set = `H1`); console `haptic dash 1/2`; SP's 16th field | the owner's participant comparison: whether separating dots and dashes by place (wrist / forearm) as well as by length helps reading |

---

## 0g. Changes in v1.9 (21 October: the learning view for the participant sessions, vibration strength)

The owner's requests of 21 October, for the participant sessions (the
questionnaire JS-04: feeling the vibrations, keying, learning Morse with
the companion display): a live transcript of dots and dashes on the phone,
above each letter; the calibrated speeds on the display; a command for the
vibration strength through DCDC1. "Last press" (the timing bar) in.

| Area | Decision | Why |
|---|---|---|
| Live keying on the phone | **`KY`** on every press counted as an element: the letter so far (or, in calibration, the whole PARIS attempt), the press's length, the boundary it was judged by; **`KY`** alone when nothing is open; **`KC`** for each letter closed (char / unknown / error, with the pattern as keyed), before the `DR` it changes | the board already decides dot or dash on release (`dec_element`); the phone needs each press, not just finished letters; the keyed pattern explains a `?` |
| Patterns above letters | the app puts the keyed pattern above each letter it saw closed, the Morse table's for the rest (text back from a failed send) | `DR` carries only the letters, and a draft's patterns would not fit a line (100 × 7 characters) |
| Speeds | **`SP`** after HI and on change, at most once a second (`PHONE_SP_GAP_MS`): calibrated / calibrating, the learned dot, dash, gap inside and between letters, the boundary, the letter and word thresholds, the health, Q / unit / Farnsworth, the strength and the rail's mV; **`CB`** for each PARIS attempt (ok, or why not) | the speeds move with every letter; a learner needs to see why PARIS was refused |
| The app's speed figure | words per minute from the person's own timing (PARIS = 10 dots, 4 dashes, 9 inner gaps, 4 letter gaps, a word gap taken as 7/3 of their letter gap), not 1200 / dot | novices' dashes and gaps are far from textbook |
| What plays | **`PL i offset unit fx10 text`** per Morse part when a playback starts (offset from the start: `hjob_item_starts()`, pure, host-tested), **`PL end`** when it ends or is stopped; the app follows the letters with the firmware's own timing | one line per playback instead of one per letter; the host tests hold the app's timing model against the generator over 400 random jobs (element for element) |
| Reading by feel | app modes: letters + Morse / Morse only (letters after it ends) / hidden (Show afterwards); received messages hidden in the chat meanwhile, until tapped | the questionnaire asks whether people relied on the screen, and whether they could recognise characters without it |
| Vibration strength | **`I` 1-5** (command mode; console `haptic strength`): DCDC1 at **3.0 / 3.1 / 3.2 / 3.3 / 3.4 V**, `I4` = 3.3 V (as before); `I` alone plays the level; stored in a former reserved settings byte (0 = never set: DCDC1 left as the board starts it); refused (error) without a power chip | on the V1.2 DCDC1 also feeds the ESP32 (flash and PSRAM need ≥ 3.0 V) and the AXP2101's DCDC1 stops at 3.4 V. The range is narrow: roughly a quarter less force at `I1` than `I5` (estimate). The PWM trims (`haptic m1 / m2`) can go lower if ever needed |
| Phone protocol 4 | `KY`, `KC`, `CB`, `SP`, `PL` | older apps ignore new tags |

---

## 0f. Changes in v1.8 (20 October: point tests, the meter session from the app, W, the key on GPIO14)

The owner's decisions of 20 October (HANDOVER §2.0), after the field days
of 16–18 October (TEST_PLAN §17).

| Area | Decision | Why |
|---|---|---|
| Key pin | **GPIO14**, a push button to GND, the ESP32's **internal pull-up only** (no resistor) | GPIO4 is the board LED's pin (the LED lit on every press, ~2 mA); GPIO14 has nothing else on it, isn't a strapping pin, can wake the chip (the power switch) and has an internal pull-up. A pull-down would be worse on GPIO4 (a permanent path through the LED) and gains nothing elsewhere; a 10 kΩ external pull-up only if stray presses ever show up (noise from the motor wires) |
| Default build | the wearable build: the external key on GPIO14, the LED mirror off (`board.h`); `VERBOSE_LOG` separate (the packet and playback logs no longer come with the LED switch) | the bench tests run on the real motors and key; the LED would add its current to every reading |
| The LED's pin when unused | input with pull-up, also in deep sleep | its reset state (a pull-down) would leak ~35 µA through the LED, switched off included |
| Motor driver | 2N2222, **330 Ω** base resistor (18 Oct) | 1 kΩ was too weak for the motor's ~120 mA start-up |
| Point test | new test `point` (TEST_PLAN §17): both boards still; a plan of 1–8 steps (SF, power), the default sweep **SF9 / 17, SF9 / 22, SF12 / 22 dBm**; **20 messages × 25 characters** per board per step; LoRa only; sent even while the link is lost; settings for the test only, put back at the end; no coordination packets (both boards compute the timetable from the plan) | concrete numbers behind the dynamic tests: attenuation per obstacle, the path-loss exponent, the effect of SF and power, at every spot |
| Its gaps | 15 / 20 / 25 / 40 s at SF9 / 10 / 11 / 12 (not the 10 / 15 / 20 / 30 s first proposed) | a message that fails takes 11.8 / 13.5 / 19.8 / 33.0 s to give up; with shorter gaps the next message failed with it, unsent. A default sweep takes 25 min |
| Label | free text typed on the app's test page; pairs share the words before the first comma, the reference has "clear"; the ladder has "open" and the distance | the analysis pairs them by name |
| Meter session | **started from the app** (`BENCH POWER` / `BENCH STOP`, `BN` lines), USB never connected; the console's `bench power` stays (it now waits for USB to go, with no time limit) | on USB the board runs from USB and the battery charges through the meter |
| Meter steps | 1, **1b keying, 1c the key held**, 2, **3a advertising (not connectable), 3b connected, 3c LoRa standby with Bluetooth, 3d SF12 listening**, 4a–c carrier, 5a–d motors, 6 off; signals on **motor M1** | keying's cost; advertising vs connected; whether receive depends on the SF; a cross-check that the currents add |
| `W` | command-mode **Bluetooth off / on**, OK + OFF / ON (as `Z`); **stored** through restarts until `W` again (or `O`); the phone told first (`EV BLE_OFF`) | the owner's request; the default (stored) taken, to confirm |
| App robustness | a red notice when the base phone hasn't answered 20 s after Start; the rover's Stop resent every 5 s until the base confirms (2 min at most, and on reconnect); a stronger warning when the base phone looks offline | three lost runs (16–18 Oct) were the base phone's internet |
| Phone protocol 3 | `TEST START test=point plan=...`, `STEP` lines; `BENCH POWER` / `BENCH STOP`; `BN refused\|wait-usb\|start\|step\|end`; `EV BLE_OFF` | older apps ignore the new lines |
| Not done | the hand-versus-pocket test (the owner: it is a wearable); `cleanSession = false` for MQTT (optional; the resent stop covers the case that mattered) | |

---

## 0e. Changes in v1.7 (15–16 October: the connection-lost fix, range settings)

From the first field results (TEST_PLAN §15) and the owner's decisions of
15 October (HANDOVER §5).

| Area | Decision | Why |
|---|---|---|
| The link's paths | **two paths, each with its own state:** LoRa and the relay (both phones); states none / checking / connected / they-can't-hear / can't-hear-them / lost / off (the relay: no phone) | the mall session: the relay kept the link "up" while LoRa was dead, and nobody knew (owner: split it in two) |
| Proof both ways | a path is connected only when we hear the partner **and** the partner reports hearing us; keepalives carry "I last heard you N s ago" for each path (2 payload bytes; 255 = never) | a board that only hears its partner can't tell the partner doesn't hear it |
| Losses | the link is lost when **no** path is connected; every loss is **one** event with its reason: silence, no-receipt (a message failed), one-way | before, a failed message lost the link silently and the 90 s check then never fired |
| What the user feels | error + ID only when both paths are down (owner: option "1. a"); a loss by no-receipt plays no second alert; LoRa alone dropping while the relay carries plays nothing (the app shows it) | the alert means "messages don't arrive now" |
| Recovery | quick keepalives (0.2–1.2 s, rate-limited) when a path turns one-way or comes back | the partner learns at once that it is heard again |
| Range level | **`G 1-4` = SF9 / SF10 / SF11 / SF12** (stored; `G` alone plays it; console `radio sf 9-12`); set on each device by hand (owner), the manual says so at `G` | range against speed; the boards can't hear each other on different SFs, SOS included |
| Timing per SF | keepalive / lost after: 30 / 90 s at SF9–10, 45 / 135 s at SF11, 60 / 180 s at SF12; retry gaps, receipt waits and the listen-before-talk limit scale with the airtime (`link_timing()`; SF9's values are v0.5.1's) | at SF12 a packet takes up to 4.3 s |
| Transmit power | **22 dBm by default** (the SX1262's maximum; the SX1276 stops at 20); boards on 17 move to 22 once; other levels on the console only | owner: max power by default; lower power saves about 1 % of battery |
| SOS | on the SF both devices are set to (no extra SF9 copy: future work) | a copy on another SF isn't heard |
| Test pace | the gaps grow with the SF so the airtime stays near 5 % (TEST_PLAN §16.3) | |
| New test | `rundown-l`: LoRa-only run-down, Bluetooth off | LoRa traffic's cost against A |
| Bench | `bench power` (the meter session steps), `bench motor` (the phyphox pattern), `bench stop` | TEST_PLAN §14 |
| Phone protocol 2 | `ST` + each path, `RD` (radio setting), `CM` / `CE` (command mode), `EV` with a detail (LOST's reason, LORA / RELAY state) | the app shows the paths, the level and command mode |

---

## 0d. Changes in v1.6 (13 October, the phone link)

v1.0 §11 (here §12) as built. The owner decided the relay (both ways,
always) and who a general SOS goes to; the rest are defaults that can be
changed.

| Area | Decision | Why |
|---|---|---|
| Phone platform | **Android first** (owner); an iPhone app may follow | the owner's phone; iOS needs a Mac to build |
| Stack | NimBLE, **Nordic UART Service** (6E400001/2/3-B5A3-F393-E0A9-E50E24DCCA9E), advertised as `MorseLink-<ID>` | as the owner's first BLE test; terminal apps speak NUS |
| When Bluetooth runs | **off until the first `O`**; once a phone has paired, on at every start | most of the time no phone is around; saves power and RAM |
| Pairing | LE Secure Connections "Just Works" with bonding; accepted only within the **60 s `O` window**: OK when a phone pairs, error if none did | no display or keypad on the device; the window is the owner's consent |
| One phone | a phone paired in the window **replaces** any other; during the window **only a new phone** gets in (the paired one is turned away until it ends) | one connection: the old phone would otherwise take it straight back |
| Outside the window | a new phone's keys are deleted and it is disconnected; any connection not accepted within 30 s is dropped | strangers can't pair or hold the connection |
| A phone that lost its keys | pairs again only with `O` (NimBLE "repeat pairing") | the owner decides |
| Lines | plain ASCII lines (`phone.h`): `HI ST DR TX OK NO RX EV RL` to the phone, `HI RL` from it; unknown lines ignored | readable in a terminal app while testing; MTU-independent |
| Live compose | `DR <whole draft>` whenever it changes (checked every 250 ms) | simpler than per-character edits and self-correcting if a line is lost |
| Messages | `TX n to text` when one leaves, then `OK n` (receipt) or `NO n` (failed, back in the draft); `RX from text` | the phone can show sent / delivered / failed |
| Relay | **both ways, always** (owner): every packet this device originates for one device goes by LoRa *and* to `morselink/<ID>` through the phone, retries and keepalives included; packets from the phone are handled as if heard | a message gets through if either path works; the receiver drops the second copy (duplicate ring) |
| General SOS through the phone | to **every contact's** topic and the partner's (owner) | the people most likely to help |
| Not through the phone | DISCOVER, PRESENCE, PING (they measure the radio); other devices' packets (an SOS relay stays on the radio) | |
| A packet relayed in | counts as hearing the partner (keeps the link up) but leaves the radio readings alone; since v1.7 it proves the relay path only | |
| Phone comes and goes | silent on the device (logged) | auxiliary: walking away from it shouldn't buzz |
| Testing | `phone sim on`: a pretend phone in the console; the ESP32 emulator has no Bluetooth | |

---

## 0c. Changes in v1.5 (13 October, discovery)

The open points of v1.0 §7.2–7.3, settled while building `D`. Each is a
default that can be changed.

| Area | Decision | Why |
|---|---|---|
| Inviting from the list | **the list ends at the first invite** | the link engine keeps one request open at a time; a second invite would cancel the first |
| Invite | tap (`E`): the **ID is echoed** (a press goes back to the list), then the request goes out exactly as `S <ID>` | the "say who before acting" rule of `S`; it also catches a double tap keyed too slowly |
| Next | double tap (`I`); after the last entry the list ends with the cancel tone | |
| Stopping | 2 s hold stops the list; 3 s command mode; 5 s cancels everything; 30 s untouched ends it | as command mode |
| Anything else keyed | error + the same entry again | you hear where you are |
| Nobody answered | **error + `0`** | `0` is never part of an ID |
| Timing | 16 slots of **190 ms** (a PRESENCE's 145 ms + 45 ms slack), the first 20 ms after the DISCOVER; the searcher listens 0.5 s past the last slot: the list plays **~4 s** after the echo | v1.0's default, 3 s / 16 slots |
| Near / mid / far | path loss = their power − RSSI; **near below 80 dB, far from 105 dB** (at 17 dBm: above −63 / below −88 dBm) | placeholders: every sweep logs each device's path loss, to set them from bench numbers |
| Answer limits | the same searcher at most **3** times, all searchers at most **20** times, per **10 minutes**; silent mode (`Z`) never answers | nobody can track a device by sweeping over and over |
| Blocklist | a blocked device can't find us, and never shows up in our list | |
| Not built | a second sweep round for collision recovery; "one invite per source per hour" | collisions are rare with a few devices (listen-before-talk helps); requests are already capped at 3 waiting |

## 0b. Changes in v1.4 (8 October, the owner's additions)

| Area | Decision | Why |
|---|---|---|
| Power switch | a switch kept in firmware: **`F`** switches off (echo `OFF`, press aborts), **holding the key 2 s** switches on (one buzz: let go; OK when ready) | the owner wants the key to be the on/off button |
| Power arriving | the device starts **only if switched on**; switched off, USB charges it without starting it | it used to start whenever power arrived |
| What "off" is | ESP32 deep sleep watching the key; radio rail cut, motors parked low (§8 "Power switch") | only the ESP32 can watch the key |
| `F` during our own SOS | **refused** (error); cancel the SOS first | being off would silently stop the SOS resends |
| Incoming message in a conversation | **text alone**, no attention and no ID, when the last exchange with that device (a message either way, or connecting) was **at most 5 min** ago; otherwise attention → ID → text | in a running conversation the announcement only slows things down |
| Messages from someone else | always announced in full, and they become "the conversation" | a bare message must never be mistaken for the partner's |
| After a restart | nothing is remembered: the first message is announced | the pairing may be days old |
| Loopback (13 Oct, v0.3.5) | **never saved**: off after every restart or switch-off | left on, the pretend partner answers in the real partner's name, so a real partner going away was never noticed |

## 0a. Changes in v1.3 (the radio step)

Details the earlier versions left open, settled while implementing the radio.
Each is a default that can be changed.

| Area | Decision | Why |
|---|---|---|
| Transmit power | **17 dBm** default; 2–20 dBm with `radio power` | above 17 dBm the SX1276's high-power mode asks for a low duty cycle |
| Sync word | **0x5E** | neither nibble matches 0x12 (SX127x default), 0x34 (LoRaWAN) or 0x2B (Meshtastic) |
| Packet types | one spare code used for **PING** (bench and range test); `LINK_REPLY` gets a fifth status, **ENDED** | §6 |
| Accept handshake | every accept (by `S` or `P`) ends with **PAIR_CONFIRM**; the accepting side moves its link only when the confirm arrives | a lost accept never strands the old partner |
| A request answered BUSY | **stays open**: a later accept still connects, unless the requester has asked someone else since | otherwise the queue would be pointless |
| Requests waiting for A / N | at most **3**; a played one waits **15 min**, a queued one up to **60 min** | v1.0 had 24 h |
| Who can message you | **anyone not blocked**, not just the current partner | an SOS responder must reach the SOS sender, who may keep their own partner |
| A message without a receipt | the link counts as **lost**; messages queued behind it fail with it, in **one** error, and all their text goes back into the draft | one error to hear, nothing lost |
| Messages queued | up to **3** behind the one being delivered, in order | |
| A and N | their echo plays the **requester's ID** | you hear who you are answering |
| Link at restart | restored, unless it had **ended** | a partner who moved on isn't asked again |
| SOS relaying | **every** SOS-family packet (also targeted ones and `SOS_CANCEL`) is relayed once, hop limit **2** | a cancel must reach whoever the SOS reached |
| Acknowledging a blocked sender's SOS | **unblocks** them | "SOS ignores blocklists" must hold for the conversation that follows |

---

## 0. Changes since v1.0

| Area | v1.0 | v1.1 | Why |
|---|---|---|---|
| M1 pin | GPIO32 | **GPIO25** (confirmed) | GPIO32 is hard-wired to the SX1276 DIO2 on the T-Beam |
| Key pin | GPIO39 | **GPIO4** (confirmed); **GPIO14** since v1.8 (§0f) | owner's change; GPIO4 also drives the board LED (lit while pressed), hence the move |
| Test setup | — | **board's IO38 button + LED** until the motors and key are built | two switches in `board.h` (§2) |
| Linking | — | **accept every time; busy = 10 min; keepalive 30 s / lost at 90 s** | decided 3 and 6 Oct (§7) |
| Motors | LRA | **ERM coin (Pololu #1638)** | actual part; GPIO-high = on, LEDC for intensity |
| Button pull-up | — | on the ESP32's 3V3 (not DCDC1) | DCDC1 can be switched off between vibrations |
| Model | send to selected peer | **walkie-talkie link** | stays attached to the last selected recipient; notified if the connection is lost |
| Message ACKs | ACK + "delivered" pips | **nothing the user acknowledges**; a silent automatic receipt is planned so a failed send gives an error (§7) | devices work as pairs |
| Target announcement | before the first character | **after the send hold, before transmission** | keying from idle (incl. SOS) never waits for a wake tap; same safety purpose |
| SOS | menu item + confirm hold | **anywhere, no hold**; confirm by silence | 3 s after SOS, 2 s after a target |
| New commands | — | **E** (read back draft), **O** (60 s phone-pairing window) | |
| New behaviours | — | 100-char cap; draft discarded after 60 s untouched; low-battery warnings at 15 % / 5 %; press during echo aborts | |
| Calibration maths | see §11 | several fixes | found by simulation (§11.8) |

---

## 1. Overview

MorseLink is a wearable Morse-code communicator.
- A user keys a message with a single button; the device decodes it to text
  on the fly and transmits it.
- On the receiving device the text is re-rendered as Morse and delivered
  through two vibration motors along the forearm.

**Store-and-render, not live keying.** The complete message is decoded before
transmission. The receiver renders it at *their own* reading speed. One
packet per message.

**Walkie-talkie model.**
- A device stays attached to the last selected recipient (its *link*).
- If the connection is lost, the user is notified.
- Sending with no recipient gives an error, and the draft is kept.

**Transport priority:**
1. LoRa (primary).
2. BLE device-to-device (Phase 2).
3. Phone relay over MQTT (auxiliary).

The device is fully functional with no phone present.

---

## 2. Hardware

**Board:** LilyGO T-Beam **V1.1** (ESP32, SX1276 868 MHz, AXP192, NEO-6M unused).
**V1.2** (AXP2101 power chip) is also supported since v0.3.1: same pins, different rails (below).
The radio may be an **SX1262** instead of the SX1276 (supported since v0.3.2,
detected at boot): same SPI pins, CS 18, reset 23; interrupt on DIO1 = GPIO33,
BUSY on GPIO32; DIO2 drives the antenna switch, DIO3 powers the TCXO (1.8 V).
The sync word 0x5E is written as 0x54E4, the SX126x form of the same word, so
SX1262 and SX1276 boards hear each other.
Two boards available.

| Function | GPIO | Notes |
|---|---|---|
| Motor M1 (wrist) | **25** | NPN (2N2222) via 330 Ω (v1.8; 1 kΩ was too weak), flyback diode, motor from 3.3 V |
| Motor M2 (forearm, ~9 cm proximal) | **13** | same stage |
| Key | **14** (v1.8; GPIO4 before) | a push button to GND; the ESP32's internal pull-up (~45 kΩ), no resistor |

**Build switches [impl]:** `USE_ONBOARD_BUTTON`, `USE_ONBOARD_LED` and
`VERBOSE_LOG` in `board.h`. Since v1.8 the default is the wearable build
(the external key, the LED off, the verbose logs on).
- `USE_ONBOARD_BUTTON 1`: the key becomes the board's IO38 button.
- `USE_ONBOARD_LED 1`: the haptic output is mirrored on the board LED
  (GPIO4), driven open-drain and inverted: M1 (or both) shows bright and M2
  alone shows dim, so a dash reads bright-then-dim. It adds the LED's
  current to everything measured. Off, the LED's pin is an input with its
  pull-up (dark, no current, also in deep sleep).
- The motor pins are driven in every build.
- The build refuses a key on a strapping pin (0 / 2 / 12 / 15), on a pin
  that can't wake the chip, or on the LED's pin with the LED switch on.

Board-fixed pins:

| Function | GPIO |
|---|---|
| LoRa SCK / MISO / MOSI / CS / RST | 5 / 19 / 27 / 18 / 23 |
| LoRa DIO0 / DIO1 / DIO2 | 26 / 33 / **32** |
| I²C SDA / SCL (AXP192 @ 0x34) | 21 / 22 |
| AXP192 IRQ | 35 |
| GPS RX / TX (unused) | 34 / 12 |
| Onboard user button (the test setup's key) | 38 |
| Onboard LED (active-low, from 3.3 V) | 4 |
| Onboard LED (active-low) | 4 |

`board.h` refuses to compile with a motor or key on GPIO32/33, or a motor on
an input-only pin.

**AXP192 rails [impl]:**

| Rail | Feeds | Setting |
|---|---|---|
| DCDC1 | motor supply (OLED pads) | 3.3 V; switched off 1.5 s after the last vibration (rail gating, on by default) |
| DCDC3 | ESP32 | never touched |
| LDO2 | LoRa | 3.3 V, on |
| LDO3 | GPS | off (saves ~40 mA) |

Battery voltage, charge current and USB presence are read from the AXP192
ADCs. If an OLED is ever fitted, disable rail gating (`haptic gating 0`): an
unpowered OLED can hold the I²C bus down.

**AXP2101 rails (T-Beam V1.2) [impl]:**

| Rail | Feeds | Setting |
|---|---|---|
| DCDC1 | ESP32 **and** the header pads (motor supply) | never touched, so **no rail gating** on V1.2 |
| ALDO2 | LoRa | 3.3 V, on (off at power-up: the radio is unpowered until `pmic_init`) |
| ALDO3 | GPS | off |

Battery voltage and USB presence are read; the AXP2101 has no current meter.

**Motors:** Pololu #1638, a 10 × 2 mm shaftless ERM.
- Electrical: 2.5–3.5 V, 60 mA running, 120 mA start.
- ~30–50 ms spin-up and ~50–80 ms coast, so units much below ~120 ms smear.
- Driven through LEDC (20 kHz, 10-bit); at 100 % intensity the output is
  simply high.

**Timing [impl]:** element timing uses `esp_timer` with absolute deadlines (no
drift accumulation). Key edges are timestamped in the ISR (µs).

---

## 3. Software environment

- **ESP-IDF v6.1**, VS Code + Espressif extension.
- NimBLE for BLE (step 5); the existing NUS `main.c` is the starting point.
- FreeRTOS tick 1 kHz; console on UART0 at 115200.

Tasks:

| Task | Core | Priority | Role |
|---|---|---|---|
| keyer | 1 | 12 | debounce, hold thresholds |
| ui | 1 | 8 | the whole interaction state machine |
| inject | 1 | 9 | console key injection |
| console REPL | — | 2 | serial console |
| net | 0 | 10 | link protocol engine, SX1276 radio, loopback test network |
| esp_timer | 0 | 22 | haptic segments, rail gating |

---

## 4. Identity and addressing

- Device ID: 3 characters from the 31-symbol alphabet `23456789ABCDEFGHJKMNPQRSTUVWXYZ`
  (no 0/O/1/I/L). Assigned centrally and set over the serial console
  (`id K7Q`); stored in NVS and kept through factory reset.
- **Wire address, 24 bits [impl]:**
  - 3-character IDs map to `[0, 31³)`.
  - 4-character IDs map to `[31³, 31³ + 31⁴)`, so the two lengths can never collide.
  - `0xFFFFFF` = broadcast; `0xFFFFFE` = none.
- User IDs exist only in the phone app/backend.

---

## 5. Contacts (peer table) [impl]

- 9 slots (one Morse digit each). Slots never reorder. Table full gives
  error + `FULL`.
- Persisted record: `used`, `verified`, `addr` (24-bit), `shared_key[16]`
  (Phase 2).
- Runtime-only fields (`last_heard`, `last_rssi`) stay in RAM to avoid flash
  wear.
- Blocklist of 16 entries (oldest drops out). `X` on a blocked ID unblocks it.

---

## 6. Packet format [impl]

The same format on every path (`main/proto.h`). Header is 10 bytes in Phase 1
(18 with the Phase 2 auth tag), big-endian:

| Field | Size | Notes |
|---|---|---|
| Version + type | 1 B | 4 bits each; version 1 |
| Flags | 1 B | bits 0–1 hop count (SOS relays), bit 2 long preamble, bit 3 relayed via phone, bits 4–7 zero |
| Source / destination | 3 B + 3 B | 24-bit addresses (§4); destination `0xFFFFFF` = everyone |
| Message ID | 2 B | the same for every retry of one packet |
| Payload | var | text ≤ 100 chars, or protocol data |

Types (v1.0's `ACK` is removed; link types added, see §7):

| Code | Type | Payload | Answered by |
|---|---|---|---|
| 1 | `DISCOVER` | slots (1 B), slot length in 10 ms steps (1 B) | `PRESENCE` from every discoverable device, in a random slot |
| 2 | `PRESENCE` | sender's transmit power, dBm (1 B), sweep tag = low byte of the DISCOVER's ID (1 B) | |
| 3 | `LINK_REQ` | none | `LINK_REPLY` |
| 4 | `LINK_REPLY` | status (1 B), ref = ID of the request (2 B) | `PAIR_CONFIRM` if ACCEPT |
| 5 | `PAIR_CONFIRM` | ref = ID of the ACCEPT (2 B) | |
| 6 | `KEEPALIVE` | none | `LINK_REPLY` ENDED if not linked |
| 7 | `MSG` | the text, 1–100 characters | `MSG_RECEIPT` |
| 8 | `MSG_RECEIPT` | ref = ID of the message (2 B) | |
| 9, 10, 11 | `SOS`, `SOS_ACK`, `SOS_CANCEL` | none (Phase 2: a position) | |
| 12 | `UNPAIR` | none | |
| 13 | `PING` | kind, RSSI, SNR, dBm (4 B) | `PING` kind = pong (bench tool) |

Codes 0, 14 and 15 are unused (2 spare). `LINK_REPLY` statuses: RECEIVED,
BUSY, ACCEPT, DECLINE, and **ENDED** ("we are not linked (any more)"). Longer
payloads than listed are accepted and the extra bytes ignored, so a later
version can add fields.

**Message IDs** are reserved in blocks of 1000: flash holds the start of the
next block, and a restart jumps to it. A counter restarting at 0 would get
fresh packets dropped as duplicates by devices that heard the old ones. The
counter survives a factory reset, like the device ID.

**Duplicate suppression:** a ring of the last 48 (source, message ID) pairs,
each remembered for 10 minutes. A message can arrive by both LoRa and the
phone relay. A duplicate `MSG` is receipted again but not delivered twice.
Keepalives, pings and discovery packets aren't tracked: repeats are harmless.

**Radio [impl]:** 869.525 MHz, 125 kHz bandwidth, SF9, coding rate 4/5,
explicit header, CRC on, sync word 0x5E, PA_BOOST at 17 dBm (2–20).
- The 869.4–869.65 MHz sub-band allows 500 mW e.r.p. under ICASA at less
  than 10 % duty cycle.
- Airtime: a 10-byte packet takes 144 ms; a 100-character message about
  590 ms.
- **Duty cycle:** each device counts its airtime over a rolling hour (60
  one-minute buckets) and holds back anything that would exceed 10 %. SOS
  traffic is exempt. Keepalives alone use about 0.5 %.
- **Listen before talk:** a device holds its packet back while it can hear
  someone else's preamble or header (at most 1.5 s), since the two packets
  would destroy each other.
- **Preambles (Phase 2 hook):** packets that must reach a possibly sleeping
  receiver (requests, messages, keepalives, notices, SOS) are marked "long
  preamble"; immediate replies are not. Both lengths are 8 symbols in
  Phase 1. Low-power listening (CAD) only needs `RADIO_PREAMBLE_LONG` raised
  above the sniff interval, and the receive side switched to CAD.
- The discovery slot length is derived from airtime: a PRESENCE's 145 ms
  plus 45 ms, so 190 ms at SF9 (§7).

---

## 7. Linking and pairing [impl]

The owner's model is a pair of walkie-talkies:
- The device stays attached to the last selected recipient (its *link*)
  until the user picks someone else. There is no hang-up command.
- Every connect attempt needs the other person to accept, whether it's by
  contact slot (`S`) or by typed ID (`P`).

Constants are in `config.h`: `LINK_BUSY_MS`, `KEEPALIVE_MS`, `LINK_LOST_MS`.

### Connecting

1. **Request.** `S` or `P` sends `LINK_REQ`. The other device immediately
   answers `RECEIVED` (silent for its user), then:
   - **free:** its user is alerted (attention, requester's ID, decision
     pending) and answers with `A` (accept) or `N` (decline);
   - **busy:** it answers `BUSY` and queues the request (see below).

   No reply after quick retries (about 10 s) means **unavailable**.
2. **Busy** = a message sent or received on the current link within the
   last **10 minutes**.
   - A queued request plays once 10 minutes have passed since the last
     message, or the link drops; the user then accepts or declines.
   - Accepting moves the link, and the previous partner is told the link
     ended.
3. **Keepalives.** Linked devices exchange a `KEEPALIVE` every **30 s**
   (45 s at SF11, 60 s at SF12), each saying how long ago the sender last
   heard us on each path (v1.7, §0e).
   - A path is connected when both directions are proven; the link is lost
     when no path is: nothing heard for **90 s** (135 / 180 s at SF11 / 12),
     the partner reports it can't hear us, or a message got no receipt.
     Error + their ID (not again after a failed message's own error).
   - Both ways proven again: restored, OK + their ID.
   - Sending while lost gives error + ID, and the draft is kept.
4. **Feedback for the requester:**

   | Outcome | Pattern |
   |---|---|
   | accepted | OK + ID (new contact: OK + slot digit) |
   | BUSY | error + `BUSY` |
   | declined | error + `NO` |
   | unavailable | error + ID |

5. `N` on a saved contact's request only declines. Blocking applies only to
   strangers.
6. After pairing with a new device, it becomes the current link.

### Sending

**Silent delivery receipt [impl]:** each `MSG` is answered automatically by
a tiny receipt that neither user feels.
- The sender plays OK when the receipt arrives.
- No receipt after retries: error + ID, and the draft is restored.

Without it, a message sent in the window before "connection lost" fires
(up to ~2 min) would look sent but never arrive. The owner expects an error
in that case, which needs the receipt.

### How it works on the air [impl]

The protocol engine is `main/link.c` (pure C, host-tested); `main/net.c`
runs it with the radio.

**Retries.** A request, an accept and a message are each sent up to 5 times,
1.6–2.4 s apart, then the sender waits 2 s for the answer: about 10 s in all
("unavailable" / "not delivered"). Retries reuse the message ID, so the
receiver acts once and answers every copy.

**Requesting (S / P).**
- The request stays *open* after RECEIVED or BUSY: if the other user accepts
  later, the link is made then (OK + ID).
- It closes when answered (accepted / declined), when the requester asks
  someone else ("the newest wins"), or after 80 minutes.
- An ACCEPT for a request that is no longer open is answered ENDED, and the
  acceptor hears error + ID.
- The link only moves when the other side accepts.

**Answering.**
- A request is accepted straight away, without asking the user, if the
  requester is already our partner, or if we were asking for them too (both
  asked at once).
- Otherwise it waits for A / N (at most 3; a 4th is ignored, so the
  requester hears "unavailable"). A played request waits 15 min for an
  answer; one queued while busy waits up to 60 min to be played.
- A [n] / N [n]: position n among the requests played, oldest first; the
  echo plays the requester's ID.
- The accepting side repeats ACCEPT until PAIR_CONFIRM arrives, and only
  then moves its link (no confirm within ~10 s: error + ID, link unchanged).
- Both ends add each other as contacts (OK + slot digit for a new one).

**The link.**
- Anything the partner itself transmits, to anyone, proves they are in
  range. Each side therefore only sends a `KEEPALIVE` when it has sent
  nothing else for ~26–30 s (random, so two devices don't keep colliding).
- Nothing heard for 90 s: lost (keepalives continue); heard again: restored.
- When the link moves, the old partner gets `LINK_REPLY` ENDED (sent twice)
  and hears error + ID. Its sends are then refused (error + ID, draft kept)
  and its keepalives stop.
- Any device whose keepalive arrives although it isn't our partner is told
  ENDED again (UNPAIR if we forgot it), at most every 10 s. So both ends
  always come to agree, even after a missed notice or a restart.
- Exceptions:
  - a device we are in the middle of accepting;
  - a device that acknowledged our SOS while that SOS is still being resent:
    acknowledging made us its partner without a handshake (§10), and it
    must be able to reach us.

**Messages.**
- Messages are accepted from anyone not blocked (receipted, and played with
  the sender's ID), not only from the partner.
- Busy counts only messages to and from the partner.
- Up to 3 messages can wait behind the one being delivered, and go in order.
- When one gets no receipt, those behind it fail with it in a single error.
  All their text goes back into the draft, in front of anything keyed since,
  and the link counts as lost.
- The user heard the recipient announced, so a message is refused if the
  link has moved to someone else in the meantime.

**Unpairing (X).** `UNPAIR` is sent twice. The other device removes us from
its contacts, and drops the link if it was with us (error + ID).

**Restart.** The link is restored from flash and starts "up", with a hello
keepalive within 5 s. A link that had *ended* is not restored. Sending then
gives error + ID until the user picks someone.

### Discovery [impl] (v1.5)

Open discovery (`D`), as v1.0 §7.2–7.3, with the details in §0c. The
engine is `main/link.c` (on air) and `main/disc.c` (the found list and the
answer limits); constants are in `config.h` (`DISC_*`).

**On air.**
1. The searcher broadcasts `DISCOVER` carrying its window: 16 slots of
   190 ms.
2. Every discoverable device in range answers `PRESENCE` in a random slot,
   timed from the moment the DISCOVER ended (everyone heard it end at
   once), with no user involvement. Simultaneous LoRa packets destroy each
   other, hence the slots; listen-before-talk spaces out answers that come
   too close.
3. `PRESENCE` carries the answerer's transmit power and the sweep's tag
   (the low byte of the DISCOVER's message ID), so a late answer to an
   older sweep can't slip into a newer one.
4. The searcher listens through the last slot plus 0.5 s, then sorts by
   **path loss** (their power − the RSSI here) into near / mid / far,
   keeping arrival order inside each group (RSSI swings ±20 dB with
   orientation and bodies: no finer ranking).
5. An answer that can't leave by the end of its slot + 0.5 s (channel busy,
   duty cycle used up) is dropped; a DISCOVER that can't leave within 5 s
   ends the sweep with nobody found.

**Who answers.** Not in silent mode (`Z`); not a device that blocked the
searcher; at most 3 answers to the same searcher and 20 in all per 10
minutes. Discovery packets count against the duty cycle (a DISCOVER and
each PRESENCE take 144 ms). Answering is silent for the answerer's user.

**Walking the list.** The first entry plays about 4 s after the `D` echo.
Each entry is the device's ID, a word gap, and N / M / F.

| Key | Action |
|---|---|
| `E` (tap) | invite: the ID is echoed (a press goes back to the list), then the request goes out exactly as `S <ID>`; the list ends |
| `I` (double tap) | next; after the last, the list ends (cancel tone) |
| hold 2 s | stop the list (cancel tone) |
| hold 3 s / 5 s | command mode / cancel everything |
| anything else | error + the same entry again |
| nothing for 30 s | the list ends (cancel tone) |
| SOS | still SOS |

Nobody answered: error + `0`. Incoming alerts wait until the list is over;
an SOS alarm interrupts it.

---

## 8. Input [impl]

### Holds (feedback buzzes at each threshold while held)

| Released after | Feedback at threshold | Meaning |
|---|---|---|
| < boundary | — | dot |
| boundary … 1.5 s | — | dash |
| 1.5 – 2 s | — | nothing |
| 2 – 3 s | pip | send / execute |
| 3 – 5 s | pip pip | command mode (draft kept) |
| ≥ 5 s | long | cancel everything: draft, command, SOS entry, **active SOS** |

### Playback interaction

- **Any press stops any playback**, and that press is also keying. SOS can
  therefore be keyed at any moment.
- A *lone* one-element character right after a stop-press, followed by a
  word gap or a hold, is dropped: that tap only meant "stop".
- Key timing is frozen while the key is down: no letter or word gap can time
  out mid-press.

### Composition

| Event | Rule |
|---|---|
| letter gap | learned threshold (§11) closes a character |
| word gap | inserts a space |
| error prosign (7+ dots) | deletes the last letter and any word space a pause added after it; no space is inserted for the pause that follows |
| undecodable pattern | `?` |
| 100th character | further characters give an error tone |
| draft untouched 60 s | discarded with the cancel tone |
| incoming alerts | queue; they play after the key has rested 3 s (10 s while a draft exists) |

### Sending

1. Hold 2 s.
2. The device plays the **recipient's ID** (safety announcement).
3. Message transmitted, then OK.

A press during the ID cancels the send and keeps the draft.

### Command mode

Hold 3 s, then key a letter (+ argument), then hold 2 s. The argument is
echoed (for `X` with a slot digit, the slot's ID), and a press during the
echo aborts. Leaving command mode untouched for 30 s exits it with the cancel
tone.

| Key | Command | Argument | State |
|---|---|---|---|
| `P` | pair / connect to a new device | ID | [impl] |
| `D` | discover nearby | — | [impl] (v1.5); the list is walked with tap / double tap (§7) |
| `A` / `N` | accept / decline a pending request | [position] | [impl]; the echo plays the requester's ID |
| `S` | select recipient | slot or ID | [impl] (asks the other device) |
| `R` | replay received message | [1–9] | [impl] |
| `L` | list contacts | — | [impl] |
| `V` | verify (key code) | slot or ID | Phase 2 |
| `X` | forget contact (sends UNPAIR) / unblock | slot or ID | [impl] |
| `B` | battery % + recipient status | — | [impl] |
| `C` | recalibrate | — | [impl]; cancel keeps the old profile |
| `Q` | reading speed | 1–9 | [impl] |
| `Z` | toggle silent (undiscoverable) | — | [impl]; plays ON/OFF; alerts still play |
| `E` | read back the draft | — | [impl] |
| `F` | switch off (see below) | — | [impl]; the echo plays `OFF`; refused while our SOS is active |
| `O` | open a 60 s window to bond the owner's phone | — | [impl] (v1.6); nothing plays until the window ends: OK (a phone paired) or error (none came) |
| `G` | range level: 1–4 = SF9–SF12 | [1–4] | [impl] (v1.7); alone: plays the level; OK when set; both devices by hand |

SOS is not a menu item: it is recognised everywhere (§10).

### Power switch [impl] (v1.4)

| Situation | What happens |
|---|---|
| `F`, echo `OFF` untouched | the switch is set to off (in flash); motors parked low and held; radio rail off (V1.1: LDO2 and DCDC1; V1.2: ALDO2); ESP32 deep sleep with the key as its only wake-up |
| key held 2 s while off | one 150 ms buzz ("let go"); after the release the device starts normally and plays OK |
| key tapped while off | wakes for a moment, sees the key released before 2 s, sleeps again |
| power arrives (USB, battery, reset button, crash) | starts only if the switch is on; switched off, it goes straight back to sleep (charging continues on USB) |
| board's PWR button | unchanged hardware power cut; it doesn't move the switch |

The first-use calibration has no command mode, so `F` is available once
calibrated (the console's `off` works any time). Switched off, the device
still draws a little (deep sleep); for storage use PWR or remove the battery.

---

## 9. Haptic output [impl]

Channel split, unchanged from v1.0:
- **dot** = M1 alone.
- **dash** = M1 then M2.
- **system tones** = both together.

Anything that is Morse plays on the split.

Timing at U = 200 ms (`Q5`):

| Element | Duration |
|---|---|
| dot | 200 |
| dash | M1 190, both 20, M2 390 |
| intra gap | 200 |
| letter gap | 600 |
| word gap | 1400 |

Tunables:
- M1's share of a dash: `haptic split`, e.g. 75 = 0.75 U / 2.25 U.
- Overlap: `haptic overlap`.
- Farnsworth stretch: `haptic farnsworth`.
- Per-motor intensity: `haptic m1/m2`.

Reading speeds Q1…Q9 = 320, 280, 250, 220, **200**, 180, 160, 140, 120 ms.

System tones (pip = 80 ms, long = 300 ms):

| Pattern | Meaning |
|---|---|
| pip | 2 s threshold |
| pip pip | 3 s threshold |
| long | 5 s threshold / cancelled |
| pip–long | OK / sent |
| pip pip pip (fast) | error |
| long–pip | keying degraded |
| long + 3 pips | attention |
| slow pip pip pip | decision pending |
| two spaced pips | SOS delivered |
| 3 × 500 ms | **SOS armed** |
| 5 × (1 s on / 0.3 s off) | **SOS alarm** |
| quiet pip | unverified peer (Phase 2) |

Composite events:

| Event | Sequence |
|---|---|
| incoming message | attention → sender ID → text, once (no repeats; `R` replays with the ID) |
| incoming message, active conversation (last exchange with that device ≤ 5 min ago, connecting included; v1.4) | text only |
| send | recipient ID → OK |
| unavailable / connection lost | error → ID |
| restored | OK → ID |
| BUSY | error → `BUSY` |
| declined | error → `NO` |
| table full | error → `FULL` |
| pair succeeded | OK → slot digit |
| low battery | attention → % |
| SOS delivered | two spaced pips → responder ID |
| SOS cancelled by sender | long → ID |
| discovery list entry (v1.5) | ID → N / M / F |
| discovery: nobody answered | error → `0` |

---

## 10. SOS [impl, over the air]

### Recognised anywhere

Idle, composing, command mode, during playback, during first-use calibration.
Either form works, even with a wrecked or missing calibration (§11.7):
- the run-together prosign `...---...`;
- S O S as letters, as a whole word.

### Sending side

1. **Confirm by silence:** 3 s after SOS, or 2 s after a target argument
   (slot or ID). An unreadable argument falls back to a **general SOS**.
2. **Arm:** target ID echo (if targeted), then three long buzzes. A press
   aborts.
3. **Send:** general means all devices in range (relayed, see below) plus the
   phone relay; targeted means one device. OK confirms.
4. **Resend:** every 30 s for 10 min, then every 2 min, until a 5 s hold
   cancels. Cancelling sends `SOS_CANCEL`, which stops responders' alarms.
5. Each `SOS_ACK` plays delivered + the responder's ID. If you had no
   recipient, the first responder becomes it.

### Receiving side

1. The alarm interrupts everything and repeats every 30 s until a tap
   acknowledges it.
2. The tap sends `SOS_ACK`, plays the sender's ID, and makes them your
   recipient. A blocked sender is unblocked, so the conversation that
   follows works too (v1.3).
3. A second SOS arriving during an alarm (or while you're sending your own)
   alarms as soon as that one is dealt with.
4. Resends from an SOS you already acknowledged are re-acknowledged silently.

### Rules

- SOS ignores silent mode, blocklists and duty-cycle limits.
- Every SOS-family packet (`SOS`, `SOS_ACK`, `SOS_CANCEL`, general or
  targeted) is relayed once per device.
  - Relay pause: random 0.2–1.5 s.
  - Hop limit: 2.
  - Duplicates are dropped by message ID.
  - Only the addressee (or everyone, for a general SOS) alarms; relays stay
    silent.
- Each SOS (and each resend) goes out twice; a cancel three times.
- A resend is a new packet, so it is relayed again and reaches devices that
  have come into range since.

---

## 11. Adaptive keying calibration [impl]

As v1.0 §10, with the changes below. All were found or verified with the
keying simulator in `test/host`.

### 11.1 Model

Two independent clusters (dots, dashes) and two gap clusters (inside a
character, between characters), each tracked as mean + MAD. The ratio is a
diagnostic.

### 11.2 Bootstrap: PARIS

The 14 elements are labelled by position.
- MADs come from the **measured spread**, floored at 8 % of the mean (10 %
  for gaps). v1.0's flat 20 % started any keyer with a ratio of 2.2 or less
  as "marginal".
- PARIS is accepted only if:
  - the new profile classifies its own PARIS correctly;
  - ratio ≥ 1.8 and d′ ≥ 1.5;
  - letter gaps clearly exceed intra gaps.

  Otherwise: error and re-prompt.

### 11.3 Decision boundaries

| Boundary | Rule |
|---|---|
| dot/dash | spread-weighted once 20 samples of each exist, geometric mean before |
| letter gap | geometric mean of the gap clusters, kept above `intra + 3·MAD`, below `0.75 · letter`, and clamped to **1.5–4 × dot_mean** |
| word gap | `min(letter + 4.5·MAD, 2 × letter)`, at least 1.6 × the letter threshold |

The letter-gap clamp anchors it to element timing: dictionary validation
can't catch mis-split characters (B → T S), so a runaway would otherwise go
unnoticed.

The word-gap rule changed because v1.0's 2 × letter missed ~8 % of word
gaps for consistent keyers. Keyers with variable gaps still get 2 ×.

### 11.4 What is learned from

- **Elements: z-score test against the *other* cluster.** A sample is learned
  when it is more than 2 sd from the other cluster's mean (3 sd for 10
  elements after an error prosign), plus a 5 % band at the boundary.
  - v1.0's fixed ±15 % band around a spread-weighted boundary cut off the
    upper tail of the dots. That biased dot_mean low and stopped it tracking
    a user who slows down: 30 % errors in simulation, now 0 %.
- Intra gaps: learned when below 0.85 × the threshold.
- Letter gaps: learned when clearly above the intra cluster (z > 2.5) and
  below the word threshold.
- Dictionary validation is unchanged: only valid characters are learned from.

### 11.5 Update rule

- Mean step capped at ±15 % × α.
- **The MAD update clips the deviation at 3 × MAD.** One 1.4 s hesitation
  right after PARIS can no longer crash d′.
- MAD floors: 4 % (elements), 6 % (gaps).
- Phantom updates as v1.0 (quarter weight, ratio-anchored). The ratio is
  frozen while either class has been unseen for more than 15 elements.

### 11.6 Health

d′ is computed as in v1.0.

| d′ | Behaviour |
|---|---|
| ≥ 1.6 | healthy (stays healthy down to 1.5) |
| 1.0–1.5 | marginal: boundary frozen at the last good value |
| < 1.0 | collapsed: "keying degraded" (long–pip) once, boundary held at the last good value |

While degraded, the means **keep learning in the background**, so the profile
recovers without `C` (verified in simulation).

Other rules:
- α: 0.15 → 0.04 over 50 confident elements. After 20 min idle, 0.15 for 10
  elements, then decays over 20.
- Error prosign: undo the deleted character's learning exactly (per-character
  snapshots) and widen the band.
- The profile is saved to NVS only while the key rests (≥ 20 s idle, ≥ 5 min
  apart): flash writes stall both cores.

### 11.7 SOS detection (independent of calibration)

1. One 9-element group shaped like SOS: immediately.
2. The last 9 elements shaped like SOS and **bounded by word-length silence
   on both sides**, however they were grouped. This covers S O S as letters
   and a slow, panicked prosign broken up by long gaps. It is checked when
   the closing silence arrives, so one misread dash in "SORRY" can't trigger
   an emergency mid-word.

The shape test, on durations only:
- the three longest elements are the middle three;
- the step after the third-longest beats any step that would make 4–6
  elements long (this rejects "DOG", "ROD", "GOD");
- the long three average at least 1.5 × the median short element (this
  rejects all-dot words like "HIS").

### 11.8 Simulation results (test/host)

| Keyer (600-char corpus) | Errors (all / letters only) |
|---|---|
| 12 WPM, 10 % jitter | 0.0 % / 0.0 % |
| ratio 2.2 | 0.0 % / 0.0 % |
| speeding up 140 → 85 ms | 0.0 % / 0.0 % |
| slowing down 90 → 150 ms | 0.0 % / 0.0 % |
| beginner with thinking pauses | 7.6 % / 2.0 % |
| 22 % jitter | 12.7 % / 7.5 % |

SOS:
- 0 false triggers in 400 random keyers (~240k characters).
- 98 % detection under a deliberately harsh panic model.

---

## 12. Phone app (auxiliary)

As v1.0 §11, with the decisions of §0d:
- BLE NUS, bonding + LE Secure Connections; the owner's phone is accepted
  during a 60 s `O` window. **[impl]** (device side, v0.4.0)
- Live compose and messages: the line protocol below. **[impl]** (device side)
- MQTT relay (`morselink/<device-id>`, opaque packets, base64 in `RL`
  lines). **[impl]** on the device; the app does the MQTT part.
- Flutter + `flutter_blue_plus`, Android first. (next)

### The BLE link as built [impl]

The phone writes to RX (…0002, write / write without response, encrypted
only); the device notifies on TX (…0003). Both carry lines of ASCII ending
in `\n`, fields separated by one space, a text field always last;
notifications are cut at MTU − 3 bytes and the phone joins them at `\n`.
Nothing is sent to a phone, and nothing it writes is used, until it is
accepted (bonded with the device's consent).

| Device → phone | Meaning |
|---|---|
| `HI <own ID> <protocol> <firmware>` | hello: first line after the phone is accepted, again after its `HI` (protocol 2 since v0.6.0, 3 since v0.6.1) |
| `ST <partner\|-> <none\|connected\|lost\|ended> <battery %\|?> <silent 0/1> <lora> <relay>` | status: after `HI` and whenever any part changes; the last two (protocol 2): each path's state |
| `RD <sf> <bandwidth kHz> <dBm>` | (protocol 2) the radio setting: after `HI` and whenever it changes |
| `CM <command>` / `CE <how> [<command>]` | (protocol 2) command mode: what is keyed so far; how it ended (done / cancelled / timeout / sos) or a command was given up (aborted / error: still in command mode) |
| `DR <draft>` | the whole draft whenever it changes (`DR` alone: empty) |
| `TX <n> <to> <text>` | message n left for `<to>` (n counts from 1 since start-up) |
| `OK <n>` / `NO <n>` | its receipt came / it wasn't delivered (its text is back in the draft, unless its recipient was just forgotten with `X`) |
| `RX <from> <text>` | a message arrived |
| `EV <what> <ID\|ALL\|-> [<detail>]` | `LINKED BUSY DECLINED UNAVAILABLE FAILED REQUEST LOST RESTORED ENDED UNPAIRED SOS SOS_ACK SOS_CANCEL SOS_SENT SOS_STOPPED`, and (protocol 2) `LORA RELAY` with the path's new state; `LOST` has its reason (silence / no-receipt / one-way); (protocol 3) `BLE_OFF <own ID>`: `W` switches Bluetooth off now |
| `RL <ID> <base64 packet>` | relay: publish this packet to `morselink/<ID>` |
| `T,<n>,<ms>,<EVENT>,...` | test mode's log lines (TEST_PLAN §4.4); the point test (protocol 3) adds `STEP` |
| `BN <what> [<detail>]` | (protocol 3) the meter session: `refused <why>`, `wait-usb`, `start <total s> <max dBm>`, `step <id> <s>`, `end <how>` (TEST_PLAN §17.7) |

| Phone → device | Meaning |
|---|---|
| `HI [anything]` | send `HI`, `ST`, `DR` again |
| `RL <base64 packet>` | a packet that arrived on `morselink/<own ID>`: handled as if the radio had heard it |
| `TEST START <key=value ...>` / `TEST STOP` | start / stop a test (TEST_PLAN §11; a point test's `plan=9:17,9:22,12:22`, protocol 3) |
| `BENCH POWER` / `BENCH STOP` | (protocol 3) start / stop the meter session |

Relay rules (`phone_relay_targets()`): only packets this device originates;
never DISCOVER / PRESENCE / PING; a packet for one device goes to that
device's topic; a general SOS or SOS_CANCEL to the partner's and every
contact's (each once); other broadcasts nowhere. Every copy the engine
transmits is relayed, retries included; receivers drop duplicates.

Composing on the phone stays Phase 2 (possibly earlier, for testing).

Note: on a free public broker anyone can read or inject packets, and
3-character IDs are easy to enumerate. Fine for a prototype.

---

## 13. Deferred / Phase 2

| Item | Notes |
|---|---|
| Crypto (X25519 + AES-CCM), `V` verification codes | packet fields reserved |
| BLE device-to-device | |
| Compose on the phone | |
| Draft slot | the current draft already survives an unavailable recipient |
| Low-power listening (CAD) | hook in place (§6): packets that must wake a receiver are marked long-preamble; raise `RADIO_PREAMBLE_LONG` and switch receiving to CAD |
| Sender WPM hint | |
| Haptic word marker | |
| Using the GPS | e.g. a position in SOS packets |
| Discovery extras | a second sweep round for collision recovery (devices already heard stay silent); "one invite per source per hour" |

---

## 14. Build order and plan to 26 Oct

| Step | Target |
|---|---|
| 0–4 bring-up, haptics, key, calibration, commands | **done 2 Oct** |
| Bench session with the test setup (board button + LED); motors when they arrive | from 6 Oct |
| 6–8 LoRa transport, linking/pairing, SOS over the air | **done 7 Oct** (v0.3); confirmed on the boards (V1.2 + SX1262) with v0.3.3, 8 Oct |
| Owner's additions: power switch, quiet replies in a conversation | **done 8 Oct** (v0.3.4) |
| 7 discovery (`D`) | **done 13 Oct** (v0.3.5) |
| 5 BLE link to phone (NUS port, live compose, O window, relay transport) | **done 13 Oct** (v0.4.0, device side) |
| 9 Phone app (Android): display, then MQTT relay | 14–21 Oct |
| Integration, measurements (range, battery, reading speed), report | 21–26 Oct |

---

## 15. Open questions

1. ~~Silent delivery receipt (§7)~~: built (v1.3).
2. On hardware: the dash-sweep perceptual test; tune split and overlap.
3. On hardware: the fastest readable speed.
4. On hardware: range at 17 dBm and at 20 dBm (`radio ping`), for the report.
5. On hardware: the near / mid / far thresholds for discovery (§0c), from the
   path losses each `D` logs at known distances.
6. On hardware: the phone link with the owner's Android phone (pairing in
   the `O` window, reconnecting after a restart, a second phone refused),
   and the RAM left with Bluetooth running (the boot log shows the free
   heap before it starts).
