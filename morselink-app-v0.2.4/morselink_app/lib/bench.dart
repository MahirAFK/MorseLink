// MorseLink - the meter session (firmware v0.6.1, main/bench.c: `bench
// power`): the steps the board goes through by itself while a multimeter in
// series with the battery shows its current, and the same timetable, so the
// app can say "now: step 3b, 32 s left" while the board's Bluetooth is off
// (steps 1 to 3a) and the board can't tell it.
//
// The list and the timings must match bench.c exactly: the firmware's host
// test and this app's test both check the total (543.8 s on an SX1262).
// When the board is connected it says each step as it starts (BN step), and
// the app follows that instead of its own clock.

/// What a step changes (bench_kind_t).
enum BenchKind { cpu, keying, listen, bleAdv, bleConn, radioStandby, listenSf12, carrier, motor, off }

/// One step of the meter session.
class BenchStep {
  final String id; // "1", "1b", "3a", "4b"...: the sheet's row
  final int step; // the number the board buzzes before it (1-6)
  final BenchKind kind;
  final int dbm; // carrier steps: the power
  final int secs; // how long it lasts (0: until the key wakes the board)
  final String what; // what the board does
  final String todo; // what the owner does meanwhile ('' = just read the meter)

  const BenchStep(this.id, this.step, this.kind, this.secs, this.what, {this.dbm = 0, this.todo = ''});
}

/// The steps, as bench.c's BENCH_POWER (agreed 20 Oct).
const List<BenchStep> benchSteps = [
  BenchStep('1', 1, BenchKind.cpu, 60, 'processor only (radio standby, Bluetooth off)'),
  BenchStep('1b', 1, BenchKind.keying, 45, '+ keying', todo: 'key PARIS over and over at your usual speed'),
  BenchStep('1c', 1, BenchKind.keying, 15, '+ the key held down',
      todo: 'hold the key about 3 s, three times (never 5 s: that stops the session)'),
  BenchStep('2', 2, BenchKind.listen, 60, '+ LoRa listening at SF9'),
  BenchStep('3a', 3, BenchKind.bleAdv, 60, '+ Bluetooth advertising, not connectable'),
  BenchStep('3b', 3, BenchKind.bleConn, 60, 'Bluetooth connectable: this phone reconnects',
      todo: 'keep this app open next to the board (read in the second half: it takes a few seconds to connect)'),
  BenchStep('3c', 3, BenchKind.radioStandby, 60, 'LoRa to standby, Bluetooth still connected'),
  BenchStep('3d', 3, BenchKind.listenSf12, 60, 'LoRa listening at SF12, Bluetooth connected'),
  BenchStep('4a', 4, BenchKind.carrier, 10, 'back to SF9; transmitting a carrier, 17 dBm', dbm: 17),
  BenchStep('4b', 4, BenchKind.carrier, 10, 'transmitting a carrier, 20 dBm', dbm: 20),
  BenchStep('4c', 4, BenchKind.carrier, 10, 'transmitting a carrier, 22 dBm (SX1262 only)', dbm: 22),
  BenchStep('5a', 5, BenchKind.motor, 15, 'motor M1 100 %'),
  BenchStep('5b', 5, BenchKind.motor, 15, 'motor M1 50 %'),
  BenchStep('5c', 5, BenchKind.motor, 15, 'motor M2 100 %'),
  BenchStep('5d', 5, BenchKind.motor, 15, 'both motors 100 %'),
  BenchStep('6', 6, BenchKind.off, 0, 'switched off', todo: 'read the off current, then hold the key 2 s to switch on'),
];

// The signals, as bench.h: a new step number gets that many buzzes on M1
// (each on and off for 300 ms, the last followed by a 2 s pause), a part of
// the same step one 100 ms buzz and 1 s still.
const int benchSettleMs = 5000; // BENCH_SETTLE_S: after "BN start", before step 1's signal
const int _blinkMs = 300;
const int _pauseMs = 2000;
const int _subBlinkMs = 100;
const int _subPauseMs = 1000;

/// Does a board whose chip goes up to maxDbm skip this step (22 dBm on an
/// SX1276)?
bool benchSkips(BenchStep s, int maxDbm) => s.kind == BenchKind.carrier && s.dbm > maxDbm;

/// The signal before step s, ms (prevStep: the number of the step before;
/// bench_signal_ms).
int benchSignalMs(BenchStep s, int prevStep) =>
    s.step != prevStep ? 2 * _blinkMs * s.step + _pauseMs - _blinkMs : _subBlinkMs + _subPauseMs;

/// One step in the timetable: when its signal starts, when the step itself
/// starts and ends, ms after "BN start".
class BenchSlot {
  final BenchStep step;
  final int signalMs;
  final int startMs;
  final int endMs;

  const BenchSlot(this.step, this.signalMs, this.startMs, this.endMs);
}

/// The whole timetable for a board whose chip goes up to maxDbm.
List<BenchSlot> benchTimetable(int maxDbm) {
  final out = <BenchSlot>[];
  var t = benchSettleMs;
  var last = 0;
  for (final s in benchSteps) {
    if (benchSkips(s, maxDbm)) continue;
    final sig = t;
    t += benchSignalMs(s, last);
    out.add(BenchSlot(s, sig, t, t + s.secs * 1000));
    t += s.secs * 1000;
    last = s.step;
  }
  return out;
}

/// From step 1's signal to switching off, ms (bench_power_ms).
int benchTotalMs(int maxDbm) {
  final tt = benchTimetable(maxDbm);
  return tt.last.startMs - benchSettleMs;
}

/// Where the session is at `ms` after "BN start": the slot (null before the
/// first signal) and whether its signal is still on.
class BenchNow {
  final BenchSlot? slot;
  final bool signal; // the board is buzzing the number (nothing to read yet)
  final int leftMs; // until the step (or its signal) ends; 0 for step 6

  const BenchNow(this.slot, this.signal, this.leftMs);
}

BenchNow benchAt(List<BenchSlot> tt, int ms) {
  if (tt.isEmpty || ms < tt.first.signalMs) {
    return BenchNow(null, false, tt.isEmpty ? 0 : tt.first.signalMs - ms);
  }
  for (final s in tt) {
    if (ms < s.startMs) return BenchNow(s, true, s.startMs - ms);
    if (ms < s.endMs) return BenchNow(s, false, s.endMs - ms);
  }
  return BenchNow(tt.last, false, 0); // step 6: off until the key wakes it
}
