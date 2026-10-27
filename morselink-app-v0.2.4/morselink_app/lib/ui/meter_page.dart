// MorseLink - the meter session (firmware v0.6.1, bench power;
// docs/TEST_PLAN.md section 17): the board's current, step by step, read on a
// multimeter in series with the battery.
//
// Started from here so the board never needs USB (on USB it runs from USB,
// and the battery charges through the meter, which can blow its fuse). The
// board then steps through its states by itself, buzzing each step's number
// on M1. In steps 1 to 3a its Bluetooth is off (or not connectable), so this
// page runs the same timetable (lib/bench.dart) and shows where it is; from
// 3b on the board is connected again and says each step itself.

import 'dart:async';

import 'package:flutter/material.dart';

import '../app_state.dart';
import '../bench.dart';

class MeterPage extends StatefulWidget {
  final AppState app;

  const MeterPage({super.key, required this.app});

  @override
  State<MeterPage> createState() => _MeterPageState();
}

class _MeterPageState extends State<MeterPage> {
  String _error = '';
  Timer? _tick;

  AppState get app => widget.app;

  @override
  void initState() {
    super.initState();
    _tick = Timer.periodic(const Duration(milliseconds: 500), (_) {
      if (mounted && app.bench != null) setState(() {});
    });
  }

  @override
  void dispose() {
    _tick?.cancel();
    super.dispose();
  }

  /// Ask before starting: the procedure, then BENCH POWER.
  Future<void> _start() async {
    final block = app.benchBlocking();
    if (block != null) {
      setState(() => _error = block);
      return;
    }
    final go = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Start the meter session?'),
        content: const SingleChildScrollView(
          child: Text('Check first:\n'
              '- the board battery is full (charge LED off);\n'
              '- the meter is in series with the battery, on its mA range;\n'
              '- USB is NOT connected (the board would run from USB, and the battery would charge through the meter);\n'
              '- the motors are connected, and the key;\n'
              '- this phone stays next to the board with the app open.\n\n'
              'The board then goes through every step by itself (about 9 minutes). Read each step in its second '
              'half. A 5 s hold on the key stops it at any time.'),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Start')),
        ],
      ),
    );
    if (go != true || !mounted) return; // (the page may have closed while the dialog was open)
    setState(() => _error = app.startBench() ?? '');
  }

  static String _secs(int ms) => '${(ms / 1000).ceil()} s';

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: app,
      builder: (context, _) {
        final b = app.bench;
        return Scaffold(
          appBar: AppBar(title: const Text('Meter session')),
          body: ListView(
            padding: const EdgeInsets.all(16),
            children: [
              if (b == null || !b.active) ..._idle(context, b) else ..._running(context, b),
              const Divider(height: 32),
              Text('The steps', style: Theme.of(context).textTheme.titleMedium),
              const SizedBox(height: 4),
              ..._stepList(context, b),
            ],
          ),
        );
      },
    );
  }

  /// Before (or after) a session: what it is, the Start button, the last result.
  List<Widget> _idle(BuildContext context, BenchRun? b) {
    final total = benchTotalMs(22) ~/ 1000;
    return [
      Text('The board goes through ${benchSteps.length} steps by itself, about ${total ~/ 60} min ${total % 60} s in '
          'all; motor M1 buzzes each step\'s number before it (one short buzz between the parts of a step). Write '
          'down the current of each step on the sheet (TEST_PLAN 14.1).'),
      const SizedBox(height: 16),
      FilledButton.icon(icon: const Icon(Icons.play_arrow), label: const Text('Start'), onPressed: _start),
      if (_error.isNotEmpty) Padding(padding: const EdgeInsets.only(top: 12), child: Text(_error, style: const TextStyle(color: Colors.red))),
      if (b != null && b.state == 'refused')
        Padding(padding: const EdgeInsets.only(top: 12), child: Text('The board refused: ${_refusal(b.detail)}', style: const TextStyle(color: Colors.red))),
      if (b != null && b.state == 'ended')
        Padding(
          padding: const EdgeInsets.only(top: 12),
          child: Row(children: [
            Expanded(child: Text('Last session: ${_endText(b.detail)}')),
            TextButton(onPressed: app.clearBench, child: const Text('Clear')),
          ]),
        ),
    ];
  }

  static String _refusal(String why) => switch (why) {
        'test' => 'a test is running',
        'sos' => 'your SOS is active (a 5 s hold cancels it)',
        'running' => 'it is already running',
        _ => why,
      };

  static String _endText(String how) => switch (how) {
        'off' => 'done: the board switched off (step 6). Read the off current, then hold the key 2 s.',
        'app' => 'stopped from this phone',
        'hold' => 'stopped with a 5 s hold',
        'stopped' => 'stopped from the console',
        _ => how,
      };

  /// While it runs: the step now, what to do, the time left, what comes next.
  List<Widget> _running(BuildContext context, BenchRun b) {
    final tt = Theme.of(context).textTheme;
    final out = <Widget>[];
    if (b.state == 'asking') {
      out.add(const Text('Asking the board...'));
    } else if (b.state == 'wait-usb') {
      out.add(const Text('Unplug USB: the board starts 5 s after USB is out.', style: TextStyle(fontSize: 18)));
    } else {
      final now = b.now();
      final slot = now.slot;
      if (slot == null) {
        out.add(Text('Starting: step 1 in ${_secs(now.leftMs)} (let the meter settle)', style: const TextStyle(fontSize: 18)));
      } else {
        final st = slot.step;
        out.add(Text('Step ${st.id}', style: tt.displaySmall));
        out.add(Text(st.what, style: const TextStyle(fontSize: 18)));
        if (now.signal) {
          out.add(Text('M1 is buzzing the step number: starts in ${_secs(now.leftMs)}'));
        } else if (st.kind == BenchKind.off) {
          out.add(Text(st.todo, style: const TextStyle(fontWeight: FontWeight.bold)));
        } else {
          out.add(Text('${_secs(now.leftMs)} left${now.leftMs <= st.secs * 500 ? '  - read the meter now' : ''}'));
          if (st.todo.isNotEmpty) {
            out.add(Padding(
                padding: const EdgeInsets.only(top: 6),
                child: Text('You: ${st.todo}', style: const TextStyle(fontWeight: FontWeight.bold))));
          }
        }
        final i = b.table.indexOf(slot);
        if (i >= 0 && i + 1 < b.table.length) {
          final nx = b.table[i + 1].step;
          out.add(Padding(padding: const EdgeInsets.only(top: 8), child: Text('Next: ${nx.id}, ${nx.what}', style: tt.bodySmall)));
        }
      }
      out.add(Padding(
        padding: const EdgeInsets.only(top: 8),
        child: Text(app.ble.ready ? 'The board is connected (it says each step itself).' : 'The board is not connected '
            "(expected in steps 1-3a): this page keeps time by itself.", style: tt.bodySmall),
      ));
      if (b.detail.isNotEmpty) out.add(Text(b.detail, style: const TextStyle(color: Colors.orange)));
    }
    out.add(const SizedBox(height: 16));
    out.add(OutlinedButton.icon(
      icon: const Icon(Icons.stop),
      label: Text(app.ble.ready ? 'Stop' : 'Stop (a 5 s hold on the board stops it now)'),
      onPressed: app.stopBench,
    ));
    return out;
  }

  /// Every step with its time; the one running highlighted.
  List<Widget> _stepList(BuildContext context, BenchRun? b) {
    final now = b?.active == true ? b!.now().slot?.step.id : null;
    final max = b?.maxDbm ?? 22;
    return [
      for (final s in benchSteps)
        if (!benchSkips(s, max))
          Container(
            color: s.id == now ? Theme.of(context).colorScheme.primaryContainer : null,
            padding: const EdgeInsets.symmetric(vertical: 3, horizontal: 4),
            child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
              SizedBox(width: 36, child: Text(s.id, style: const TextStyle(fontWeight: FontWeight.bold))),
              Expanded(child: Text(s.what)),
              SizedBox(width: 48, child: Text(s.secs == 0 ? '' : '${s.secs} s', textAlign: TextAlign.right)),
            ]),
          ),
    ];
  }
}
