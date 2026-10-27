# MorseLink manual

For firmware v0.6.4 and app v0.2.4. The app shows this same manual (the ? at
the top of its main screen).

The key is a push button on the board's GPIO14 (to GND; nothing else is
needed). In the test setup (a build option) the key is the board's middle
button instead, and everything you would feel on the motors also shows on
the board's LED.

## 1. The key

Short press = dot, long press = dash. Letters and spaces are decoded as you
key, from the timing the device learned in its calibration (section 12).

| Hold, then release after | You feel at that point | Meaning |
|---|---|---|
| 1.5 to 2 s | nothing | nothing (deliberately) |
| 2 s | pip | **send** the draft; in command mode, **execute** |
| 3 s | pip pip | **command mode** (your draft is kept); in command mode: **leave it** (OK; the draft is kept) |
| 5 s | long | **cancel everything**: draft, command, SOS entry, and an active SOS |

- **8 dots** (7 or more): error, deletes the last letter.
- **Any press during a playback** stops it, and is keying too, so an SOS can
  always be keyed. A lone tap that only stopped a playback is dropped.
- The draft holds up to 100 characters. Left untouched for 60 s it is
  discarded (the cancel buzz), unless `K` keeps it (section 4).
- Alerts that arrive while you key wait until the key has rested 3 s (10 s
  while there is a draft). An SOS alarm never waits.

## 2. Sending and receiving

**Sending:** key the message, then hold 2 s.

1. The device plays the **recipient's ID**: a press now cancels the send and
   keeps the draft.
2. The message leaves.
3. **OK** (pip–long) plays when its silent receipt comes back, usually
   within a second.

No receipt: **error + ID**, and the text goes back into your draft (in front
of anything you keyed since). Hold 2 s to try again. Sending while the link
is lost gives error + ID at once, and the draft is kept.

**Receiving:** attention + the sender's ID + the text. In a running
conversation (your last exchange with that same device at most 5 minutes
ago) the text plays alone. `R` replays the last message with its sender's
ID (`R2` the one before, up to `R9`).

## 3. Connecting to someone

The device works like a walkie-talkie: it stays linked to one partner until
you pick someone else, and the other person always has to accept.

Command mode, `S` + a contact's slot digit (or an ID), or `P` + a new ID,
then hold 2 s. Nothing plays until they answer:

| You feel | Means |
|---|---|
| OK + their ID | connected (a new contact: OK + its slot digit) |
| error + `BUSY` | they are in a conversation: your request waits in their queue |
| error + `NO` | declined |
| error + their ID | no answer (unavailable) |

**Being asked:** attention + their ID + "decision pending" (slow pip pip
pip). Command mode, `A` accepts, `N` declines (`A2` / `N2` for the second
request waiting). `N` on someone who isn't a contact also blocks them; `X`
+ their ID unblocks.

Up to 9 contacts. `L` plays them (slot digit + ID each); `X` + a slot or ID
forgets one (and tells them).

## 4. Command mode

Hold 3 s (pip pip), key the letter and its argument, hold 2 s. The device
echoes the argument first (for `X` with a slot: that contact's ID; for `F`:
OFF): a press during the echo aborts. **To leave command mode without a
command, hold 3 s again** (OK; your draft is kept). Nothing keyed for 30 s
also leaves it (the cancel buzz). The app shows what you key under
**COMMAND**.

| Command | Does |
|---|---|
| `S` slot or ID | connect to a contact or an ID (asks them) |
| `P` ID | connect to a new ID; OK + its slot digit when accepted |
| `A` [n] / `N` [n] | accept / decline a request (n: which one, oldest first) |
| `D` | discovery: who is around? (section 8) |
| `L` | list the contacts: slot digit + ID |
| `X` slot or ID | forget a contact (they are told), or unblock an ID |
| `R` [1-9] | replay the last (or an older) received message |
| `E` | read back the draft |
| `B` | battery %, then the link: OK or error + the partner's ID |
| `Q` 1-9 | reading speed (section 11) |
| `H` [1-2] | **how dashes feel** (section 11): 1 = the sweep from wrist to forearm (the default), 2 = forearm motor only (dots stay on the wrist); then a sample A in the new style. `H` alone plays which |
| `I` [1-9] | **vibration strength** (section 11): 1 weakest to 9 strongest, 8 as the board starts; `I` alone plays the level in use. OK and A (a dot and a dash) play at the new strength |
| `K` | **keep the draft**: OK + ON, and it is no longer discarded after 60 s untouched, until you send it or cancel it (5 s). `K` again: OK + OFF, the 60 s back. With no draft: error |
| `G` [1-4] | **range level**: 1 = SF9 (the default) to 4 = SF12; `G` alone plays the level in use. **Set the same level on both devices** (section 5) |
| `Z` | silent mode on / off (undiscoverable): plays ON or OFF |
| `C` | recalibrate (PARIS again; a 5 s hold keeps the old profile) |
| `O` | pair your phone: a 60 s window (section 9) |
| `W` | Bluetooth off / on: plays OK + OFF or ON. Off stays off, restarts included, until `W` again (or `O`). The phone is told first (section 9) |
| `F` | switch off (the echo plays OFF); hold the key 2 s to switch on |
| `V` | verification codes: not available yet (error) |

An unknown command or a wrong argument plays error, and you stay in command
mode.

## 5. Range level (G)

> **Both devices must be on the same range level.** On different levels
> they can't hear each other over the radio at all, SOS included. Set each
> device by hand (`G` + the level), then check with `G` alone. The app
> warns before a test if the two boards differ.

A higher level reaches further but every packet takes longer on the air, so
messages are slower and fewer fit in the radio's 10 % airtime limit. The
link's checks slow down with it:

| Level | SF | 25 characters on the air | 100 characters | Keepalive every | Lost after |
|---|---|---|---|---|---|
| `G1` | SF9 (default) | 0.25 s | 0.6 s | 30 s | 90 s |
| `G2` | SF10 | 0.5 s | 1.1 s | 30 s | 90 s |
| `G3` | SF11 | 1.0 s | 2.4 s | 45 s | 135 s |
| `G4` | SF12 | 1.8 s | 4.3 s | 60 s | 180 s |

Each level adds about 2.5 dB to the link budget (level 4 against level 1:
about 8 dB); the field tests measure what that gives in metres. The level
is stored and survives switching off. Transmit power is set on the console
(`radio power`, section 14); it is 22 dBm unless changed.

## 6. The link: LoRa and the relay

A message can travel two ways: **LoRa** (the boards' own radio) and the
**relay** (through both owners' phones and the internet, when both phones
are connected to their boards and online). Every message goes both ways at
once; the partner keeps whichever copy arrives first. So it gets through if
either path works.

- **Lost:** error + the partner's ID, when neither path works any more:
  nothing heard on either for the lost time (section 5's table), or the
  partner can't hear us (one-way), or a message got no receipt (then the
  message's own error + ID says it, and nothing more plays).
- **Restored:** OK + the partner's ID.
- **LoRa alone dropping while the relay still works plays nothing:** your
  messages still arrive. The app shows it.
- If the partner links with someone else: error + their ID.

The app shows each path:

| The app says | Means |
|---|---|
| LoRa connected | heard both ways recently |
| checking... | just started (a new link, or the phone just connected); settles within the lost time |
| one-way: K7Q can't hear you | we hear them, but their reports say our packets don't reach them |
| one-way: you can't hear K7Q | they hear us (their reports come by the other path), but we don't hear them |
| lost | neither way |
| Relay off | no phone connected to this board (or a test is running: tests send their own copies) |

## 7. SOS

**Sending:** key `...---...` (or S O S as letters) at any time: idle,
composing, in command mode, during a playback, even before calibrating.

1. To target one person, follow it with a slot digit or an ID.
2. After 3 s of silence (2 s after a target): the target's ID, then three
   long buzzes (SOS armed). A press now aborts.
3. It goes out (general: every device in range, and through your phone to
   every contact; targeted: that one device), and repeats every 30 s for
   10 minutes, then every 2 minutes, until **a 5 s hold cancels it** (the
   others' alarms stop too).
4. Each acknowledgement plays two spaced pips + the responder's ID. If you
   had no recipient, the first responder becomes it.

Every device that hears an SOS passes it on once, so it reaches a little
beyond direct range. SOS ignores silent mode, blocking and the airtime
limit.

**Receiving:** the alarm (5 long buzzes) interrupts everything and repeats
every 30 s until you **tap**: that acknowledges it, plays the sender's ID,
and makes them your recipient (a blocked sender is unblocked).

## 8. Discovery (D)

Command mode, `D`, hold 2 s. About 4 s later the devices that answered play
one at a time, nearest first: each one's ID, then N, M or F (near, mid,
far). Error + 0: nobody answered.

| While a device is offered | What happens |
|---|---|
| tap once | invite it: its ID plays again (a press goes back to the list), then it gets a request, as with `S` |
| double tap | the next device; after the last, the cancel buzz |
| hold 2 s | stop the list (3 s: command mode; 5 s: cancel everything) |
| anything else | error, then the same device again |

Nothing keyed for 30 s ends the list. Your device answers other people's
`D` by itself (you feel nothing) unless it is in silent mode (`Z`), and it
answers the same person at most 3 times in 10 minutes.

## 9. Your phone and the app

One phone per device; the device works the same without it.

- **Pairing:** command mode, `O`, hold 2 s. For 60 s a new phone can pair:
  in the app, Find my MorseLink, tap it, tap Pair when Android asks. OK
  plays when it has; error if no phone came. Your paired phone never needs
  `O` again: it reconnects by itself whenever it is near.
- **A new phone** replaces the old one (`O` again with the new phone).
- If the app keeps losing the board right after connecting, the pairing is
  stale on one side: forget MorseLink-*ID* in the phone's Bluetooth
  settings, `O` on the board, and pair again. The app says so when it sees
  it.
- **Bluetooth off:** command mode, `W`, hold 2 s: OK + OFF. The board tells
  the phone first, and the app says "Bluetooth switched off on the board
  (W)" and tries only every 30 s. LoRa carries the messages alone (no
  relay). `W` again: OK + ON, and the phone reconnects by itself. With no
  phone paired, `W` can't switch it on (error): `O` pairs one.

**What the app shows:**

- **Status:** the board's Bluetooth, the internet, the partner and the link,
  the battery, silent mode, each path (section 6) and the radio setting.
- **Keying:** your draft letter by letter, with each letter's dots and
  dashes above it, the letter you are keying as you key it (each press
  shows as a dot or a dash the moment you let go), what the last letter
  became ("·− → A", or "?" when it isn't a letter), and where your last
  press landed against the dot / dash boundary. Under it, your keying speed
  and the reading speed. In command mode, **COMMAND** and what you have
  keyed, with what the letter does; afterwards, briefly, how it ended
  (done, not understood, aborted, 30 s untouched).
- **Now playing:** while something plays on your arm (a message, an ID,
  PARIS), its letters with their dots and dashes, the letter buzzing now
  highlighted.
- **Learn** (the school icon): all of the above, large, for learning Morse:
  the PARIS calibration while the board asks for it (each press green or
  red against PARIS, and why an attempt wasn't accepted), the keying, what
  is playing, **your timing** (your average dot and dash, how many times
  longer your dashes are, your pauses against the board's limits, words
  per minute, how steady), the reading speed, the vibration strength, and
  a Morse chart. To practise reading by feel, choose **Morse only** (the
  letters appear once it has played) or **Hidden** (tap Show afterwards);
  messages that arrive meanwhile stay hidden in the chat until tapped.
- **Chat:** messages sent (waiting, then delivered or not delivered) and
  received, and events (requests, lost and restored, each path changing,
  SOS).
- **Tests** (the flask): the field tests (the test plan). Among them:
  - **Point test:** both boards still at one spot. Type a label for the
    spot (e.g. "pillar A, behind, 12 m"), choose **Single** (one range
    level and power) or **Sweep** (a list of them; the default is SF9 /
    17 dBm, SF9 / 22 dBm, SF12 / 22 dBm, about 25 minutes), Start. Both
    boards switch to each step's setting by themselves and back to their
    own at the end; you set nothing by hand. The page counts down each step
    and shows how each board hears the other. **Repeat at the next spot**
    starts the same plan with a new label.
  - **Meter session:** the board's current, step by step, on a multimeter in
    series with the battery, **with USB unplugged**. Tests > Meter session >
    Start; the board then goes through its states by itself (about 9
    minutes), buzzing each step's number on the wrist motor, and the page
    says which step it is and what to do (key PARIS in 1b, hold the key in
    1c, read the off current at the end).
  - After Start the app checks that the base phone answered: if it hasn't
    within 20 s, a red notice says its board is **not** running the test
    (check its internet).
- **Settings:** the internet relay, the logs, the board, and **Keep running
  in the background**: a small permanent notification keeps Bluetooth and
  the relay going with the screen off. On Android 13 and later the
  notification shows only if notifications are allowed for MorseLink; it
  works either way.

For long sessions also set Settings > Apps > MorseLink > Battery >
**Unrestricted** on the phone (Samsung: keep MorseLink out of "Sleeping
apps").

## 10. Switching on and off

| You do | What happens |
|---|---|
| command mode, `F`, hold 2 s | the echo plays OFF (a press aborts), then the device goes dark: radio off, nothing plays |
| hold the key 2 s while it is off | one short buzz: let go. It starts and plays OK |
| a short press while it is off | nothing: it goes back to sleep |
| USB in, a battery fitted, or RST | it starts only if it was switched on |

`F` is refused while your own SOS is active. Switched off, it still draws a
little: for weeks of storage use the PWR button or take the battery out.

## 11. What you feel

Morse plays on the two motors: a dot on M1 (the wrist), a dash from M1 to
M2 (the forearm): M1 for the first 1.4 units, M2 for the last 1.9, both
together for 0.3 units in the middle, so it feels like a sweep up the arm
(or, with `H2`, a dash on M2 alone: then which motor buzzes tells dot from
dash, as well as how long). The timing is the same either way. System
signals use both:

| Pattern | Means |
|---|---|
| pip | the 2 s point of a hold |
| pip pip | the 3 s point |
| long | the 5 s point; cancelled |
| pip–long | OK, sent, delivered |
| pip pip pip (fast) | error |
| long + 3 pips | attention (a message, a request) |
| slow pip pip pip | decision pending (A or N) |
| two spaced pips | your SOS was acknowledged |
| 3 × 0.5 s | SOS armed (a press aborts) |
| 5 × 1 s | **SOS alarm** (tap to acknowledge) |
| long–pip | keying degraded: recalibrate (`C`) |

**Reading speed** (`Q`, unit = one dot):

| `Q` | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|
| unit, ms | 320 | 280 | 250 | 220 | 200 | 180 | 160 | 140 | 120 |

**Vibration strength** (`I`): how hard the motors are driven. `I8` is how
the board starts (full power); `I9` is a little stronger (the supply raised
from 3.3 to 3.4 V); `I1` to `I7` are gentler (and quieter): the motors are
driven at part power, each buzz starting with a short full-power kick
(30 ms) so the motor spins up. A coin motor's force goes about with the
square of its drive, so each level is roughly a quarter weaker than the one
above; `I1` is about a fifth of `I8` (estimates: phyphox can measure it).
Stored; survives switching off. The timing is the same at every level.

| `I` | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|
| drive, % | 45 | 52 | 59 | 66 | 74 | 82 | 91 | 100 | 100 |
| supply, V | 3.3 | 3.3 | 3.3 | 3.3 | 3.3 | 3.3 | 3.3 | 3.3 (as the board starts) | 3.4 |

## 12. Calibration

At first use (and after `C`) the device plays attention, then PARIS. Key
PARIS back (`.--. .- .-. .. ...`), then stop for 3 s or hold 2 s. OK means
calibrated; error means try again (the prompt replays). From then on it
keeps adapting to your keying. SOS works even before calibrating.

## 13. Morse code

A dash is three dots long; one dot of silence between the parts of a
letter, three between letters, seven between words.

**Letters**

| Letter | Code | Letter | Code |
|---|---|---|---|
| A | `.-` | N | `-.` |
| B | `-...` | O | `---` |
| C | `-.-.` | P | `.--.` |
| D | `-..` | Q | `--.-` |
| E | `.` | R | `.-.` |
| F | `..-.` | S | `...` |
| G | `--.` | T | `-` |
| H | `....` | U | `..-` |
| I | `..` | V | `...-` |
| J | `.---` | W | `.--` |
| K | `-.-` | X | `-..-` |
| L | `.-..` | Y | `-.--` |
| M | `--` | Z | `--..` |

**Digits**

| Digit | Code | Digit | Code |
|---|---|---|---|
| 1 | `.----` | 6 | `-....` |
| 2 | `..---` | 7 | `--...` |
| 3 | `...--` | 8 | `---..` |
| 4 | `....-` | 9 | `----.` |
| 5 | `.....` | 0 | `-----` |

**Punctuation**

| Sign | Code | Sign | Code |
|---|---|---|---|
| . | `.-.-.-` | , | `--..--` |
| ? | `..--..` | ' | `.----.` |
| ! | `-.-.--` | / | `-..-.` |
| ( | `-.--.` | ) | `-.--.-` |
| & | `.-...` | : | `---...` |
| ; | `-.-.-.` | = | `-...-` |
| + | `.-.-.` | - | `-....-` |
| _ | `..--.-` | " | `.-..-.` |
| $ | `...-..-` | @ | `.--.-.` |

**Prosigns:** `&` doubles as AS (wait), `+` as AR (end of message), `=` as
BT (break); they are sent as ordinary characters. `...---...` run together
is SOS (section 7). Eight dots (7 or more) is error.

IDs use the letters and digits without 0, 1, I, L and O (they are too easy
to mix up): 3 or 4 characters, such as K7Q.

## 14. The console (USB, for tests)

The board's USB port, 115200 baud, in a serial monitor. `status` shows
everything; `help` lists all commands.

| Command | Does |
|---|---|
| `status` | state, link and paths, radio, calibration, contacts, battery, draft |
| `id K7Q` | set the device ID |
| `radio` | the radio: chip, SF and range level, power, airtime used, noise |
| `radio sf 9-12` | the range level by its SF (as `G 1-4`; not during a test) |
| `radio power 2-22` | transmit power in dBm (default 22; the older SX1276 boards reach 20; not during a test) |
| `radio ping` | every device in range answers with how well it heard you |
| `radio log on/off` | log every packet with its RSSI and SNR |
| `test start TEST [PATHS] [ROLE]` | test mode by hand (normally the app starts it). Tests: `baseline`, `dynamic`, `point` (the default sweep), `rundown-a`, `rundown-b`, `rundown-l` (LoRa only, Bluetooth off), `rundown-m` (PARIS on the motors); paths `both`, `lora`, `relay`; role `rover`, `base` |
| `test start test=point plan=9:17,12:22 role=rover` | a point test with its own plan (SF:dBm steps; `n=` sets the messages per step, 1-20) |
| `test stop` | end the test (or hold the key 5 s) |
| `test dump` | the last run-down's battery log, as CSV |
| `bench power` | the meter session: the board steps through its states by itself, the wrist motor buzzing each step's number (the test plan's sections 14.1 and 17). It waits until USB is unplugged; the app can start it without USB at all |
| `bench motor [m1/m2]` | the motor pattern for phyphox (section 14.3 of the test plan) |
| `bench stop` | end either |
| `text [-s] HELLO` | key a message by injection (`-s`: and send it) |
| `key PATTERN` | inject keying: `.` `-`, space = letter gap, `/` = word gap, `S` / `C` / `X` = 2 / 3 / 5 s holds |
| `phone` | the phone link (also in `status`) |
| `phone pair` / `phone forget` | as `O` / forget the paired phone |
| `phone off` / `phone on` | as `W`: Bluetooth off (stored) / on again |
| `loopback on/off` | one board alone: a pretend partner answers instead of the radio (until the next restart) |
| `speed 1-9`, `haptic ...`, `motor M1 M2` | reading speed, motor tuning (`haptic strength 1-9`: as `I`; `haptic dash 1/2`: as `H`; `haptic overlap 30` / `split 125`: the dash's shape, in hundredths of a unit), the motors directly |
| `peer add ID`, `peer del SLOT` | edit the contacts |
| `verbose on/off` | log each keyed character's timing and everything played |
| `off`, `restart`, `calibrate`, `factory-reset yes` | as F; reboot; recalibrate; erase everything but the ID (the paired phone too) |
