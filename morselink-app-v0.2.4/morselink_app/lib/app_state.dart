// MorseLink - everything the app knows and does, in one place. The screens
// (lib/ui) only show it and call its methods; it tells them to redraw with
// notifyListeners().
//
//   - the device: its lines over Bluetooth (ble_link.dart, protocol.dart)
//     become the status, the live draft, the chat and the events;
//   - the relay: RL lines from the device are published on MQTT, packets
//     on our topic go down to the device (relay.dart);
//   - tests (docs/TEST_PLAN.md): the rover phone starts a session on its own
//     board and, over MQTT, on the base phone and board; both phones log
//     the board's T lines, GPS, labels, Bluetooth and MQTT to a JSON-lines
//     file (storage.dart);
//   - a daily log of everything the device says (for finding problems);
//   - (v0.2.0) each path's state (LoRa, relay), the radio setting, command
//     mode as it is keyed, the radio setting exchanged with the partner's
//     phone (a warning before a test if the two boards differ), and the
//     background service that keeps it all running with the screen off;
//   - (v0.2.1) the point test (a plan of radio settings at one spot, with a
//     label; STEP lines; how the other board hears us, from its phone), the
//     meter session (BENCH POWER, BN lines, the same timetable as the board),
//     the board's Bluetooth switched off with W (EV BLE_OFF), and the field
//     fixes of 16-18 Oct: a red notice when the base phone hasn't answered
//     20 s after Start, a stop that is resent until the base confirms it,
//     and a stronger warning when the base phone looks offline;
//   - (v0.2.2) the learning view (firmware v0.6.2, protocol 4): every press
//     and letter as it is keyed (KY / KC), the PARIS calibration (CB), the
//     keying and reading speeds (SP), and what plays on the motors (PL), all
//     kept in `learn` (learning.dart) for the screens; received messages can
//     be hidden until tapped, for practising reading by feel.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:geolocator/geolocator.dart';

import 'background.dart';
import 'bench.dart';
import 'ble_link.dart';
import 'learning.dart';
import 'protocol.dart';
import 'relay.dart';
import 'storage.dart';

const String appVersion = '0.2.4';

enum ChatKind { sent, received, event, sos }

enum SendState { pending, delivered, failed }

/// One entry of the chat list.
class ChatItem {
  final ChatKind kind;
  final String who; // the other device's ID
  final String text;
  final int? n; // TX number (sent messages)
  SendState? state; // sent messages
  bool masked; // received while the learning view hid the text: shown when tapped (v0.2.2)
  final DateTime time = DateTime.now();

  ChatItem(this.kind, this.who, this.text, {this.n, this.state, this.masked = false});
}

/// Counters for one path (LoRa or relay) of the running test.
class PathCounts {
  int sent = 0;
  int ack = 0;
  int fail = 0;
  int skip = 0;

  String get rate => (ack + fail) == 0 ? '-' : '${(100 * ack / (ack + fail)).toStringAsFixed(0)} %';
}

/// What the rover's set-up form says about a session (TEST_PLAN section 2.4).
class TestMeta {
  String test = 'dynamic'; // baseline / dynamic / rundown-a / rundown-b / rundown-l / point
  String paths = 'both'; // both / lora / relay
  // the point test (v0.2.1)
  String label = ''; // the spot, typed: "pillar A, behind, 12 m"
  bool sweep = true; // a sweep (the plan below) or a single step
  List<PlanStep> plan = List.of(defaultPlan); // the sweep's steps
  PlanStep single = const PlanStep(9, 22); // the single step's setting
  String txDbm = '22'; // typed: used only when the board doesn't report its radio (firmware before v0.6.0)
  RadioInfo? radio; // this board's setting (RD), filled in at the start
  RadioInfo? partnerRadio; // the other board's, as its phone reported it
  String baseDesc = '';
  String baseHeight = '';
  String battery = '';
  String note = '';

  Map<String, Object> toJson() => {
        'test': test,
        'paths': paths,
        'tx_dbm': radio?.dbm ?? int.tryParse(txDbm) ?? txDbm,
        if (radio != null) 'sf': radio!.sf,
        if (radio != null) 'bw_khz': radio!.bwKhz,
        if (partnerRadio != null) 'peer_radio': partnerRadio!.toJson(),
        'base': baseDesc,
        'height': baseHeight,
        'carry': 'hand',
        'battery': battery,
        'note': note,
        if (test == 'point') 'label': label,
        if (test == 'point') 'plan': planText(steps),
      };

  /// The point test's plan as it will run: the sweep, or the one step.
  List<PlanStep> get steps => sweep ? plan : [single];

  /// A copy (Repeat at the next spot: the same plan, a new label).
  TestMeta copy() {
    final m = TestMeta()
      ..test = test
      ..paths = paths
      ..label = label
      ..sweep = sweep
      ..plan = List.of(plan)
      ..single = single
      ..txDbm = txDbm
      ..baseDesc = baseDesc
      ..baseHeight = baseHeight
      ..battery = battery
      ..note = note;
    return m;
  }
}

/// A test session on this phone (rover or base).
class TestSession {
  final String id; // the rover phone's start time, YYYYMMDD-HHMMSS
  final String role; // rover / base
  final String test;
  final String paths;
  final int seed;
  final String peer; // the other board's ID
  final DateTime started = DateTime.now();
  String board = 'starting'; // starting / running / refused (why) / stopping / stopped
  bool stopping = false;
  int gen = 0;
  final PathCounts lora = PathCounts();
  final PathCounts relay = PathCounts();
  num? rssi; // last packet heard over LoRa
  num? snr;
  int? batPct;
  String file = '';
  // v0.2.0, from the board's LINK and PATH lines (firmware v0.6.0)
  int lost = 0; // times the link was lost during the session
  String lastLostWhy = '';
  DateTime? lastLostAt;
  bool linkUp = true; // false from a LINK lost until its restored
  String pathL = ''; // LoRa's state, as the last PATH line said ('' = none yet)
  String pathR = ''; // the relay's
  // v0.2.1, the point test (the board's STEP lines; firmware v0.6.1)
  final List<PlanStep> plan; // its steps (empty: not a point test)
  String label; // the spot
  int step = 0; // the plan step on now, 1-based (0: none yet, or the board's own setting back)
  int stepSf = 0;
  int stepDbm = 0; // the power the board says it uses (clamped to its chip)
  DateTime? stepAt; // when that STEP line arrived
  int stepMs = 0; // how long the step lasts
  final Map<int, int> seqStep = {}; // message number -> its step (from GEN), to count results per step
  late final List<PathCounts> stepCounts = List.generate(plan.length, (_) => PathCounts());
  num? theirRssi; // how the other board hears us: its phone's latest 'pt' message
  num? theirSnr;
  DateTime? theirAt;
  DateTime? lastPtSent; // base: when we last told the rover how we hear it

  TestSession(this.id, this.role, this.test, this.paths, this.seed, this.peer,
      {this.plan = const [], this.label = ''});

  bool get isPoint => test == 'point';

  /// Seconds left in the current step (0 if none).
  int get stepLeftS {
    final at = stepAt;
    if (at == null || step <= 0) return 0;
    final left = stepMs ~/ 1000 - DateTime.now().difference(at).inSeconds;
    return left < 0 ? 0 : left;
  }
}

/// The meter session (bench power, firmware v0.6.1), as the board's BN lines
/// and the timetable (bench.dart) say.
class BenchRun {
  String state = 'asking'; // asking / refused / wait-usb / running / ended
  String detail = ''; // refused: why; ended: how
  DateTime? startedAt; // "BN start": the timetable counts from here
  int maxDbm = 22; // the board's chip (no 22 dBm step below 22)
  int totalS = 0; // the board's total (from BN start)
  List<BenchSlot> table = benchTimetable(22);
  String boardStep = ''; // the step the board last said (BN step), while connected
  final DateTime asked = DateTime.now();

  bool get active => state == 'asking' || state == 'wait-usb' || state == 'running';

  /// Where the session is now (null slot before step 1's signal).
  BenchNow now() {
    final at = startedAt;
    if (at == null) return const BenchNow(null, false, 0);
    return benchAt(table, DateTime.now().difference(at).inMilliseconds);
  }

  /// The board said step `id` begins now: follow its clock (a timetable
  /// started from a BN start the phone got late stays in step).
  void sync(String id) {
    boardStep = id;
    for (final s in table) {
      if (s.step.id == id) {
        startedAt = DateTime.now().subtract(Duration(milliseconds: s.startMs));
        return;
      }
    }
  }
}

/// A stop the rover sent and the base phone hasn't confirmed yet (resent
/// every 5 s for up to 2 minutes, and at once when the internet comes back).
class _PendingStop {
  final String session;
  final String peer;
  final DateTime since = DateTime.now();

  _PendingStop(this.session, this.peer);
}

/// One finding of the checks before a test starts.
class StartCheck {
  final bool blocking; // true: the test can't start (the board would refuse it, or nothing would be logged)
  final String text;
  final bool severe; // a warning that very likely means a wasted run (shown red, but "Start anyway" stays)

  const StartCheck(this.blocking, this.text, {this.severe = false});
}

class AppState extends ChangeNotifier {
  AppState(this.settings);

  final Settings settings;
  late final BleLink ble = BleLink(onLine: _onDeviceLine, onState: _onBleState);
  late final Relay relay = Relay(onMessage: _onMqtt, onState: _onMqttState);
  final JsonlLog _daily = JsonlLog();
  final JsonlLog _sess = JsonlLog();
  Directory? logDir;

  // ---- the device, as its lines describe it ----
  String ownId = '';
  String fw = '';
  String partner = '-';
  String link = 'none'; // none / connected / lost / ended
  String battery = '?';
  bool silent = false;
  String draft = '';
  String bleState = 'idle';
  String bleWhy = ''; // why the board isn't connected, or what to do (ble_link.dart)
  int mtu = 23;
  String mqttState = 'off';
  int protocol = 0; // the board's protocol (HI); 0 = not known yet
  bool accepted = false; // the board said HI on this connection (it accepts this phone)
  String lora = ''; // the LoRa path's state (ST, protocol 2); '' = not reported (older firmware)
  String relayPath = ''; // the relay path's state, as the board sees it
  RadioInfo? radio; // the board's radio setting (RD)
  String? cmd; // command mode: what has been keyed so far (null: not in command mode)
  String cmdEnd = ''; // how the last command ended, in words (shown for a few seconds)
  Timer? _cmdEndTimer;
  RadioInfo? partnerRadio; // the partner board's setting, as its phone said
  DateTime? partnerRadioAt;
  Completer<void>? _radioWait; // startChecks() waiting for the partner's phone to answer
  final List<ChatItem> chat = [];
  String? sosFrom; // someone's SOS is active
  bool ownSos = false; // ours is
  String? sosAckBy;

  // ---- tests ----
  TestSession? session;
  String lastSessionFile = '';
  String notice = ''; // the latest problem worth showing on the test page
  String baseStatus = ''; // what the base phone last said (rover)
  double? baseLat;
  double? baseLon;
  DateTime? basePosAt;
  String area = '';
  String situation = '';
  Position? fix;
  StreamSubscription<Position>? _gpsSub;
  Timer? _posTimer;
  Timer? _stopTimer;
  // v0.2.1
  String baseState = ''; // the base phone's last state word (starting / running / refused / no board / busy / stopped)
  bool baseAlarm = false; // rover: the base phone hasn't answered 20 s after Start, or its board isn't running
  Timer? _baseCheck;
  _PendingStop? _pendingStop;
  Timer? _stopResend;
  TestMeta? lastMeta; // the last test this phone started as the rover (Repeat at the next spot)
  DateTime? mqttConnectedAt; // when the internet relay last (re)connected
  bool bleOffByW = false; // the board switched its Bluetooth off with W (EV BLE_OFF)
  BenchRun? bench; // the meter session
  Timer? _benchTimer;
  DateTime? _lastKick;
  // v0.2.2
  final Learning learn = Learning(); // the learning view: keying, calibration, speeds, what plays

  /// Start everything that can start: logs, the remembered device.
  Future<void> init() async {
    relay.prefix = settings.prefix;
    relay.host = settings.broker;
    try {
      logDir = await logDirectory();
    } catch (_) {}
    final d = logDir;
    if (d != null) _daily.open(d, 'ML_log_${_ymd(DateTime.now())}.jsonl');
    _log({'kind': 'app', 'event': 'start', 'app': appVersion});
    var ticks = 0;
    Timer.periodic(const Duration(seconds: 5), (_) {
      _daily.flush();
      _sess.flush();
      if (++ticks % 12 == 0) _updateBackground(force: true); // once a minute, in case Android stopped it
    });
    if (settings.deviceId.isNotEmpty) {
      ble.use(settings.deviceId);
      ble.start();
    }
    _updateBackground();
    notifyListeners();
    await _locationAllowed(); // ask once, early (needed for tests)
  }

  // ------------------------------------------------------------------------
  // Logging
  // ------------------------------------------------------------------------

  static String _two(int v) => v.toString().padLeft(2, '0');
  static String _ymd(DateTime d) => '${d.year}${_two(d.month)}${_two(d.day)}';

  /// A record to the daily log and, during a session, the session file.
  void _log(Map<String, Object?> rec, {bool daily = true}) {
    if (daily) _daily.write(rec);
    if (_sess.isOpen) _sess.write(rec);
  }

  // ------------------------------------------------------------------------
  // The device
  // ------------------------------------------------------------------------

  /// The board picked on the connect page: remember it and connect.
  Future<void> useDevice(BluetoothDevice d, String name) async {
    await ble.forget();
    settings.deviceId = d.remoteId.str;
    settings.deviceName = name;
    await settings.save();
    ble.use(settings.deviceId);
    ble.start();
    _updateBackground();
    notifyListeners();
  }

  Future<void> forgetDevice() async {
    await ble.forget();
    await relay.stop();
    settings.deviceId = '';
    settings.deviceName = '';
    await settings.save();
    ownId = '';
    partner = '-';
    link = 'none';
    lora = '';
    relayPath = '';
    radio = null;
    partnerRadio = null;
    _updateBackground();
    notifyListeners();
  }

  /// The Settings switch for the background service.
  Future<void> setBackground(bool on) async {
    settings.background = on;
    await settings.save();
    _updateBackground();
    notifyListeners();
  }

  /// The background service's notification text: the board, its partner and
  /// the paths, in a few words.
  String get backgroundText {
    final name = settings.deviceName.isEmpty ? 'The board' : settings.deviceName;
    if (!ble.ready) return '$name: ${bleState == 'pairing' ? 'pairing' : 'not connected'}';
    if (partner == '-') return '$name connected; no partner';
    final paths = <String>[
      if (lora.isNotEmpty) 'LoRa ${pathStateText(lora, partner)}',
      if (relayPath.isNotEmpty) 'relay ${pathStateText(relayPath, partner)}',
    ];
    return '$name: $partner $link${paths.isEmpty ? '' : ' (${paths.join(', ')})'}';
  }

  /// Run the background service while a board is chosen (and the switch is
  /// on), with the current text; stop it otherwise.
  void _updateBackground({bool force = false}) {
    if (settings.deviceId.isEmpty || !settings.background) {
      Background.stop();
    } else {
      Background.show(backgroundText, force: force);
    }
  }

  /// New MQTT settings: reconnect with them.
  Future<void> applyRelaySettings(String prefix, String broker) async {
    settings.prefix = prefix.trim().isEmpty ? settings.prefix : prefix.trim();
    settings.broker = broker.trim().isEmpty ? settings.broker : broker.trim();
    await settings.save();
    relay.prefix = settings.prefix;
    relay.host = settings.broker;
    await relay.stop();
    if (ownId.isNotEmpty) relay.start(ownId);
    notifyListeners();
  }

  void _onBleState(String s, int m, String why) {
    bleState = s;
    bleWhy = why;
    if (bleOffByW && s != 'connected') {
      bleWhy = 'Bluetooth is off on the board (W): W again on the board turns it on (this phone tries every 30 s)';
    }
    mtu = m;
    if (s != 'connected') {
      accepted = false;
      cmd = null; // (we can't see command mode without the board)
      learn.boardGone(); // (nor what is keyed or playing)
    }
    _log({'kind': 'ble', 'state': s, 'mtu': m, if (why.isNotEmpty) 'why': why});
    _updateBackground();
    notifyListeners();
  }

  void _onMqttState(String s) {
    final was = mqttState;
    mqttState = s;
    _log({'kind': 'mqtt', 'state': s, 'broker': relay.host});
    if (s == 'connected' && was != 'connected') {
      mqttConnectedAt = DateTime.now();
      _publishRadio(partner); // (the partner's phone may have missed a change)
      _askRadio();
      _sendPendingStop(); // (a stop that couldn't go out while the internet was down)
    }
    _updateBackground();
    notifyListeners();
  }

  /// How the last command ended, shown in the keying box for 5 s.
  void _showCmdEnd(String text) {
    cmdEnd = text;
    _cmdEndTimer?.cancel();
    _cmdEndTimer = Timer(const Duration(seconds: 5), () {
      cmdEnd = '';
      notifyListeners();
    });
  }

  void _addChat(ChatItem c) {
    chat.add(c);
    if (chat.length > 300) chat.removeAt(0);
  }

  /// The learning view: how much of what plays the screen shows (v0.2.2).
  /// Messages that arrive while it isn't "all" stay hidden in the chat until
  /// tapped.
  void setShowMode(ShowMode m) {
    learn.mode = m;
    notifyListeners();
  }

  /// Show the last playback's text (morse / hidden modes).
  void revealPlayback() {
    learn.revealed = true;
    notifyListeners();
  }

  /// Show a received message that was hidden for practice.
  void revealChat(ChatItem c) {
    c.masked = false;
    notifyListeners();
  }

  /// One line from the device (phone.h).
  void _onDeviceLine(String line) {
    if (line.startsWith('T,')) {
      _onTestLine(line);
      notifyListeners();
      return;
    }
    final sp = line.indexOf(' ');
    final tag = sp < 0 ? line : line.substring(0, sp);
    final rest = sp < 0 ? '' : line.substring(sp + 1);
    if (tag != 'RL') _log({'kind': 'dev', 'line': line});
    switch (tag) {
      case 'HI':
        final f = rest.split(' ');
        accepted = true;
        if (bleOffByW) {
          bleOffByW = false; // (W again: the board is back)
          ble.slow = false;
        }
        if (f.isNotEmpty && f[0].isNotEmpty && f[0] != '-') _setOwnId(f[0]);
        if (f.length >= 2) protocol = int.tryParse(f[1]) ?? 0;
        if (f.length >= 3) fw = f[2];
        break;
      case 'ST':
        // ST <partner> <link> <battery> <silent> [<lora> <relay>] (paths: protocol 2)
        final f = rest.split(' ');
        if (f.length >= 4) {
          final was = partner;
          partner = f[0];
          link = f[1];
          battery = f[2];
          silent = f[3] == '1';
          lora = f.length >= 6 ? f[4] : '';
          relayPath = f.length >= 6 ? f[5] : '';
          if (partner != was) {
            partnerRadio = null; // (that was the old partner's)
            partnerRadioAt = null;
            _publishRadio(partner);
            _askRadio();
          }
          _updateBackground();
        }
        break;
      case 'RD':
        // RD <sf> <bandwidth kHz> <dBm> (protocol 2): tell the partner's phone when it changes
        final r = RadioInfo.parse(rest);
        if (r != null && r != radio) {
          radio = r;
          _publishRadio(partner);
        }
        break;
      case 'CM':
        // Command mode: what has been keyed so far ("CM" alone: nothing yet)
        cmd = rest;
        break;
      case 'CE':
        // CE <how> [<command>]: aborted / error stay in command mode (a CM follows)
        final f = splitN(rest, 2);
        _showCmdEnd(commandEndText(f[0], f.length > 1 ? f[1] : ''));
        if (f[0] != 'aborted' && f[0] != 'error') cmd = null;
        break;
      case 'DR':
        draft = rest;
        learn.onDraft(rest); // (the patterns above the letters)
        break;
      // The learning view (protocol 4, firmware v0.6.2): learning.dart
      case 'KY':
        learn.onKY(rest, DateTime.now());
        break;
      case 'KC':
        learn.onKC(rest, DateTime.now());
        break;
      case 'CB':
        learn.onCB(rest, DateTime.now());
        break;
      case 'SP':
        learn.onSP(rest);
        break;
      case 'PL':
        learn.onPL(rest, DateTime.now());
        break;
      case 'TX':
        final f = splitN(rest, 3);
        if (f.length == 3) _addChat(ChatItem(ChatKind.sent, f[1], f[2], n: int.tryParse(f[0]), state: SendState.pending));
        break;
      case 'OK':
      case 'NO':
        final n = int.tryParse(rest.trim());
        for (var i = chat.length - 1; i >= 0; i--) {
          final c = chat[i];
          if (c.kind == ChatKind.sent && c.n == n && c.state == SendState.pending) {
            c.state = tag == 'OK' ? SendState.delivered : SendState.failed;
            break;
          }
        }
        break;
      case 'RX':
        final f = splitN(rest, 2);
        if (f.length == 2) {
          _addChat(ChatItem(ChatKind.received, f[0], f[1], masked: learn.mode != ShowMode.all));
        }
        break;
      case 'EV':
        // EV <what> <who> [<detail>] (detail: protocol 2)
        final f = splitN(rest, 3);
        if (f.length >= 2) _onEvent(f[0], f[1], f.length > 2 ? f[2] : '');
        break;
      case 'RL':
        final f = rest.split(' ');
        if (f.length == 2) _relayOut(f[0], f[1]);
        break;
      case 'BN':
        // The meter session (protocol 3): BN <what> [<detail>]
        _onBench(splitN(rest, 2));
        break;
    }
    notifyListeners();
  }

  void _setOwnId(String id) {
    if (id == ownId) return;
    ownId = id;
    relay.prefix = settings.prefix;
    relay.host = settings.broker;
    relay.start(id);
  }

  void _onEvent(String what, String who, String detail) {
    switch (what) {
      case 'SOS':
        sosFrom = who;
        break;
      case 'SOS_CANCEL':
        if (sosFrom == who) sosFrom = null;
        break;
      case 'SOS_SENT':
        ownSos = true;
        sosAckBy = null;
        break;
      case 'SOS_STOPPED':
        ownSos = false;
        sosAckBy = null;
        break;
      case 'SOS_ACK':
        sosAckBy = who;
        break;
      case 'BLE_OFF':
        // W on the board: its Bluetooth goes off on purpose. Keep the board,
        // try less often, say why.
        bleOffByW = true;
        ble.slow = true;
        break;
    }
    var text = eventText(what, who, detail);
    // One path down while the other works: say that messages still get through.
    if (what == 'LORA' && detail != 'connected' && relayPath == 'connected') {
      text += ' (the relay still carries messages)';
    } else if (what == 'RELAY' && detail != 'connected' && lora == 'connected') {
      text += ' (LoRa still carries messages)';
    }
    final sos = what.startsWith('SOS');
    _addChat(ChatItem(sos ? ChatKind.sos : ChatKind.event, who, text));
  }

  /// Dismiss the SOS banner (the device keeps its own state).
  void clearSos() {
    sosFrom = null;
    if (!ownSos) sosAckBy = null;
    notifyListeners();
  }

  // ------------------------------------------------------------------------
  // The relay
  // ------------------------------------------------------------------------

  /// RL <ID> <b64> from the device: publish it to <ID>'s topic.
  void _relayOut(String dst, String b64) {
    if (!isBase64Packet(b64)) return;
    final topic = '${settings.prefix}/$dst';
    final ok = relay.publish(topic, b64);
    final info = PacketInfo.fromBase64(b64);
    _log({'kind': 'relay', 'dir': ok ? 'pub' : 'drop', 'topic': topic, ...?info?.toJson()});
  }

  void _onMqtt(String topic, String payload) {
    final own = '${settings.prefix}/$ownId';
    if (topic == own) {
      final b64 = payload.trim();
      if (!isBase64Packet(b64)) return;
      final info = PacketInfo.fromBase64(b64);
      _log({'kind': 'relay', 'dir': 'sub', 'topic': topic, 'ble': ble.ready ? 1 : 0, ...?info?.toJson()});
      ble.sendLine('RL $b64');
    } else if (topic == '$own/ctl') {
      Map<String, dynamic>? m;
      try {
        final v = jsonDecode(payload);
        if (v is Map<String, dynamic>) m = v;
      } catch (_) {}
      if (m == null) return;
      _log({'kind': 'ctl', 'dir': 'in', 'msg': m});
      _onCtl(m);
      notifyListeners();
    }
  }

  void _publishCtl(String toId, Map<String, Object?> m) {
    final topic = '${settings.prefix}/$toId/ctl';
    final ok = relay.publish(topic, jsonEncode(m));
    _log({'kind': 'ctl', 'dir': ok ? 'out' : 'drop', 'to': toId, 'msg': m});
  }

  /// Tell board `to`'s phone this board's radio setting (the rover warns
  /// before a test if the two boards differ). Sent when the setting or the
  /// partner changes, when the internet relay connects, and when asked.
  /// "board": false if this phone's board isn't connected (no setting then).
  void _publishRadio(String to) {
    if (to == '-' || to.isEmpty || ownId.isEmpty || !relay.connected) return;
    final r = radio;
    _publishCtl(to, {'cmd': 'radio', 'from': ownId, 'board': ble.ready, if (r != null) ...r.toJson()});
  }

  /// Ask the partner's phone for its board's setting (answered by 'radio').
  void _askRadio() {
    if (partner == '-' || partner.isEmpty || ownId.isEmpty || !relay.connected) return;
    _publishCtl(partner, {'cmd': 'radio?', 'from': ownId});
  }

  /// Make sure the partner's setting is recent (10 min): ask, and wait up to
  /// 3 s for the answer.
  Future<void> _freshPartnerRadio() async {
    final at = partnerRadioAt;
    if (at != null && DateTime.now().difference(at).inMinutes < 10) return;
    final w = Completer<void>();
    _radioWait = w;
    _askRadio();
    try {
      await w.future.timeout(const Duration(seconds: 3));
    } catch (_) {}
    if (identical(_radioWait, w)) _radioWait = null;
  }

  // ------------------------------------------------------------------------
  // Tests (docs/TEST_PLAN.md)
  // ------------------------------------------------------------------------

  static String sessionIdFor(DateTime d) =>
      '${_ymd(d)}-${_two(d.hour)}${_two(d.minute)}${_two(d.second)}';

  Future<bool> _locationAllowed() async {
    try {
      if (!await Geolocator.isLocationServiceEnabled()) return false;
      var p = await Geolocator.checkPermission();
      if (p == LocationPermission.denied) p = await Geolocator.requestPermission();
      return p == LocationPermission.always || p == LocationPermission.whileInUse;
    } catch (_) {
      return false;
    }
  }

  /// GPS once a second while a session runs. Its foreground notification
  /// (and wake lock) also keeps the app working with the screen off.
  Future<void> _startGps() async {
    if (_gpsSub != null) return;
    if (!await _locationAllowed()) {
      notice = 'GPS is off or not allowed: the session runs without positions';
      return;
    }
    final settingsGps = AndroidSettings(
      accuracy: LocationAccuracy.best,
      distanceFilter: 0,
      intervalDuration: const Duration(seconds: 1),
      foregroundNotificationConfig: ForegroundNotificationConfig(
        notificationTitle: 'MorseLink test running',
        notificationText: 'Logging the board, GPS and the relay',
        enableWakeLock: true,
      ),
    );
    _gpsSub = Geolocator.getPositionStream(locationSettings: settingsGps).listen((p) {
      fix = p;
      _log({
        'kind': 'gps',
        'lat': p.latitude,
        'lon': p.longitude,
        'acc': p.accuracy,
        'alt': p.altitude,
        'spd': p.speed,
        'hdg': p.heading,
      }, daily: false);
      notifyListeners();
    }, onError: (Object e) {
      notice = 'GPS stopped: $e';
    });
  }

  Future<void> _stopGps() async {
    await _gpsSub?.cancel();
    _gpsSub = null;
  }

  /// Distance from the rover to the base, metres (null if unknown).
  double? get distanceToBase {
    final f = fix;
    final la = baseLat;
    final lo = baseLon;
    if (f == null || la == null || lo == null) return null;
    return Geolocator.distanceBetween(f.latitude, f.longitude, la, lo);
  }

  /// Open the session file and write its first record. The radio fields
  /// are this phone's own board's (RD; the meta's otherwise, typed on older
  /// firmware); peer_radio is the other board's, as its phone said.
  bool _openSession(TestSession s, Map<String, Object?> meta, {RadioInfo? peerRadio}) {
    final d = logDir;
    if (d == null) return false;
    final name = 'ML_${s.id}_${s.role}_$ownId.jsonl';
    if (!_sess.open(d, name)) return false;
    s.file = _sess.path;
    final r = radio;
    _sess.write({
      'kind': 'session',
      'session': s.id,
      'role': s.role,
      'test': s.test,
      'paths': s.paths,
      'seed': s.seed,
      ...meta,
      if (r != null) 'tx_dbm': r.dbm,
      if (r != null) 'sf': r.sf,
      if (r != null) 'bw_khz': r.bwKhz,
      if (peerRadio != null) 'peer_radio': peerRadio.toJson(),
      'phone': settings.phoneName,
      'app': appVersion,
      'protocol': protocol,
      'fw': fw,
      'clock': 'network (assumed: set the phone to automatic time)',
      'own': ownId,
      'peer': s.peer,
    });
    return true;
  }

  /// Does this test send messages, over LoRa, over the relay? (rundown-l
  /// and the point test run on LoRa only, whatever the Paths chip says.)
  static bool testSends(TestMeta m) => m.test != 'rundown-a';
  static bool _loraOnly(TestMeta m) => m.test == 'rundown-l' || m.test == 'point';
  static bool testUsesLora(TestMeta m) => testSends(m) && (_loraOnly(m) || m.paths != 'relay');
  static bool testUsesRelay(TestMeta m) => testSends(m) && !_loraOnly(m) && m.paths != 'lora';

  /// The first reason this test can't start now, or null.
  String? _blocking(TestMeta meta) {
    if (session != null) return 'A session is already running.';
    if (bench?.active == true) return 'The meter session is running.';
    if (!ble.ready) return 'The board is not connected over Bluetooth.';
    if (ownId.isEmpty) return "The board hasn't said who it is yet (wait a moment).";
    if (partner == '-' || link != 'connected') {
      return 'The board must be linked to the other board first (S on the device), and the link up.';
    }
    if (!relay.connected) return "The internet relay is not connected: the base phone can't be told.";
    if (meta.test == 'point') {
      if (meta.label.trim().isEmpty) return 'Type a label for this spot first (e.g. "pillar A, behind, 12 m").';
      if (meta.steps.isEmpty) return 'The sweep has no steps: add one.';
      if (protocol > 0 && protocol < 3) {
        return "This board's firmware ($fw) is older than v0.6.1: it doesn't know the point test.";
      }
    }
    if (testUsesLora(meta) && lora.isNotEmpty && lora != 'connected' && lora != 'checking') {
      return 'LoRa to $partner is ${pathStateText(lora, partner)}, and this test needs LoRa (the board would '
          'refuse it). Are both boards on the same range level (G)? Closer together?';
    }
    return null;
  }

  /// What speaks for and against starting this test now (TEST_PLAN section
  /// 7, as far as the app can see), for the confirmation before Start:
  /// blocking problems, then warnings. Asks the partner's phone for its
  /// board's range level first unless it said so in the last 10 minutes
  /// (waits up to 3 s).
  Future<List<StartCheck>> startChecks(TestMeta meta) async {
    final out = <StartCheck>[];
    final block = _blocking(meta);
    if (block != null) {
      out.add(StartCheck(true, block));
      return out;
    }
    final lo = testUsesLora(meta);
    if (lo) await _freshPartnerRadio();
    final mine = radio;
    final theirs = partnerRadio;
    if (lo && mine != null) {
      if (theirs != null && theirs.sf != mine.sf) {
        out.add(StartCheck(false, 'The range levels differ: this board is on ${mine.level} (SF${mine.sf}), '
            '$partner on ${theirs.level} (SF${theirs.sf}). LoRa only works with both on the same level: G on the '
            'devices.'));
      } else if (theirs == null) {
        out.add(StartCheck(false, "$partner's phone didn't say which range level its board is on. Check that "
            'both boards are on ${mine.level} (G alone plays the level).'));
      }
    }
    // The base phone looks offline (the A night, Dynamic 3 and the SF12 run,
    // 16-18 Oct): it hasn't said anything for 10 minutes, and this phone's
    // internet reconnected since. Its board would not start.
    final heard = partnerRadioAt;
    final up = mqttConnectedAt;
    final quiet = heard == null || DateTime.now().difference(heard).inMinutes >= 10;
    if (quiet && up != null && (heard == null || up.isAfter(heard))) {
      out.add(StartCheck(
          false,
          "$partner's phone hasn't answered for over 10 minutes, and this phone's internet reconnected since: it "
          "is probably offline, so its board would NOT start. Put the base phone on Wi-Fi and its charger, open the "
          'app there, and check its Tests page says "connected" before you start.',
          severe: true));
    }
    if (testUsesRelay(meta) && relayPath.isNotEmpty && relayPath != 'connected' && relayPath != 'checking') {
      out.add(StartCheck(false, 'The relay to $partner is ${pathStateText(relayPath, partner)}: the relay copies '
          'will fail.'));
    }
    if (!silent) {
      out.add(const StartCheck(false, 'Silent mode is off on this board (Z on the device); the plan has it on.'));
    }
    final b = int.tryParse(battery);
    if (b != null && b < 30 && !meta.test.startsWith('rundown')) {
      out.add(StartCheck(false, 'The board battery is at $b %.'));
    }
    if (protocol > 0 && protocol < 2) {
      out.add(StartCheck(false, 'The board runs older firmware ($fw): no path states or range level here.'));
    } else if (protocol > 0 && protocol < appProtocol) {
      out.add(StartCheck(false, 'The board runs older firmware ($fw) than this app is made for (v0.6.2): '
          'the tests work, but update it when you can.'));
    }
    if (meta.test == 'rundown-a' || meta.test == 'rundown-l') {
      out.add(const StartCheck(false, "This test switches both boards' Bluetooth off: the phones lose them until "
          'the run ends (that is expected). Afterwards: test dump on the PC.'));
    }
    if (meta.test == 'point' && mine != null && meta.steps.any((p) => p.sf != mine.sf || p.dbm != mine.dbm)) {
      out.add(StartCheck(false, 'Both boards switch to each step\'s setting by themselves and go back to their '
          'own (${mine.text}) at the end: nothing to set by hand.'));
    }
    return out;
  }

  /// The TEST START line for this session (both roles).
  static String _startLine(TestSession s, String role) {
    final plan = s.isPoint ? ' plan=${planText(s.plan)}' : '';
    return 'TEST START test=${s.test} paths=${s.paths} role=$role session=${s.id} seed=${s.seed}$plan';
  }

  /// Rover: 20 s after Start, has the base phone answered? If not (or it
  /// said its board isn't running), a red notice: the field problem of 16-18
  /// Oct, when the base phone's internet was down and nobody knew.
  void _armBaseCheck(TestSession s) {
    _baseCheck?.cancel();
    baseAlarm = false;
    _baseCheck = Timer(const Duration(seconds: 20), () {
      if (!identical(session, s)) return;
      if (baseState != 'starting' && baseState != 'running') {
        baseAlarm = true;
        notice = baseState.isEmpty
            ? "The base phone hasn't answered: its board is NOT running the test. Check its internet, or stop and "
                'start again.'
            : 'The base phone says "$baseStatus": its board is NOT running the test. Stop, fix it, start again.';
        notifyListeners();
      }
    });
  }

  /// The rover's Start button (after the confirmation). Null when started,
  /// otherwise why not.
  Future<String?> startRover(TestMeta meta) async {
    final block = _blocking(meta);
    if (block != null) return block;
    if (_loraOnly(meta)) meta.paths = 'lora'; // (the board runs these LoRa only: the logs say so too)
    meta.radio = radio;
    meta.partnerRadio = partnerRadio;
    final now = DateTime.now();
    final id = sessionIdFor(now);
    final seed = int.parse(id.replaceAll('-', '')) % 4294967296;
    final point = meta.test == 'point';
    final s = TestSession(id, 'rover', meta.test, meta.paths, seed, partner,
        plan: point ? List.of(meta.steps) : const [], label: point ? meta.label.trim() : '');
    notice = '';
    if (!_openSession(s, meta.toJson(), peerRadio: partnerRadio)) return "The log file can't be written.";
    session = s;
    lastSessionFile = s.file;
    lastMeta = meta.copy();
    area = '';
    situation = '';
    baseStatus = 'waiting for the base phone';
    baseState = '';
    baseLat = null;
    baseLon = null;
    _pendingStop = null; // (an old stop still waiting is moot now)
    _stopResend?.cancel();
    ble.sendLine(_startLine(s, 'rover'));
    final r = radio;
    _publishCtl(partner, {
      'cmd': 'start',
      'session': s.id,
      'test': s.test,
      'paths': s.paths,
      'seed': s.seed,
      'from': ownId,
      'meta': meta.toJson(),
      if (point) 'plan': planText(s.plan),
      if (point) 'label': s.label,
      if (r != null) 'radio': r.toJson(), // (the base checks its board is on the same level)
    });
    _armBaseCheck(s);
    await _startGps();
    notifyListeners();
    return null;
  }

  /// Point test: a new label typed during the session (the spot's
  /// description corrected): a label record in the file.
  void setPointLabel(String label) {
    final s = session;
    if (s == null || !s.isPoint || label.trim().isEmpty) return;
    s.label = label.trim();
    _log({'kind': 'label', 'label': s.label}, daily: false);
    notifyListeners();
  }

  /// A start from the rover phone, over MQTT: this phone becomes the base.
  Future<void> _startBase(Map<String, dynamic> m) async {
    final id = '${m['session'] ?? ''}';
    final from = '${m['from'] ?? ''}';
    final cur = session;
    if (cur != null) {
      if (cur.id == id) {
        _publishCtl(from, {'cmd': 'status', 'session': id, 'state': cur.board, 'from': ownId});
      } else {
        _publishCtl(from, {'cmd': 'status', 'session': id, 'state': 'busy', 'detail': 'another session runs', 'from': ownId});
      }
      return;
    }
    final seedV = m['seed'];
    final seed = seedV is int ? seedV : int.tryParse('$seedV') ?? 0;
    final test = '${m['test'] ?? 'dynamic'}';
    final plan = test == 'point' ? parsePlan('${m['plan'] ?? ''}') ?? defaultPlan : const <PlanStep>[];
    final s = TestSession(id, 'base', test, '${m['paths'] ?? 'both'}', seed, from,
        plan: plan, label: '${m['label'] ?? ''}');
    final meta = m['meta'] is Map ? Map<String, Object?>.from(m['meta'] as Map) : <String, Object?>{};
    meta.remove('peer_radio'); // (the rover's view of this board: this board's own is written instead)
    final roverRadio = RadioInfo.fromJson(m['radio']);
    final mine = radio;
    // A LoRa test with the boards on different range levels: started all the
    // same (the rover was warned), but both phones say so.
    var warn = '';
    if (roverRadio != null && mine != null && roverRadio.sf != mine.sf && s.paths != 'relay' && s.test != 'rundown-a') {
      warn = 'range levels differ: this board on ${mine.level} (SF${mine.sf}), the rover on ${roverRadio.level} '
          '(SF${roverRadio.sf})';
    }
    notice = warn.isEmpty ? '' : 'Warning: $warn';
    if (!_openSession(s, meta, peerRadio: roverRadio)) {
      _publishCtl(from, {'cmd': 'status', 'session': id, 'state': 'refused', 'detail': 'no log file', 'from': ownId});
      return;
    }
    session = s;
    lastSessionFile = s.file;
    if (!ble.ready) {
      s.board = 'board not connected';
      _publishCtl(from, {'cmd': 'status', 'session': id, 'state': 'no board', 'from': ownId});
    } else if (s.isPoint && protocol > 0 && protocol < 3) {
      s.board = 'refused: older firmware';
      notice = "This board's firmware ($fw) is older than v0.6.1: it doesn't know the point test.";
      _publishCtl(from, {'cmd': 'status', 'session': id, 'state': 'refused', 'detail': 'base firmware older than v0.6.1', 'from': ownId});
    } else {
      ble.sendLine(_startLine(s, 'base'));
      _publishCtl(from, {'cmd': 'status', 'session': id, 'state': 'starting', if (warn.isNotEmpty) 'detail': warn, 'from': ownId});
    }
    await _startGps();
    _posTimer?.cancel();
    _posTimer = Timer.periodic(const Duration(seconds: 10), (_) => _publishPos());
    notifyListeners();
  }

  /// Base: our position, for the rover's distance display.
  void _publishPos() {
    final s = session;
    final f = fix;
    if (s == null || s.role != 'base' || f == null) return;
    _publishCtl(s.peer, {'cmd': 'pos', 'session': s.id, 'lat': f.latitude, 'lon': f.longitude, 'acc': f.accuracy, 'from': ownId});
  }

  void _onCtl(Map<String, dynamic> m) {
    final op = '${m['cmd'] ?? ''}';
    final s = session;
    switch (op) {
      case 'start':
        _startBase(m);
        break;
      case 'stop':
        // The rover resends a stop until we confirm it: stop our board once;
        // a stop for a session we no longer run (or never ran) is answered
        // "stopped", so the rover stops asking.
        final sid = '${m['session'] ?? ''}';
        if (s != null && s.role == 'base' && s.id == sid) {
          if (!s.stopping) stopSession();
        } else {
          _publishCtl('${m['from'] ?? ''}', {'cmd': 'status', 'session': sid, 'state': 'stopped', 'detail': 'not running', 'from': ownId});
        }
        break;
      case 'status':
        final st = '${m['state'] ?? ''}';
        final p = _pendingStop;
        if (p != null && p.session == '${m['session'] ?? ''}' && st != 'starting' && st != 'running') {
          _pendingStop = null; // the base confirmed: its board isn't running the test any more
          _stopResend?.cancel();
        }
        if (s != null && s.role == 'rover') {
          baseState = st;
          baseStatus = '$st${m['detail'] != null ? ': ${m['detail']}' : ''}';
          if (st == 'running' || st == 'starting') {
            baseAlarm = false;
          } else if (st == 'refused' || st == 'no board' || st == 'busy') {
            baseAlarm = true; // it answered, but its board isn't running the test
            notice = 'The base phone says "$baseStatus": its board is NOT running the test.';
          }
        }
        break;
      case 'pt':
        // The base phone, during a point test: how its board hears ours.
        if (s != null && s.role == 'rover' && s.id == '${m['session'] ?? ''}') {
          final r = m['rssi'];
          final q = m['snr'];
          if (r is num) s.theirRssi = r;
          if (q is num) s.theirSnr = q;
          s.theirAt = DateTime.now();
        }
        break;
      case 'pos':
        final la = m['lat'];
        final lo = m['lon'];
        if (la is num && lo is num) {
          baseLat = la.toDouble();
          baseLon = lo.toDouble();
          basePosAt = DateTime.now();
        }
        break;
      case 'radio':
        // The partner's phone: its board's setting (none if its board isn't connected).
        if ('${m['from'] ?? ''}' == partner) {
          partnerRadio = RadioInfo.fromJson(m);
          partnerRadioAt = DateTime.now();
          final w = _radioWait;
          _radioWait = null;
          if (w != null && !w.isCompleted) w.complete();
        }
        break;
      case 'radio?':
        _publishRadio('${m['from'] ?? ''}');
        break;
    }
  }

  /// Stop: the board stops (its STOP line closes the file); the rover also
  /// tells the base. A second tap while stopping closes the file at once.
  Future<void> stopSession() async {
    final s = session;
    if (s == null) return;
    if (s.stopping) {
      await _finishSession();
      return;
    }
    s.stopping = true;
    s.board = 'stopping';
    ble.sendLine('TEST STOP');
    if (s.role == 'rover') _stopBase(s);
    _stopTimer?.cancel();
    _stopTimer = Timer(const Duration(seconds: 45), () => _finishSession());
    notifyListeners();
  }

  /// Rover: tell the base phone to stop, and keep telling it every 5 s until
  /// it confirms (or 2 minutes pass): on 18 Oct the rover's Stop was dropped
  /// while it was offline, and the base ran on for 21 minutes.
  void _stopBase(TestSession s) {
    _pendingStop = _PendingStop(s.id, s.peer);
    _sendPendingStop();
    _stopResend?.cancel();
    _stopResend = Timer.periodic(const Duration(seconds: 5), (_) => _sendPendingStop());
  }

  /// Send the stop still waiting for its confirmation (when the internet is
  /// up; otherwise the next try, or the reconnect, sends it).
  void _sendPendingStop() {
    final p = _pendingStop;
    if (p == null) {
      _stopResend?.cancel();
      return;
    }
    if (DateTime.now().difference(p.since).inSeconds > 120) {
      _pendingStop = null;
      _stopResend?.cancel();
      notice = "The base phone never confirmed the stop: its board may still be running the test. Stop it there "
          '(Stop on its phone, or a 5 s hold on the board).';
      notifyListeners();
      return;
    }
    if (relay.connected) _publishCtl(p.peer, {'cmd': 'stop', 'session': p.session, 'from': ownId});
  }

  Future<void> _finishSession() async {
    final s = session;
    if (s == null) return;
    _stopTimer?.cancel();
    _posTimer?.cancel();
    _baseCheck?.cancel();
    _log({
      'kind': 'session_end',
      'session': s.id,
      'gen': s.gen,
      'lora': {'sent': s.lora.sent, 'ack': s.lora.ack, 'fail': s.lora.fail, 'skip': s.lora.skip},
      'relay': {'sent': s.relay.sent, 'ack': s.relay.ack, 'fail': s.relay.fail, 'skip': s.relay.skip},
      'link_lost': s.lost,
      if (s.isPoint) 'label': s.label,
      if (s.isPoint)
        'steps': [
          for (var i = 0; i < s.plan.length; i++)
            {
              'step': i + 1,
              'sf': s.plan[i].sf,
              'dbm': s.plan[i].dbm,
              'sent': s.stepCounts[i].sent,
              'ack': s.stepCounts[i].ack,
              'fail': s.stepCounts[i].fail,
              'skip': s.stepCounts[i].skip,
            }
        ],
    }, daily: false);
    _sess.close();
    session = null;
    await _stopGps();
    notifyListeners();
  }

  /// A test-mode line from the board: into the logs, and the live counters.
  void _onTestLine(String line) {
    final t = TestLine.parse(line);
    if (t == null) return;
    _log({'kind': 'board', 'n': t.n, 'ms': t.ms, 'ev': t.ev, ...t.fields});
    final s = session;
    if (s == null) return;
    switch (t.ev) {
      case 'START':
        s.board = 'running';
        if (s.role == 'base') _publishCtl(s.peer, {'cmd': 'status', 'session': s.id, 'state': 'running', 'from': ownId});
        break;
      case 'REFUSED':
        s.board = 'refused: ${t.str('reason')}';
        notice = 'The board refused the test: ${refusalText(t.str('reason'))}';
        if (s.role == 'base') {
          _publishCtl(s.peer, {'cmd': 'status', 'session': s.id, 'state': 'refused', 'detail': t.str('reason'), 'from': ownId});
        }
        break;
      case 'GEN':
        s.gen++;
        final st = t.integer('step'); // (a point test's message: its step)
        final sc = st != null && st >= 1 && st <= s.stepCounts.length ? s.stepCounts[st - 1] : null;
        final seq = t.integer('seq');
        if (st != null && seq != null) s.seqStep[seq] = st;
        if (t.fields.containsKey('lora_skip')) {
          s.lora.skip++;
          sc?.skip++;
        } else if (t.str('lora') == 'queued') {
          s.lora.sent++;
          sc?.sent++;
        }
        if (t.fields.containsKey('relay_skip')) {
          s.relay.skip++;
        } else if (t.fields.containsKey('relay_id')) {
          s.relay.sent++;
        }
        break;
      case 'ACK':
        (t.str('path') == 'R' ? s.relay : s.lora).ack++;
        _stepOf(s, t)?.ack++;
        break;
      case 'FAIL':
        (t.str('path') == 'R' ? s.relay : s.lora).fail++;
        _stepOf(s, t)?.fail++;
        break;
      case 'RX':
        s.rssi = t.number('rssi');
        s.snr = t.number('snr');
        // Base, point test: tell the rover how we hear it (every 5 s at
        // most), so its page shows both directions.
        if (s.isPoint && s.role == 'base' && t.str('type') == 'MSG' && t.fields.containsKey('seq')) {
          final last = s.lastPtSent;
          if (last == null || DateTime.now().difference(last).inSeconds >= 5) {
            s.lastPtSent = DateTime.now();
            _publishCtl(s.peer, {'cmd': 'pt', 'session': s.id, 'step': s.step, 'rssi': s.rssi, 'snr': s.snr, 'from': ownId});
          }
        }
        break;
      case 'STEP':
        // STEP i=<n>,sf=..,dbm=..,n=..,step_ms=.. (point test; i=0 ... restored=1 at the end)
        s.step = t.integer('i') ?? 0;
        s.stepSf = t.integer('sf') ?? 0;
        s.stepDbm = t.integer('dbm') ?? 0;
        s.stepMs = t.integer('step_ms') ?? 0;
        s.stepAt = DateTime.now();
        break;
      case 'STAT':
        final b = t.integer('bat_pct');
        if (b != null && b >= 0) s.batPct = b;
        break;
      case 'LINK':
        // LINK state=lost,why=... / restored / ended (losses: firmware v0.6.0)
        if (t.str('state') == 'lost') {
          s.lost++;
          s.lastLostWhy = t.str('why');
          s.lastLostAt = DateTime.now();
          s.linkUp = false;
        } else if (t.str('state') == 'restored') {
          s.linkUp = true;
        }
        break;
      case 'PATH':
        // PATH path=L|R,state=...: a path changed during the test
        if (t.str('path') == 'R') {
          s.pathR = t.str('state');
        } else {
          s.pathL = t.str('state');
        }
        break;
      case 'STOP':
        final reason = t.str('reason');
        s.board = 'stopped ($reason)';
        if (s.role == 'base') {
          _publishCtl(s.peer, {'cmd': 'status', 'session': s.id, 'state': 'stopped', 'detail': reason, 'from': ownId});
        } else if (!s.stopping && reason != 'done') {
          // It ended by itself here (a 5 s hold): stop the base too (resent
          // until it confirms). Not after a baseline's or point test's
          // "done": the base finishes its own messages (it started half a
          // grid later), and a stop would cut its last ones.
          _stopBase(s);
        }
        _finishSession();
        break;
    }
  }

  /// A point test's ACK / FAIL: the counters of the step its message
  /// belonged to (from its GEN line), or null.
  PathCounts? _stepOf(TestSession s, TestLine t) {
    if (!s.isPoint || t.str('path') == 'R') return null;
    final seq = t.integer('seq');
    final st = seq == null ? null : s.seqStep[seq];
    return st != null && st >= 1 && st <= s.stepCounts.length ? s.stepCounts[st - 1] : null;
  }

  // ------------------------------------------------------------------------
  // The meter session (bench power, firmware v0.6.1; TEST_PLAN section 17)
  // ------------------------------------------------------------------------

  /// The first reason the meter session can't start now, or null.
  String? benchBlocking() {
    if (session != null) return 'A test is running.';
    if (bench?.active == true) return 'The meter session is already running.';
    if (!ble.ready || !accepted) return 'The board is not connected over Bluetooth.';
    if (protocol > 0 && protocol < 3) return "This board's firmware ($fw) is older than v0.6.1: no meter session.";
    return null;
  }

  /// Start the meter session: the board steps through its states by itself
  /// (BN lines tell this phone; between them the timetable does).
  String? startBench() {
    final block = benchBlocking();
    if (block != null) return block;
    bench = BenchRun();
    ble.sendLine('BENCH POWER');
    _log({'kind': 'bench', 'event': 'asked'});
    _benchTimer?.cancel();
    _benchTimer = Timer.periodic(const Duration(seconds: 1), (_) => _benchTick());
    notifyListeners();
    return null;
  }

  /// Stop it early (only reaches the board while it is connected: from step
  /// 3b on; before that a 5 s hold on the board stops it).
  void stopBench() {
    final b = bench;
    if (b == null) return;
    if (ble.ready) ble.sendLine('BENCH STOP');
    if (!ble.ready) {
      b.state = 'ended';
      b.detail = 'stopped here (the board may still run: a 5 s hold stops it)';
    }
    notifyListeners();
  }

  /// Forget a finished meter session (its page shows the start again).
  void clearBench() {
    if (bench?.active == true) return;
    bench = null;
    _benchTimer?.cancel();
    _benchTimer = null;
    notifyListeners();
  }

  /// A BN line from the board (protocol 3).
  void _onBench(List<String> f) {
    final what = f.isEmpty ? '' : f[0];
    final detail = f.length > 1 ? f[1] : '';
    final b = bench ?? BenchRun(); // (a session started from the console shows here too)
    bench = b;
    _log({'kind': 'bench', 'event': what, if (detail.isNotEmpty) 'detail': detail});
    switch (what) {
      case 'refused':
        b.state = 'refused';
        b.detail = detail;
        break;
      case 'wait-usb':
        b.state = 'wait-usb';
        break;
      case 'start':
        final d = detail.split(' ');
        b.state = 'running';
        b.startedAt = DateTime.now();
        b.totalS = d.isNotEmpty ? int.tryParse(d[0]) ?? 0 : 0;
        b.maxDbm = d.length > 1 ? int.tryParse(d[1]) ?? 22 : 22;
        b.table = benchTimetable(b.maxDbm);
        if (b.totalS > 0 && b.totalS != benchTotalMs(b.maxDbm) ~/ 1000) {
          b.detail = "the board's step list differs from this app's (${b.totalS} s, here "
              "${benchTotalMs(b.maxDbm) ~/ 1000} s): update the app or the board; the board's BN lines still count";
        }
        break;
      case 'step':
        b.state = 'running';
        b.sync(detail.split(' ').first);
        break;
      case 'end':
        b.state = 'ended';
        b.detail = detail;
        break;
    }
    _benchTimer ??= Timer.periodic(const Duration(seconds: 1), (_) => _benchTick());
  }

  /// Once a second during the meter session: redraw, and from step 3b on,
  /// while the board is connectable, reconnect at once instead of after
  /// Bluetooth's back-off (every 3 s at most).
  void _benchTick() {
    final b = bench;
    if (b == null || !b.active) {
      _benchTimer?.cancel();
      _benchTimer = null;
      notifyListeners();
      return;
    }
    final now = b.now();
    final k = now.slot?.step.kind;
    final connectable = k != null && k.index >= BenchKind.bleConn.index && k != BenchKind.off;
    if (connectable && !now.signal && !ble.ready) {
      final last = _lastKick;
      if (last == null || DateTime.now().difference(last).inSeconds >= 3) {
        _lastKick = DateTime.now();
        ble.kick();
      }
    }
    if (k == BenchKind.off && !now.signal && !ble.ready) {
      b.state = 'ended';
      b.detail = 'off';
    }
    notifyListeners();
  }

  /// The label buttons (TEST_PLAN section 2.4).
  void setArea(String a) {
    area = a;
    _log({'kind': 'label', 'area': area, 'sit': situation, 'note': ''}, daily: false);
    notifyListeners();
  }

  void setSituation(String v) {
    situation = v;
    _log({'kind': 'label', 'area': area, 'sit': situation, 'note': ''}, daily: false);
    notifyListeners();
  }

  void addNote(String note) {
    if (note.trim().isEmpty) return;
    _log({'kind': 'label', 'area': area, 'sit': situation, 'note': note.trim()}, daily: false);
    notifyListeners();
  }
}
