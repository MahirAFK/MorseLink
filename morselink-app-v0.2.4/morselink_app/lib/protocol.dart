// MorseLink - what the device and the phone say to each other.
//
// The device's side is the firmware's main/phone.h: one line of ASCII per
// message, fields separated by single spaces, a text field always last.
// Test-mode log lines start with "T," (firmware main/ntest.c, and
// docs/TEST_PLAN.md section 4.4). Relayed packets are base64 in RL lines;
// their 10-byte header is described in main/proto.h.
//
// Protocol 2 (firmware v0.6.0) adds, at the ends of lines and as new tags:
// each path's state in ST (LoRa and the relay), the radio setting (RD), the
// command being keyed (CM) and how command mode ended (CE), and a detail in
// EV lines (why a link was lost; a path's new state). A protocol 1 board
// (v0.5.x) simply doesn't send them, and the app shows what it has.
//
// Protocol 3 (firmware v0.6.1) adds the point test (TEST START test=point
// plan=...; STEP lines), the meter session (BENCH POWER / BENCH STOP to the
// board, BN lines back) and EV BLE_OFF (W switched the board's Bluetooth
// off).
//
// Protocol 4 (firmware v0.6.2) adds the learning view's lines: KY (each
// press), KC (each letter), CB (a PARIS attempt judged), SP (the keying and
// reading speeds) and PL (what plays on the motors), read by learning.dart;
// and the I command (vibration strength) in command mode.

import 'dart:convert';
import 'dart:typed_data';

/// The protocol this app speaks (HI's second field; the firmware's
/// PHONE_PROTOCOL).
const int appProtocol = 4;

/// The characters a device ID is made of (main/morse.h).
const String idAlphabet = '23456789ABCDEFGHJKMNPQRSTUVWXYZ';
const int _base3 = 31 * 31 * 31; // 3-character IDs come first, then 4-character ones
const int addrBroadcast = 0xFFFFFF;

/// The printable ID of a 24-bit device address: "ALL" for broadcast, "?"
/// if it is not an ID (the inverse of the firmware's id_to_addr()).
String addrToId(int a) {
  if (a == addrBroadcast) return 'ALL';
  int n;
  if (a < _base3) {
    n = 3;
  } else if (a < _base3 + _base3 * 31) {
    n = 4;
    a -= _base3;
  } else {
    return '?';
  }
  final out = List<String>.filled(n, '');
  for (var i = n - 1; i >= 0; i--) {
    out[i] = idAlphabet[a % 31];
    a ~/= 31;
  }
  return out.join();
}

/// Packet type names, as the firmware's logs spell them (proto_type_name()).
const List<String> packetTypeNames = [
  'NONE', 'DISCOVER', 'PRESENCE', 'LINK_REQ', 'LINK_REPLY', 'PAIR_CONFIRM', 'KEEPALIVE', //
  'MSG', 'RECEIPT', 'SOS', 'SOS_ACK', 'SOS_CANCEL', 'UNPAIR', 'PING',
];

/// What a relayed packet's header says, for the log (the phone never needs
/// more: it passes packets on without understanding them).
class PacketInfo {
  final String type;
  final String src;
  final String dst;
  final int id;
  final int bytes;

  PacketInfo(this.type, this.src, this.dst, this.id, this.bytes);

  /// Decode the header of a base64 packet; null if it isn't one.
  static PacketInfo? fromBase64(String b64) {
    Uint8List b;
    try {
      b = base64Decode(b64);
    } catch (_) {
      return null;
    }
    if (b.length < 10) return null;
    final t = b[0] & 0x0F;
    final type = t < packetTypeNames.length ? packetTypeNames[t] : '?';
    final src = (b[2] << 16) | (b[3] << 8) | b[4];
    final dst = (b[5] << 16) | (b[6] << 8) | b[7];
    final id = (b[8] << 8) | b[9];
    return PacketInfo(type, addrToId(src), addrToId(dst), id, b.length);
  }

  Map<String, Object> toJson() => {'type': type, 'src': src, 'dst': dst, 'id': id, 'bytes': bytes};
}

final RegExp _b64 = RegExp(r'^[A-Za-z0-9+/]+={0,2}$');

/// Is `s` plausibly a base64 packet (so nothing else is ever written to the
/// device as an RL line)? A packet is 10..122 bytes: 16..164 characters.
bool isBase64Packet(String s) => s.length >= 16 && s.length <= 200 && s.length % 4 == 0 && _b64.hasMatch(s);

/// Split `s` into at most `n` space-separated words; the last keeps the
/// rest of the line, spaces and all (for the text fields).
List<String> splitN(String s, int n) {
  final out = <String>[];
  var rest = s;
  while (out.length < n - 1) {
    final i = rest.indexOf(' ');
    if (i < 0) break;
    out.add(rest.substring(0, i));
    rest = rest.substring(i + 1);
  }
  out.add(rest);
  return out;
}

/// Fields of a T line that are always text, even when they look like a
/// number: a board ID such as "234" or "2E5" must stay an ID in the logs.
const Set<String> _textFields = {
  'src', 'dst', 'own', 'peer', 'session', 'role', 'test', 'paths', 'fw', //
  'type', 'path', 'why', 'reason', 'state', 'lora', 'lora_skip', 'relay', 'relay_skip',
  'plan', // (v0.6.1: a point test's START, "9:17/9:22/12:22")
};

/// A plain decimal number: "-7", "420", "9.5" (not "2E5", "0x1A" or "NaN",
/// which Dart's parsers would also take).
final RegExp _decimal = RegExp(r'^-?[0-9]+(\.[0-9]+)?$');

/// One test-mode log line from the board: "T,<n>,<board ms>,<EVENT>,k=v,...".
/// Plain decimal values become numbers (except the text fields above).
class TestLine {
  final int n;
  final int ms;
  final String ev;
  final Map<String, Object> fields;

  TestLine(this.n, this.ms, this.ev, this.fields);

  static TestLine? parse(String line) {
    final parts = line.split(',');
    if (parts.length < 4 || parts[0] != 'T') return null;
    final n = int.tryParse(parts[1]);
    final ms = int.tryParse(parts[2]);
    if (n == null || ms == null) return null;
    final f = <String, Object>{};
    for (var i = 4; i < parts.length; i++) {
      final eq = parts[i].indexOf('=');
      if (eq <= 0) continue;
      final k = parts[i].substring(0, eq);
      final v = parts[i].substring(eq + 1);
      if (_textFields.contains(k) || !_decimal.hasMatch(v)) {
        f[k] = v;
      } else {
        f[k] = int.tryParse(v) ?? double.parse(v); // (a very long integer becomes a double)
      }
    }
    return TestLine(n, ms, parts[3], f);
  }

  String str(String k) => '${fields[k] ?? ''}';
  int? integer(String k) {
    final v = fields[k];
    return v is int ? v : null;
  }

  num? number(String k) {
    final v = fields[k];
    return v is num ? v : null;
  }
}

/// The radio setting a board reports (RD <sf> <bandwidth kHz> <dBm>).
class RadioInfo {
  final int sf; // spreading factor, 9-12
  final int bwKhz;
  final int dbm; // transmit power

  const RadioInfo(this.sf, this.bwKhz, this.dbm);

  /// The range level the device's G command uses for this SF: SF9 = 1 ...
  /// SF12 = 4.
  int get level => sf - 8;

  /// "range level 1 (SF9), 22 dBm".
  String get text => 'range level $level (SF$sf), $dbm dBm';

  /// From an RD line's fields ("9 125 22"); null if they aren't numbers.
  static RadioInfo? parse(String rest) {
    final f = rest.trim().split(' ');
    if (f.length < 3) return null;
    final sf = int.tryParse(f[0]);
    final bw = int.tryParse(f[1]);
    final dbm = int.tryParse(f[2]);
    if (sf == null || bw == null || dbm == null) return null;
    return RadioInfo(sf, bw, dbm);
  }

  /// From the 'radio' control message another phone sent (toJson's map);
  /// null if it has no setting (that phone's board isn't connected).
  static RadioInfo? fromJson(Object? m) {
    if (m is! Map) return null;
    final sf = m['sf'];
    final bw = m['bw'];
    final dbm = m['dbm'];
    if (sf is! int || bw is! int || dbm is! int) return null;
    return RadioInfo(sf, bw, dbm);
  }

  Map<String, Object> toJson() => {'sf': sf, 'bw': bwKhz, 'dbm': dbm};

  @override
  bool operator ==(Object other) => other is RadioInfo && other.sf == sf && other.bwKhz == bwKhz && other.dbm == dbm;

  @override
  int get hashCode => Object.hash(sf, bwKhz, dbm);
}

/// How good a path's state is, for its colour.
enum PathLevel { good, warn, bad, neutral }

/// A path's state (ST's last two fields; the detail of EV LORA / RELAY) in
/// words. `who` is the partner's ID.
String pathStateText(String state, String who) {
  switch (state) {
    case 'connected':
      return 'connected';
    case 'checking':
      return 'checking...';
    case 'they-cant-hear':
      return "one-way: $who can't hear you";
    case 'cant-hear-them':
      return "one-way: you can't hear $who";
    case 'lost':
      return 'lost';
    case 'off':
      return 'off';
    case 'none':
    case '':
      return '-';
    default:
      return state;
  }
}

/// The colour class of a path state.
PathLevel pathLevel(String state) {
  switch (state) {
    case 'connected':
      return PathLevel.good;
    case 'they-cant-hear':
    case 'cant-hear-them':
      return PathLevel.warn;
    case 'lost':
      return PathLevel.bad;
    default:
      return PathLevel.neutral;
  }
}

/// Why a link was lost (EV LOST's detail; LINK lines' why=).
String lostWhyText(String why) {
  switch (why) {
    case 'silence':
      return 'nothing heard for too long';
    case 'no-receipt':
      return 'a message got no receipt';
    case 'one-way':
      return "one-way: one side can't hear the other";
    default:
      return why;
  }
}

/// What each command letter does in the device's command mode
/// (docs/MANUAL.md section 4), for the keying box.
const Map<String, String> commandNames = {
  'P': 'connect to a new ID',
  'S': 'select the recipient (slot or ID)',
  'A': 'accept a request',
  'N': 'decline a request',
  'D': 'discover who is near',
  'L': 'list the contacts',
  'X': 'forget a contact / unblock',
  'R': 'replay a received message',
  'E': 'read back the draft',
  'B': 'battery and link',
  'Q': 'reading speed 1-9',
  'G': 'range level 1-4 (alone: which one?)',
  'H': 'dashes: 1 sweep, 2 forearm motor only (alone: which?)',
  'I': 'vibration strength 1-9, 8 normal (alone: which one?)',
  'K': 'keep the draft past 60 s, on / off',
  'Z': 'silent mode on / off',
  'C': 'recalibrate',
  'O': 'pair a phone (60 s)',
  'W': 'Bluetooth off / on',
  'F': 'switch off',
  'V': 'verify (not available yet)',
};

/// How command mode ended (a CE line), for the keying box.
String commandEndText(String how, String command) {
  final c = command.isEmpty ? 'The command' : command;
  switch (how) {
    case 'done':
      return command.isEmpty ? 'Command mode left' : '$command: done';
    case 'cancelled':
      return 'Command mode cancelled (5 s hold)';
    case 'left':
      return 'Command mode left (3 s hold); the draft is kept';
    case 'timeout':
      return 'Command mode ended: 30 s untouched';
    case 'sos':
      return 'Command mode left for the SOS';
    case 'aborted':
      return '$c: aborted during the echo';
    case 'error':
      return '$c: not understood';
    default:
      return '$how $command';
  }
}

/// Why the board refused a test (REFUSED reason=...), in words.
String refusalText(String reason) {
  switch (reason) {
    case 'running':
      return 'a test is already running on the board';
    case 'no-id':
      return 'the board has no ID yet';
    case 'loopback':
      return 'loopback is on (console: loopback off)';
    case 'no-partner':
      return "the board isn't linked to anyone (S on the device)";
    case 'link-lost':
      return 'the link to the other board is lost';
    case 'lora-lost':
      return 'LoRa to the other board is down (only the relay works). Are both on the same range level (G)?';
    case 'busy':
      return 'a message of yours still waits for its receipt: try again in a moment';
    case 'no-memory':
      return 'the board is out of memory (restart it)';
    case 'sos':
      return 'your SOS is active (a 5 s hold cancels it)';
    case 'unknown test':
      return "the board's firmware doesn't know this test (the point test needs v0.6.1)";
    case 'bad plan':
      return "the board didn't understand the plan (SF 9-12, power 2-22 dBm, at most 8 steps)";
    default:
      return reason;
  }
}

/// A test's pace at this spreading factor, in words (the firmware's
/// ttest.c: the gaps grow with the SF so the airtime stays near 5 %).
String testPaceText(String test, int sf) {
  final i = sf >= 10 && sf <= 12 ? sf - 9 : 0;
  const grid = [20, 30, 45, 90]; // baseline: one message every ... s
  const lo = [10, 20, 40, 90]; // the others: a random gap of lo..hi s
  const hi = [20, 40, 60, 130];
  switch (test) {
    case 'baseline':
      final n = i == 3 ? 60 : 120;
      return '$n messages each (5/25/50/100 characters), one every ${grid[i]} s (${n * grid[i] ~/ 60} min)';
    case 'dynamic':
    case 'rundown-b':
    case 'rundown-l':
      return 'a message every ${lo[i]}-${hi[i]} s, 5-100 characters';
    case 'point':
      return '$pointMessages messages of $pointSize characters per step from each board, one every '
          '${pointGapS(sf)} s at SF$sf';
    default:
      return '';
  }
}

// ---------------------------------------------------------------------------
// The point test (firmware v0.6.1, ttest.c): both boards still, a plan of
// radio settings. Each board computes the timetable from the plan alone;
// this is the same arithmetic, so the app can show the steps and count down.
// ---------------------------------------------------------------------------

/// Messages per board per step, and their size (TT_POINT_N, TT_POINT_SIZE).
const int pointMessages = 20;
const int pointSize = 25;

/// The most steps a plan may have (TT_PLAN_MAX).
const int planMax = 8;

/// Spreading factors and powers the test page offers (the firmware takes
/// any power 2-22; the boards clamp to their chip).
const List<int> planSfs = [9, 10, 11, 12];
const List<int> planDbms = [17, 20, 22];

int _sfIndex(int sf) => sf >= 10 && sf <= 12 ? sf - 9 : 0;

/// The gap between one board's messages at `sf`, s (tt_point_gap_ms): longer
/// than a message that fails takes to give up, so each is a try of its own.
int pointGapS(int sf) => const [15, 20, 25, 40][_sfIndex(sf)];

/// The settle time at the start of a step, s (tt_point_settle_ms).
int pointSettleS(int sf) => _sfIndex(sf) >= 2 ? 20 : 10;

/// How long a step at `sf` lasts with n messages: settle + (n + 1) gaps.
int pointStepS(int sf, [int n = pointMessages]) => pointSettleS(sf) + (n + 1) * pointGapS(sf);

/// One step of a plan: the setting both boards use.
class PlanStep {
  final int sf;
  final int dbm;

  const PlanStep(this.sf, this.dbm);

  /// "SF9 / 17 dBm".
  String get text => 'SF$sf / $dbm dBm';

  @override
  bool operator ==(Object other) => other is PlanStep && other.sf == sf && other.dbm == dbm;

  @override
  int get hashCode => Object.hash(sf, dbm);
}

/// The default sweep (agreed 20 Oct): SF9 / 17, SF9 / 22, SF12 / 22 dBm.
const List<PlanStep> defaultPlan = [PlanStep(9, 17), PlanStep(9, 22), PlanStep(12, 22)];

/// The plan as TEST START spells it ("9:17,9:22,12:22"); the board's T lines
/// write it with '/' between the steps (their fields are comma-separated).
String planText(List<PlanStep> plan, [String sep = ',']) => plan.map((p) => '${p.sf}:${p.dbm}').join(sep);

/// A plan from either spelling; null if it isn't one.
List<PlanStep>? parsePlan(String s) {
  final out = <PlanStep>[];
  for (final part in s.split(RegExp('[,/]'))) {
    final f = part.split(':');
    if (f.length != 2) return null;
    final sf = int.tryParse(f[0]);
    final dbm = int.tryParse(f[1]);
    if (sf == null || dbm == null || sf < 9 || sf > 12 || dbm < 2 || dbm > 22) return null;
    out.add(PlanStep(sf, dbm));
  }
  return out.isEmpty || out.length > planMax ? null : out;
}

/// The whole plan's length, s.
int planSeconds(List<PlanStep> plan) => plan.fold(0, (t, p) => t + pointStepS(p.sf));

/// "25 min" / "5 min 25 s".
String minutesText(int s) {
  final m = s ~/ 60;
  final r = s % 60;
  if (m == 0) return '$r s';
  return r == 0 ? '$m min' : '$m min ${r.toString().padLeft(2, '0')} s';
}

/// A short sentence for an EV line (phone.h, PHONE_EV_*). `detail` is the
/// third field (protocol 2): LOST's reason, LORA's and RELAY's new state.
String eventText(String what, String who, [String detail = '']) {
  switch (what) {
    case 'LINKED':
      return 'Connected to $who';
    case 'BUSY':
      return '$who is busy: your request waits in their queue';
    case 'DECLINED':
      return '$who declined';
    case 'UNAVAILABLE':
      return '$who did not answer';
    case 'FAILED':
      return 'Connecting to $who failed';
    case 'REQUEST':
      return '$who asks to connect (A or N on the device)';
    case 'LOST':
      return detail.isEmpty ? 'Link to $who lost' : 'Link to $who lost: ${lostWhyText(detail)}';
    case 'RESTORED':
      return 'Link to $who back';
    case 'ENDED':
      return '$who linked with someone else';
    case 'UNPAIRED':
      return '$who forgot this device';
    case 'SOS':
      return 'SOS from $who';
    case 'SOS_ACK':
      return '$who acknowledged your SOS';
    case 'SOS_CANCEL':
      return '$who cancelled their SOS';
    case 'SOS_SENT':
      return who == 'ALL' ? 'Your SOS went out to everyone' : 'Your SOS went out to $who';
    case 'SOS_STOPPED':
      return 'Your SOS was cancelled';
    case 'LORA':
      return detail == 'connected' ? 'LoRa to $who: back' : 'LoRa to $who: ${pathStateText(detail, who)}';
    case 'RELAY':
      return detail == 'connected' ? 'Relay to $who: back' : 'Relay to $who: ${pathStateText(detail, who)}';
    case 'BLE_OFF':
      return 'Bluetooth switched off on the board (W): W again on the board turns it on';
    default:
      return detail.isEmpty ? '$what $who' : '$what $who $detail';
  }
}
