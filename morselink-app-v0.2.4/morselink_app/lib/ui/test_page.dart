// MorseLink - the field tests (docs/TEST_PLAN.md).
//
// Rover (the phone with the board you carry): fill in the set-up, Start;
// the app checks what it can see (v0.2.0: LoRa up, both boards on the same
// range level, the relay, silent mode...) and asks before it starts. The
// base phone needs nothing: with the app open and its board connected, it
// starts and stops when the rover does (over the internet relay).
// During a session: the live counters, the link's losses, and the label
// buttons.
//
// v0.2.1: the point test (a label for the spot, a single setting or a
// sweep, the steps' countdown and counts, how each board hears the other,
// "Repeat at the next spot"), a red notice when the base phone hasn't
// answered, and the way to the meter session.

import 'dart:async';

import 'package:flutter/material.dart';

import '../app_state.dart';
import '../protocol.dart';
import 'meter_page.dart';

class TestPage extends StatefulWidget {
  final AppState app;

  const TestPage({super.key, required this.app});

  @override
  State<TestPage> createState() => _TestPageState();
}

class _TestPageState extends State<TestPage> {
  final TestMeta _meta = TestMeta();
  late final TextEditingController _tx;
  late final TextEditingController _base;
  late final TextEditingController _height;
  late final TextEditingController _battery;
  late final TextEditingController _phone;
  late final TextEditingController _note;
  late final TextEditingController _label;
  String _error = '';
  bool _starting = false;
  Timer? _tick;

  static const _testNames = ['baseline', 'dynamic', 'point', 'rundown-a', 'rundown-b', 'rundown-l'];
  static const _paths = {'both': 'LoRa + relay', 'lora': 'LoRa only', 'relay': 'Relay only'};
  static const _areas = {'rural': 'Rural', 'suburban': 'Suburban', 'urban': 'Urban', 'dense_urban': 'Dense urban'};
  static const _sits = {'car': 'Car', 'walking': 'Walking outdoors', 'indoors': 'Indoors', 'stationary': 'Stationary'};

  AppState get app => widget.app;

  /// What a test does, at the board's spreading factor (the pace follows it).
  String _describe(String test) {
    final pace = testPaceText(test, app.radio?.sf ?? 9);
    switch (test) {
      case 'baseline':
        return 'Baseline: both boards fixed; $pace.';
      case 'dynamic':
        return 'Dynamic: the rover moves; $pace, until Stop.';
      case 'rundown-a':
        return 'Run-down A: listening only, Bluetooth off, until the battery is empty.';
      case 'rundown-b':
        return 'Run-down B: messages as in dynamic ($pace) with Bluetooth on, until the battery is empty.';
      case 'rundown-l':
        return 'Run-down L: messages over LoRa only ($pace), Bluetooth off, until the battery is empty.';
      case 'point':
        return 'Point test: both boards still, at one spot (1 m up, antennas upright, phones 1 m away). Each step: '
            '$pointMessages messages of $pointSize characters from each board over LoRa, sent even while the link is '
            'lost; both boards switch to the step\'s setting by themselves and back at the end. '
            '${minutesText(planSeconds(_meta.steps))} for this plan.';
      default:
        return '';
    }
  }

  /// The paths the board will use: rundown-l and the point test are LoRa
  /// only whatever is picked.
  String get _effectivePaths => _meta.test == 'rundown-l' || _meta.test == 'point' ? 'lora' : _meta.paths;

  @override
  void initState() {
    super.initState();
    final s = app.settings;
    _tx = TextEditingController(text: s.txDbm);
    _base = TextEditingController(text: s.baseDesc);
    _height = TextEditingController(text: s.baseHeight);
    _battery = TextEditingController(text: s.battery);
    _phone = TextEditingController(text: s.phoneName);
    _note = TextEditingController();
    _label = TextEditingController();
    final last = app.lastMeta;
    if (last != null && last.test == 'point') {
      // Back on the page after a point test: the same plan, ready for the next spot.
      _meta.test = 'point';
      _meta.sweep = last.sweep;
      _meta.plan = List.of(last.plan);
      _meta.single = last.single;
    }
    _tick = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted && app.session != null) setState(() {});
    });
  }

  @override
  void dispose() {
    _tick?.cancel();
    for (final c in [_tx, _base, _height, _battery, _phone, _note, _label]) {
      c.dispose();
    }
    super.dispose();
  }

  /// Start: save the form, run the checks, show them and ask; then start.
  Future<void> _start() async {
    setState(() {
      _starting = true;
      _error = '';
    });
    final s = app.settings;
    s.txDbm = _tx.text.trim();
    s.baseDesc = _base.text.trim();
    s.baseHeight = _height.text.trim();
    s.battery = _battery.text.trim();
    s.phoneName = _phone.text.trim();
    await s.save();
    _meta.txDbm = s.txDbm;
    _meta.baseDesc = s.baseDesc;
    _meta.baseHeight = s.baseHeight;
    _meta.battery = s.battery;
    _meta.note = _note.text.trim();
    _meta.label = _label.text.trim();
    final checks = await app.startChecks(_meta);
    if (!mounted) return;
    final go = await _confirm(checks);
    if (!mounted) return;
    if (!go) {
      setState(() => _starting = false);
      return;
    }
    final err = await app.startRover(_meta);
    if (!mounted) return;
    setState(() {
      _starting = false;
      _error = err ?? '';
    });
  }

  /// Repeat at the next spot: the last point test's plan again, with a new
  /// label (asked for here), through the same checks.
  Future<void> _repeat() async {
    final last = app.lastMeta;
    if (last == null) return;
    final c = TextEditingController();
    final label = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Next spot'),
        content: TextField(
            controller: c,
            autofocus: true,
            decoration: InputDecoration(labelText: 'Label', hintText: last.label.isEmpty ? 'pillar A, clear, 12 m' : last.label)),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, c.text), child: const Text('Next')),
        ],
      ),
    );
    if (label == null || label.trim().isEmpty || !mounted) return;
    setState(() {
      _meta.test = 'point';
      _meta.sweep = last.sweep;
      _meta.plan = List.of(last.plan);
      _meta.single = last.single;
      _label.text = label.trim();
    });
    await _start();
  }

  /// The confirmation: what will start, and what the checks found. Problems
  /// that block the start leave only OK; warnings allow "Start anyway".
  Future<bool> _confirm(List<StartCheck> checks) async {
    final blocked = checks.any((c) => c.blocking);
    final r = app.radio;
    final pr = app.partnerRadio;
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(blocked ? "Can't start yet" : 'Start ${_meta.test}?'),
        content: SingleChildScrollView(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              if (!blocked) ...[
                Text(_describe(_meta.test)),
                const SizedBox(height: 6),
                if (_meta.test == 'point') ...[
                  Text('Label: ${_meta.label}'),
                  for (var i = 0; i < _meta.steps.length; i++)
                    Text('Step ${i + 1}: ${_meta.steps[i].text} (${minutesText(pointStepS(_meta.steps[i].sf))})'),
                ],
                Text('Paths: ${_paths[_effectivePaths] ?? _effectivePaths}'),
                Text('With ${app.partner} (its phone starts its board too)'),
                if (r != null) Text('This board: ${r.text}'),
                if (pr != null) Text('${app.partner}: ${pr.text}'),
              ],
              for (final c in checks)
                Padding(
                  padding: const EdgeInsets.only(top: 8),
                  child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Icon(c.blocking ? Icons.block : Icons.warning_amber_rounded,
                        size: 18, color: c.blocking || c.severe ? Colors.red : Colors.orange),
                    const SizedBox(width: 6),
                    Expanded(child: Text(c.text)),
                  ]),
                ),
              if (!blocked && checks.isEmpty)
                const Padding(padding: EdgeInsets.only(top: 8), child: Text('Everything the app can see looks ready.')),
            ],
          ),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: Text(blocked ? 'OK' : 'Cancel')),
          if (!blocked)
            FilledButton(
                onPressed: () => Navigator.pop(context, true), child: Text(checks.isEmpty ? 'Start' : 'Start anyway')),
        ],
      ),
    );
    return ok == true;
  }

  Future<void> _addNote() async {
    final c = TextEditingController();
    final text = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Note'),
        content: TextField(controller: c, autofocus: true, decoration: const InputDecoration(hintText: 'e.g. mall, 2nd floor')),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, c.text), child: const Text('Save')),
        ],
      ),
    );
    if (text != null) app.addNote(text);
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: app,
      builder: (context, _) => Scaffold(
        appBar: AppBar(title: const Text('Tests')),
        body: app.session == null ? _setup(context) : _running(context, app.session!),
      ),
    );
  }

  Widget _chips(Map<String, String> options, String selected, void Function(String) onPick) {
    return Wrap(
      spacing: 8,
      runSpacing: 4,
      children: [
        for (final e in options.entries)
          ChoiceChip(label: Text(e.value), selected: selected == e.key, onSelected: (_) => onPick(e.key)),
      ],
    );
  }

  Widget _field(TextEditingController c, String label, {String? hint}) {
    return Padding(
      padding: const EdgeInsets.only(top: 8),
      child: TextField(controller: c, decoration: InputDecoration(labelText: label, hintText: hint, isDense: true)),
    );
  }

  /// The point test's settings: the spot's label, then a single setting or
  /// a sweep (editable), and how long it takes.
  Widget _pointSetup(BuildContext context) {
    final tt = Theme.of(context).textTheme;
    final steps = _meta.steps;
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      const SizedBox(height: 12),
      TextField(
        controller: _label,
        decoration: const InputDecoration(
            labelText: 'Label (required): the spot', hintText: 'pillar A, behind, 12 m', isDense: true),
      ),
      const SizedBox(height: 12),
      Text('Settings', style: tt.titleMedium),
      const SizedBox(height: 4),
      _chips(const {'sweep': 'Sweep', 'single': 'Single'}, _meta.sweep ? 'sweep' : 'single',
          (v) => setState(() => _meta.sweep = v == 'sweep')),
      const SizedBox(height: 8),
      if (!_meta.sweep) ...[
        _chips({for (final sf in planSfs) '$sf': 'G${sf - 8} SF$sf'}, '${_meta.single.sf}',
            (v) => setState(() => _meta.single = PlanStep(int.parse(v), _meta.single.dbm))),
        const SizedBox(height: 4),
        _chips({for (final d in planDbms) '$d': '$d dBm'}, '${_meta.single.dbm}',
            (v) => setState(() => _meta.single = PlanStep(_meta.single.sf, int.parse(v)))),
      ] else ...[
        for (var i = 0; i < _meta.plan.length; i++)
          Row(children: [
            SizedBox(width: 60, child: Text('Step ${i + 1}')),
            DropdownButton<int>(
              value: _meta.plan[i].sf,
              items: [for (final sf in planSfs) DropdownMenuItem(value: sf, child: Text('SF$sf'))],
              onChanged: (v) => setState(() => _meta.plan[i] = PlanStep(v ?? _meta.plan[i].sf, _meta.plan[i].dbm)),
            ),
            const SizedBox(width: 12),
            DropdownButton<int>(
              value: planDbms.contains(_meta.plan[i].dbm) ? _meta.plan[i].dbm : planDbms.last,
              items: [for (final d in planDbms) DropdownMenuItem(value: d, child: Text('$d dBm'))],
              onChanged: (v) => setState(() => _meta.plan[i] = PlanStep(_meta.plan[i].sf, v ?? _meta.plan[i].dbm)),
            ),
            const Spacer(),
            IconButton(
              icon: const Icon(Icons.remove_circle_outline),
              tooltip: 'Remove this step',
              onPressed: _meta.plan.length <= 1 ? null : () => setState(() => _meta.plan.removeAt(i)),
            ),
          ]),
        Wrap(spacing: 8, children: [
          OutlinedButton.icon(
            icon: const Icon(Icons.add),
            label: const Text('Add a step'),
            onPressed: _meta.plan.length >= planMax
                ? null
                : () => setState(() => _meta.plan.add(_meta.plan.isEmpty ? const PlanStep(9, 22) : _meta.plan.last)),
          ),
          TextButton(
            onPressed: () => setState(() => _meta.plan = List.of(defaultPlan)),
            child: const Text('Default sweep'),
          ),
        ]),
      ],
      Padding(
        padding: const EdgeInsets.only(top: 4),
        child: Text('${steps.length} step${steps.length == 1 ? '' : 's'}, ${minutesText(planSeconds(steps))} '
            '(at most 2 sweeps an hour: the 10 % airtime rule).'),
      ),
    ]);
  }

  Widget _setup(BuildContext context) {
    final ready = app.bleState == 'connected' && app.link == 'connected' && app.mqttState == 'connected';
    final r = app.radio;
    final paths = app.lora.isEmpty
        ? ''
        : ' (LoRa ${pathStateText(app.lora, app.partner)}, relay ${pathStateText(app.relayPath, app.partner)})';
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        const Text(
          'This phone is the rover: it goes with the board you carry. On the base phone just open the app '
          '(board connected, internet on): it starts and stops with you. Both boards must be linked (S), '
          'on the same range level (G), in silent mode (Z), and loopback off.',
        ),
        const SizedBox(height: 12),
        Text(
            'Ready: board ${app.bleState}, link ${app.link}${app.partner == '-' ? '' : ' to ${app.partner}'}$paths, '
            'internet ${app.mqttState}',
            style: TextStyle(color: ready ? Colors.green : Colors.red)),
        const SizedBox(height: 16),
        Text('Test', style: Theme.of(context).textTheme.titleMedium),
        const SizedBox(height: 4),
        _chips({for (final k in _testNames) k: k}, _meta.test, (v) => setState(() => _meta.test = v)),
        Padding(padding: const EdgeInsets.only(top: 4), child: Text(_describe(_meta.test))),
        if (_meta.test == 'point') _pointSetup(context),
        const SizedBox(height: 16),
        Text('Paths', style: Theme.of(context).textTheme.titleMedium),
        const SizedBox(height: 4),
        if (_meta.test == 'rundown-l')
          const Text('LoRa only (Bluetooth is off during this run).')
        else if (_meta.test == 'point')
          const Text('LoRa only (the relay plays no part in a point test).')
        else if (_meta.test == 'rundown-a')
          const Text('None: this run only listens.')
        else
          _chips(_paths, _meta.paths, (v) => setState(() => _meta.paths = v)),
        const SizedBox(height: 8),
        if (r != null)
          Padding(
            padding: const EdgeInsets.only(top: 8),
            child: Text('Radio (from the board): ${r.text}. G on the device sets the range level; '
                '"radio power" on the console the power.'),
          )
        else
          _field(_tx, 'Transmit power, dBm (as set with "radio power")'),
        _field(_base, 'Base: where it stands', hint: 'garden wall, outdoors'),
        _field(_height, 'Base: height above ground', hint: '1 m'),
        _field(_battery, 'Batteries', hint: 'B1 3000 mAh (rover), B2 (base)'),
        _field(_phone, 'This phone', hint: 'my Galaxy A52'),
        _field(_note, 'Note (optional)'),
        const SizedBox(height: 20),
        FilledButton.icon(
          icon: const Icon(Icons.play_arrow),
          label: Text(_starting ? 'Checking...' : 'Start'),
          onPressed: _starting ? null : _start,
        ),
        if (app.lastMeta?.test == 'point')
          Padding(
            padding: const EdgeInsets.only(top: 8),
            child: OutlinedButton.icon(
              icon: const Icon(Icons.redo),
              label: const Text('Repeat at the next spot'),
              onPressed: _starting ? null : _repeat,
            ),
          ),
        if (_error.isNotEmpty) Padding(padding: const EdgeInsets.only(top: 12), child: Text(_error, style: const TextStyle(color: Colors.red))),
        if (app.notice.isNotEmpty)
          Padding(
              padding: const EdgeInsets.only(top: 12),
              child: Text(app.notice, style: app.baseAlarm ? const TextStyle(color: Colors.red) : null)),
        if (app.lastSessionFile.isNotEmpty)
          Padding(padding: const EdgeInsets.only(top: 12), child: Text('Last session file: ${app.lastSessionFile}')),
        const Divider(height: 32),
        OutlinedButton.icon(
          icon: const Icon(Icons.electric_meter_outlined),
          label: const Text('Meter session (power by meter)'),
          onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => MeterPage(app: app))),
        ),
      ],
    );
  }

  String _elapsed(TestSession s) {
    final d = DateTime.now().difference(s.started);
    final h = d.inHours;
    final m = d.inMinutes % 60;
    final sec = d.inSeconds % 60;
    return '${h > 0 ? '$h h ' : ''}$m min ${sec.toString().padLeft(2, '0')} s';
  }

  static String _hhmm(DateTime t) => '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';

  Widget _row(String a, String b, {Color? color}) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(children: [
        SizedBox(width: 150, child: Text(a)),
        Expanded(child: Text(b, style: color == null ? null : TextStyle(color: color))),
      ]),
    );
  }

  Widget _counts(String title, PathCounts c) {
    return _row(title, 'sent ${c.sent}, delivered ${c.ack}, failed ${c.fail}, skipped ${c.skip}  (${c.rate})');
  }

  static String _signal(num? rssi, num? snr) => rssi == null ? '-' : '$rssi dBm, SNR ${snr ?? '-'} dB';

  /// The point test while it runs: the step and its countdown, each step's
  /// counts, and how each board hears the other.
  List<Widget> _pointRunning(BuildContext context, TestSession s) {
    final n = s.plan.length;
    final their = s.theirAt;
    return [
      _row('Spot', s.label),
      if (s.step > 0)
        _row('Step ${s.step} of $n', 'SF${s.stepSf} / ${s.stepDbm} dBm, ${minutesText(s.stepLeftS)} left')
      else
        _row('Step', s.stepAt == null ? 'starting' : 'done: the board is back on its own setting'),
      for (var i = 0; i < n; i++) _counts('  ${i + 1}: ${s.plan[i].text}', s.stepCounts[i]),
      _row('We hear them', _signal(s.rssi, s.snr)),
      if (s.role == 'rover')
        _row('They hear us', their == null ? '-' : '${_signal(s.theirRssi, s.theirSnr)} (${DateTime.now().difference(their).inSeconds} s ago)'),
    ];
  }

  Future<void> _editLabel(TestSession s) async {
    final c = TextEditingController(text: s.label);
    final text = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Label'),
        content: TextField(controller: c, autofocus: true),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, c.text), child: const Text('Save')),
        ],
      ),
    );
    if (text != null) app.setPointLabel(text);
  }

  Widget _running(BuildContext context, TestSession s) {
    final dist = app.distanceToBase;
    final f = app.fix;
    final rover = s.role == 'rover';
    // The link's losses during the session (the board's LINK lines).
    var linkText = '${app.link} (${app.partner})';
    final at = s.lastLostAt;
    if (s.lost > 0 && at != null) {
      linkText += ', lost ${s.lost}x (last at ${_hhmm(at)}: ${lostWhyText(s.lastLostWhy)})';
    }
    final loraState = s.pathL.isNotEmpty ? s.pathL : app.lora;
    final r = app.radio;
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Text('${s.test} - ${s.paths} - ${s.role}', style: Theme.of(context).textTheme.titleLarge),
        Text('Session ${s.id}, ${_elapsed(s)}'),
        if (rover && app.baseAlarm)
          Container(
            margin: const EdgeInsets.only(top: 8),
            padding: const EdgeInsets.all(10),
            color: Colors.red.shade700,
            child: Text(app.notice, style: const TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
          ),
        const SizedBox(height: 8),
        _row('Board', s.board),
        if (rover) _row('Base phone', app.baseStatus, color: app.baseAlarm ? Colors.red : null),
        _row('Link', linkText, color: s.linkUp ? null : Colors.red),
        if (loraState.isNotEmpty) _row('LoRa path', pathStateText(loraState, app.partner), color: pathLevel(loraState) == PathLevel.good ? null : Colors.orange.shade800),
        _row('Internet', app.mqttState),
        if (r != null) _row('Radio', r.text),
        const Divider(),
        if (s.isPoint) ..._pointRunning(context, s),
        if (s.isPoint) const Divider(),
        _row('Messages made', '${s.gen}'),
        _counts('LoRa', s.lora),
        if (!s.isPoint) _counts('Relay', s.relay),
        if (!s.isPoint) _row('Last LoRa packet', s.rssi == null ? '-' : '${s.rssi} dBm, SNR ${s.snr} dB'),
        _row('Board battery', s.batPct == null ? '-' : '${s.batPct} %'),
        _row('GPS', f == null ? 'no fix yet' : '+/- ${f.accuracy.toStringAsFixed(0)} m'),
        if (rover) _row('Distance to base', dist == null ? '-' : '${dist.toStringAsFixed(0)} m'),
        const Divider(),
        if (s.isPoint && rover) ...[
          OutlinedButton.icon(icon: const Icon(Icons.label_outline), label: const Text('Edit the label'), onPressed: () => _editLabel(s)),
          const SizedBox(height: 8),
          OutlinedButton.icon(icon: const Icon(Icons.edit_note), label: const Text('Note'), onPressed: _addNote),
          const SizedBox(height: 20),
        ] else if (rover) ...[
          Text('Area', style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 4),
          _chips(_areas, app.area, app.setArea),
          const SizedBox(height: 12),
          Text('Situation (set "Car" before driving off; never tap while driving)',
              style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 4),
          _chips(_sits, app.situation, app.setSituation),
          const SizedBox(height: 12),
          OutlinedButton.icon(icon: const Icon(Icons.edit_note), label: const Text('Note'), onPressed: _addNote),
          const SizedBox(height: 20),
        ],
        FilledButton.icon(
          style: FilledButton.styleFrom(backgroundColor: Colors.red),
          icon: const Icon(Icons.stop),
          label: Text(s.stopping ? 'Stopping... (tap again to close now)' : rover ? 'Stop (both boards)' : 'Stop this board'),
          onPressed: app.stopSession,
        ),
        const SizedBox(height: 12),
        Text('Log file: ${s.file}', style: Theme.of(context).textTheme.bodySmall),
        if (app.notice.isNotEmpty)
          Padding(
              padding: const EdgeInsets.only(top: 8),
              child: Text(app.notice, style: app.baseAlarm ? const TextStyle(color: Colors.red, fontWeight: FontWeight.bold) : null)),
      ],
    );
  }
}
