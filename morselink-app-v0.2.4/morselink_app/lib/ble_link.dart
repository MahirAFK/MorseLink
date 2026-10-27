// MorseLink - the Bluetooth LE link to the device (firmware main/ble.c).
//
// The device offers a Nordic UART Service: the phone writes lines to RX and
// gets lines as notifications from TX. This class keeps one device
// connected for as long as the app wants it: when the link drops it tries
// again after a few seconds, so after a restart of the board (or walking
// out of range and back) everything picks up by itself.
//
// Pairing: the first connection only works while the device's O window is
// open (command mode, O). The device asks for security as soon as the phone
// connects, Android shows its "Pair with MorseLink-...?" dialog, and after
// that the bond is reused silently.
//
// Since v0.2.0, for Samsung phones (the S10e of 15 Oct kept dropping, the
// board logging "reason 0x213": the phone had hung up):
//   - while Android is pairing, the app waits for the pairing to finish
//     before it looks for the UART service (doing both at once can fail on
//     Samsung, and the retry then hung up on the pairing);
//   - the MTU is asked for once paired, not during the pairing;
//   - failed attempts back off (4, 8, 16, then 30 s), so Android's stack
//     gets time to recover from its error 133;
//   - every failure and drop says why (onState's `why`, logged by the app),
//     and a board that keeps hanging up right after connecting gets the
//     hint to pair again.
//
// v0.2.1: `slow` (the board switched its Bluetooth off with W) waits 30 s
// between attempts from the start, and kick() tries at once (the meter
// session's step 3b, when the board becomes connectable again).

import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

final Guid nusService = Guid('6E400001-B5A3-F393-E0A9-E50E24DCCA9E');
final Guid nusRx = Guid('6E400002-B5A3-F393-E0A9-E50E24DCCA9E'); // the phone writes here
final Guid nusTx = Guid('6E400003-B5A3-F393-E0A9-E50E24DCCA9E'); // the device notifies here

/// The MTU asked for once paired: a whole line (240 characters + '\n') fits
/// one notification. The board's NimBLE accepts up to 256.
const int _wantedMtu = 247;

/// Bluetooth's state right now (at start-up it may take a moment to be known).
Future<BluetoothAdapterState> adapterState() async {
  try {
    return await FlutterBluePlus.adapterState
        .firstWhere((s) => s != BluetoothAdapterState.unknown)
        .timeout(const Duration(seconds: 3));
  } catch (_) {
    return BluetoothAdapterState.unknown;
  }
}

/// Words for an Android Bluetooth status or disconnect code (the GATT / HCI
/// numbers Android reports); "" for one without words here.
String bleCodeText(int? code) {
  switch (code) {
    case 5: // authentication failure
    case 6: // PIN or key missing
      return 'the pairing is broken on one side';
    case 8: // link supervision timeout
      return 'the board went out of reach (or was switched off)';
    case 19: // remote user terminated
      return 'the board ended the connection';
    case 22: // local host terminated
      return 'this phone ended the connection';
    case 34: // LMP response timeout
      return 'the board stopped answering';
    case 61: // MIC failure
      return 'encryption failed: the pairing is broken on one side';
    case 62: // failed to be established
      return 'the connection could not be set up';
    case 133: // GATT_ERROR, Android's catch-all (common on Samsung)
      return "Android's Bluetooth error 133";
    default:
      return '';
  }
}

/// A failure with its own words (thrown inside an attempt).
class _BleProblem implements Exception {
  final String text;

  _BleProblem(this.text);

  @override
  String toString() => text;
}

class BleLink {
  /// A whole line from the device (without the '\n').
  final void Function(String line) onLine;

  /// The link changed: state is "connecting", "pairing", "connected",
  /// "not connected", "disconnected", "bluetooth off" or "idle"; mtu the
  /// negotiated size; why says what went wrong (or what to do), "" if
  /// nothing did.
  final void Function(String state, int mtu, String why) onState;

  BleLink({required this.onLine, required this.onState});

  BluetoothDevice? device;
  BluetoothCharacteristic? _rx;
  StreamSubscription<BluetoothConnectionState>? _connSub;
  StreamSubscription<List<int>>? _valSub;
  Timer? _retry;
  bool _want = false; // the app wants this device connected
  bool _busy = false; // an attempt is under way (it handles its own failures)
  final List<int> _buf = []; // bytes of the line being received
  Future<void> _writes = Future.value(); // writes go out one at a time, in order
  String state = 'idle';
  String why = '';
  int mtu = 23;
  int _failures = 0; // attempts in a row that failed (the back-off)
  int _quickDrops = 0; // connections in a row the board ended within seconds
  DateTime? _upSince; // when the current connection became usable

  /// The board said its Bluetooth is off on purpose (W): no point trying
  /// every few seconds. Cleared by the app when the board is back.
  bool slow = false;

  bool get ready => _rx != null;

  /// Use the device with this Bluetooth address (as remembered from a scan).
  void use(String remoteId) {
    if (device?.remoteId.str == remoteId) return;
    _connSub?.cancel();
    _connSub = null;
    _failures = 0;
    _quickDrops = 0;
    device = BluetoothDevice.fromId(remoteId);
  }

  /// Keep the device connected from now on.
  void start() {
    _want = true;
    _attempt();
  }

  /// Stop trying, and disconnect.
  Future<void> stop() async {
    _want = false;
    _retry?.cancel();
    _rx = null;
    try {
      await device?.disconnect();
    } catch (_) {}
    _set('idle');
  }

  void _set(String s, [String w = '']) {
    state = s;
    why = w;
    onState(s, mtu, w);
  }

  void _schedule(int seconds) {
    _retry?.cancel();
    if (_want) _retry = Timer(Duration(seconds: seconds), _attempt);
  }

  /// The wait before the next attempt: 4 s, then longer while attempts keep
  /// failing (8, 16, at most 30 s), so a confused Bluetooth stack gets time
  /// to recover and a board that is off isn't hammered.
  int get _backoff => slow
      ? 30
      : _failures <= 1
          ? 4
          : _failures == 2
              ? 8
              : _failures == 3
                  ? 16
                  : 30;

  /// Try now instead of after the back-off (the meter session's step 3b:
  /// the board has just become connectable). Nothing if connected, or an
  /// attempt is under way.
  void kick() {
    if (!_want || _busy || _rx != null) return;
    _retry?.cancel();
    _failures = 0;
    _attempt();
  }

  /// One connection attempt: connect, wait for any pairing to finish, find
  /// the UART service, a larger MTU, listen to TX, say HI. On any failure:
  /// say why, disconnect (which releases Android's GATT client: without it,
  /// later attempts fail with error 133), and try again after the back-off.
  Future<void> _attempt() async {
    final d = device;
    if (!_want || d == null || _busy || _rx != null) return;
    _busy = true;
    var step = 'connecting';
    try {
      if (await adapterState() != BluetoothAdapterState.on) {
        _set('bluetooth off', 'Bluetooth is off on this phone');
        _schedule(5);
        return;
      }
      _set('connecting', _quickDrops >= 2 ? why : '');
      _connSub ??= d.connectionState.listen((s) => _onConnState(d, s));
      // mtu: null: no automatic MTU request while the board is asking for
      // encryption (on Samsung phones the two together can fail).
      await d.connect(timeout: const Duration(seconds: 20), autoConnect: false, mtu: null);
      step = 'pairing';
      await _waitForPairing(d);
      step = 'finding the UART service';
      final services = await d.discoverServices();
      BluetoothCharacteristic? rx;
      BluetoothCharacteristic? tx;
      for (final s in services) {
        if (s.uuid != nusService) continue;
        for (final c in s.characteristics) {
          if (c.uuid == nusRx) rx = c;
          if (c.uuid == nusTx) tx = c;
        }
      }
      if (rx == null || tx == null) {
        throw _BleProblem('this is not a MorseLink (no UART service)');
      }
      if (Platform.isAndroid) {
        try {
          await d.requestMtu(_wantedMtu);
        } catch (_) {} // (23 works too: the lines just come in more pieces)
      }
      mtu = d.mtuNow;
      step = 'subscribing';
      await _valSub?.cancel();
      _valSub = tx.onValueReceived.listen(_onBytes);
      await tx.setNotifyValue(true);
      _buf.clear();
      _rx = rx;
      _failures = 0;
      _upSince = DateTime.now();
      _set('connected');
      sendLine('HI'); // the device answers HI, ST and DR (it also sends them by itself once it accepts us)
    } catch (e) {
      _rx = null;
      _failures++;
      // Connected, then dropped while setting up: the board hung up on us
      // (it does when it doesn't accept this phone). Not even connected: the
      // board is off or away, which says nothing about the pairing.
      if (step == 'connecting') {
        _quickDrops = 0;
      } else if (!d.isConnected) {
        _quickDrops++;
      }
      _set('not connected', _quickDrops >= 2 ? _pairAgainHint(d) : _failureText(step, e));
      try {
        await d.disconnect();
      } catch (_) {}
      _schedule(_backoff);
    } finally {
      _busy = false;
    }
  }

  /// Android: if a pairing is under way (the board asks for one as soon as
  /// the phone connects), wait for it to finish (at most 60 s, the board's O
  /// window) before going on. A pairing that fails or is declined is an
  /// error. Nothing to wait for if the phone is paired already.
  Future<void> _waitForPairing(BluetoothDevice d) async {
    if (!Platform.isAndroid) return;
    BluetoothBondState bs;
    try {
      bs = await d.bondState.first.timeout(const Duration(seconds: 3));
    } catch (_) {
      return; // (not known: go on as before v0.2.0)
    }
    if (bs == BluetoothBondState.bonded) return;
    if (bs == BluetoothBondState.none) {
      // Not paired: the board's security request starts the pairing in a
      // moment. If none starts, go on (the first write starts it).
      try {
        bs = await d.bondState.firstWhere((s) => s != BluetoothBondState.none).timeout(const Duration(seconds: 4));
      } catch (_) {
        return;
      }
    }
    if (bs != BluetoothBondState.bonding) return;
    _set('pairing', 'tap Pair when the phone asks');
    // The pairing ends, or the board hangs up first (it does when its O
    // window isn't open for a new phone).
    final ended = d.bondState.firstWhere((s) => s != BluetoothBondState.bonding);
    final dropped = d.connectionState.firstWhere((s) => s == BluetoothConnectionState.disconnected);
    Object first = BluetoothBondState.none;
    try {
      first = await Future.any<Object>([ended, dropped]).timeout(const Duration(seconds: 60));
    } on TimeoutException {
      throw _BleProblem("the pairing didn't finish within 60 s");
    }
    if (first is BluetoothConnectionState) {
      throw _BleProblem('the board hung up during the pairing: open its pairing window first (command mode, O)');
    }
    if (first != BluetoothBondState.bonded) {
      throw _BleProblem('the pairing failed or was declined (O on the board opens 60 s for it)');
    }
  }

  /// What went wrong, in words: the step, and the reason Android or the
  /// plugin gave.
  String _failureText(String step, Object e) {
    if (e is _BleProblem) return e.text;
    if (e is FlutterBluePlusException) {
      final words = e.platform == ErrorPlatform.android ? bleCodeText(e.code) : '';
      final desc = (e.description ?? '').trim();
      final detail = words.isNotEmpty ? words : (desc.isNotEmpty ? desc : 'error ${e.code}');
      if (step == 'connecting' && desc.toLowerCase().contains('timed out')) {
        return "the board didn't answer (off, out of reach, or connected to another phone)";
      }
      if (e.platform == ErrorPlatform.android && e.code == 133) {
        return "$step failed: Android's Bluetooth error 133 (it tries again; if it keeps on, "
            'switch Bluetooth off and on)';
      }
      return '$step failed: $detail';
    }
    if (e is TimeoutException) return '$step: no answer in time';
    final s = '$e';
    if (s.toLowerCase().contains('permission')) {
      return 'Bluetooth permission missing: allow "Nearby devices" (Settings > Apps > MorseLink > Permissions)';
    }
    return '$step failed: $s';
  }

  /// The connection went down. During an attempt the attempt handles it; a
  /// connection that was up gets its reason said, and a reconnect after
  /// 2 s (Samsung's stack needs a moment between the two). A board that
  /// keeps hanging up within seconds of connecting usually doesn't accept
  /// this phone any more: the hint says how to pair again.
  void _onConnState(BluetoothDevice d, BluetoothConnectionState s) {
    if (s != BluetoothConnectionState.disconnected || _busy) return;
    final was = _rx != null;
    _rx = null;
    _buf.clear();
    if (!was) return;
    final r = d.disconnectReason;
    final words = r != null && r.platform == ErrorPlatform.android ? bleCodeText(r.code) : '';
    final up = _upSince;
    final quick = up != null && DateTime.now().difference(up).inSeconds < 10;
    _quickDrops = quick ? _quickDrops + 1 : 0;
    _set('disconnected', _quickDrops >= 2 ? _pairAgainHint(d) : (words.isNotEmpty ? words : (r?.description ?? '')));
    _schedule(slow ? 30 : 2);
  }

  /// What to do when the board keeps hanging up right after connecting.
  String _pairAgainHint(BluetoothDevice d) {
    final name = d.platformName.isEmpty ? 'the MorseLink' : d.platformName;
    return 'the board keeps hanging up right after connecting: it may be paired with another phone, or this '
        "phone's pairing is stale. Forget $name in the phone's Bluetooth settings, then O on the board, "
        'and Find > Pair here';
  }

  /// Notifications arrive in pieces of at most MTU - 3 bytes: join them
  /// into lines at '\n'.
  void _onBytes(List<int> v) {
    for (final b in v) {
      if (b == 10) {
        if (_buf.isNotEmpty) {
          final line = latin1.decode(_buf).replaceAll('\r', '');
          _buf.clear();
          if (line.isNotEmpty) onLine(line);
        }
      } else if (_buf.length < 400) {
        _buf.add(b);
      }
    }
  }

  /// Send one line (the '\n' is added). Lines go out in order; one that
  /// can't be written (link gone) is dropped: the device catches up from its
  /// own state lines, and a relayed packet has its own retries.
  void sendLine(String line) {
    _writes = _writes.then((_) async {
      final rx = _rx;
      if (rx == null) return;
      final bytes = utf8.encode('$line\n');
      try {
        await rx.write(bytes, withoutResponse: false, allowLongWrite: bytes.length > mtu - 3);
      } catch (_) {}
    });
  }

  /// Forget the device: stop, and drop it.
  Future<void> forget() async {
    await stop();
    await _connSub?.cancel();
    _connSub = null;
    device = null;
  }
}
