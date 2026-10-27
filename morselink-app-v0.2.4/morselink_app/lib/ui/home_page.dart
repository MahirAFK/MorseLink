// MorseLink - the main screen: the device's status (v0.2.0: each path,
// LoRa and the relay, and the radio setting), an SOS banner, the keying box
// (the live draft, or in command mode the command being keyed), and the
// chat (messages sent and received, and events). v0.2.2 (firmware v0.6.2):
// the keying box shows each letter's dots and dashes above it, the letter
// being keyed, the last press's timing and the speeds; while something
// plays on the arm, what it is; the Learn page (the school icon) has all of
// it large.

import 'package:flutter/material.dart';

import '../app_state.dart';
import '../learning.dart';
import '../protocol.dart';
import 'connect_page.dart';
import 'learn_page.dart';
import 'learn_widgets.dart';
import 'manual_page.dart';
import 'meter_page.dart';
import 'settings_page.dart';
import 'test_page.dart';

class HomePage extends StatelessWidget {
  final AppState app;

  const HomePage({super.key, required this.app});

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: app,
      builder: (context, _) {
        return Scaffold(
          appBar: AppBar(
            title: Text(app.ownId.isEmpty ? 'MorseLink' : 'MorseLink ${app.ownId}'),
            actions: [
              IconButton(
                icon: const Icon(Icons.school_outlined),
                tooltip: 'Learn',
                onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => LearnPage(app: app))),
              ),
              IconButton(
                icon: const Icon(Icons.help_outline),
                tooltip: 'Manual',
                onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => const ManualPage())),
              ),
              IconButton(
                icon: const Icon(Icons.science_outlined),
                tooltip: 'Tests',
                onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => TestPage(app: app))),
              ),
              IconButton(
                icon: const Icon(Icons.settings_outlined),
                tooltip: 'Settings',
                onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => SettingsPage(app: app))),
              ),
            ],
          ),
          body: Column(
            children: [
              if (app.settings.deviceId.isEmpty) _ConnectPrompt(app: app) else _StatusCard(app: app),
              if (app.session != null) _TestBanner(app: app),
              if (app.bench?.active == true) _BenchBanner(app: app),
              if (app.sosFrom != null || app.ownSos) _SosBanner(app: app),
              _DraftCard(app: app),
              if (app.learn.playing)
                Padding(
                  padding: const EdgeInsets.fromLTRB(12, 0, 12, 8),
                  child: NowPlayingCard(app: app),
                ),
              const Divider(height: 1),
              Expanded(child: _ChatList(app: app)),
            ],
          ),
        );
      },
    );
  }
}

/// No device chosen yet.
class _ConnectPrompt extends StatelessWidget {
  final AppState app;

  const _ConnectPrompt({required this.app});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Text('No MorseLink chosen yet.', style: TextStyle(fontSize: 16)),
          const SizedBox(height: 8),
          const Text('On the device: command mode, O, hold 2 s (60 s to pair). Then tap below.'),
          const SizedBox(height: 12),
          FilledButton.icon(
            icon: const Icon(Icons.bluetooth_searching),
            label: const Text('Find my MorseLink'),
            onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => ConnectPage(app: app))),
          ),
        ],
      ),
    );
  }
}

/// The colour of a path state (protocol.dart's PathLevel).
Color pathColor(String state) => switch (pathLevel(state)) {
      PathLevel.good => Colors.green,
      PathLevel.warn => Colors.orange,
      PathLevel.bad => Colors.red,
      PathLevel.neutral => Colors.grey,
    };

/// Board, internet, partner and link, battery, silent mode; the two paths
/// and the radio setting (firmware v0.6.0).
class _StatusCard extends StatelessWidget {
  final AppState app;

  const _StatusCard({required this.app});

  @override
  Widget build(BuildContext context) {
    final linkColor = app.link == 'connected'
        ? Colors.green
        : app.link == 'lost' || app.link == 'ended'
            ? Colors.red
            : Colors.grey;
    final bleOk = app.bleState == 'connected';
    final netOk = app.mqttState == 'connected';
    final small = Theme.of(context).textTheme.bodySmall;
    // The board's own words for its Bluetooth state; while connected but not
    // yet accepted (no HI), say what it waits for.
    var bleText = app.bleState;
    if (bleOk && !app.accepted) bleText = 'connected, waiting for the board to accept this phone';
    final showPaths = bleOk && app.accepted && app.partner != '-' && app.lora.isNotEmpty;
    final r = app.radio;
    return Card(
      margin: const EdgeInsets.fromLTRB(12, 8, 12, 4),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(children: [
              Icon(bleOk ? Icons.bluetooth_connected : Icons.bluetooth_disabled, color: bleOk ? Colors.blue : Colors.grey),
              const SizedBox(width: 6),
              Expanded(child: Text('${app.settings.deviceName}: $bleText')),
              Icon(netOk ? Icons.cloud_done_outlined : Icons.cloud_off_outlined, color: netOk ? Colors.blue : Colors.grey),
              const SizedBox(width: 4),
              Text(netOk ? 'internet on' : 'internet ${app.mqttState}'),
            ]),
            if (app.bleWhy.isNotEmpty)
              Padding(
                padding: const EdgeInsets.only(left: 30, top: 2),
                child: Text(app.bleWhy, style: small?.copyWith(color: bleOk ? null : Colors.orange.shade800)),
              ),
            const SizedBox(height: 8),
            Row(children: [
              Icon(Icons.link, color: linkColor),
              const SizedBox(width: 6),
              Expanded(
                child: Text(app.partner == '-' ? 'No partner' : 'Partner ${app.partner}: ${app.link}',
                    style: const TextStyle(fontSize: 16)),
              ),
              const Icon(Icons.battery_std, size: 18),
              Text(app.battery == '?' ? '?' : '${app.battery} %'),
              if (app.silent) ...[
                const SizedBox(width: 8),
                const Icon(Icons.visibility_off, size: 18),
              ],
            ]),
            if (showPaths) ...[
              const SizedBox(height: 6),
              _PathRow(label: 'LoRa', state: app.lora, who: app.partner),
              _PathRow(
                label: 'Relay',
                state: app.relayPath,
                who: app.partner,
                note: app.relayPath == 'off' && app.session != null ? ' (test running: the test sends its own copies)' : '',
              ),
            ],
            if (bleOk && r != null)
              Padding(
                padding: const EdgeInsets.only(top: 4),
                child: Text('Radio: ${r.text}', style: small),
              ),
          ],
        ),
      ),
    );
  }
}

/// One path's line: a coloured dot, "LoRa connected".
class _PathRow extends StatelessWidget {
  final String label;
  final String state;
  final String who;
  final String note;

  const _PathRow({required this.label, required this.state, required this.who, this.note = ''});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(left: 6, top: 2),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Padding(
          padding: const EdgeInsets.only(top: 4),
          child: Icon(Icons.circle, size: 10, color: pathColor(state)),
        ),
        const SizedBox(width: 8),
        Expanded(child: Text('$label ${pathStateText(state, who)}$note')),
      ]),
    );
  }
}

class _TestBanner extends StatelessWidget {
  final AppState app;

  const _TestBanner({required this.app});

  @override
  Widget build(BuildContext context) {
    final s = app.session!;
    return Material(
      color: Colors.amber.shade200,
      child: ListTile(
        dense: true,
        leading: const Icon(Icons.science, color: Colors.black87),
        title: Text('Test ${s.test} (${s.role}) - ${s.board}', style: const TextStyle(color: Colors.black87)),
        subtitle: Text('Session ${s.id}', style: const TextStyle(color: Colors.black54)),
        onTap: () => Navigator.push(context, MaterialPageRoute(builder: (_) => TestPage(app: app))),
      ),
    );
  }
}

/// The meter session is running (v0.2.1): where it is; a tap opens its page.
class _BenchBanner extends StatelessWidget {
  final AppState app;

  const _BenchBanner({required this.app});

  @override
  Widget build(BuildContext context) {
    final b = app.bench!;
    final slot = b.now().slot;
    final what = b.state == 'wait-usb'
        ? 'unplug USB'
        : slot == null
            ? 'starting'
            : 'step ${slot.step.id}: ${slot.step.what}';
    return Material(
      color: Colors.lightBlue.shade100,
      child: ListTile(
        dense: true,
        leading: const Icon(Icons.electric_meter, color: Colors.black87),
        title: Text('Meter session: $what', style: const TextStyle(color: Colors.black87)),
        onTap: () => Navigator.push(context, MaterialPageRoute(builder: (_) => MeterPage(app: app))),
      ),
    );
  }
}

class _SosBanner extends StatelessWidget {
  final AppState app;

  const _SosBanner({required this.app});

  @override
  Widget build(BuildContext context) {
    final lines = <String>[];
    if (app.sosFrom != null) lines.add('SOS from ${app.sosFrom}');
    if (app.ownSos) {
      lines.add(app.sosAckBy == null ? 'Your SOS is active' : 'Your SOS: ${app.sosAckBy} acknowledged');
    }
    return Material(
      color: Colors.red.shade700,
      child: ListTile(
        leading: const Icon(Icons.warning_amber_rounded, color: Colors.white, size: 36),
        title: Text(lines.join('\n'),
            style: const TextStyle(color: Colors.white, fontSize: 20, fontWeight: FontWeight.bold)),
        subtitle: const Text('Answer on the device (tap to acknowledge; 5 s hold cancels your own).',
            style: TextStyle(color: Colors.white70)),
        trailing: IconButton(icon: const Icon(Icons.close, color: Colors.white), onPressed: app.clearSos),
      ),
    );
  }
}

/// The keying box: the draft, letter by letter as it is keyed; in command
/// mode (firmware v0.6.0) the command being keyed instead, under a COMMAND
/// note, with what its letter does (the draft is kept meanwhile). How the
/// last command ended shows for a few seconds.
class _DraftCard extends StatelessWidget {
  final AppState app;

  const _DraftCard({required this.app});

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;
    final cmd = app.cmd;
    final inCmd = cmd != null;
    final children = <Widget>[];
    if (cmd != null) {
      final what = cmd.isEmpty ? null : commandNames[cmd[0]];
      children.addAll([
        Row(children: [
          Text('COMMAND',
              style: tt.labelMedium?.copyWith(color: cs.tertiary, fontWeight: FontWeight.bold, letterSpacing: 1.5)),
          const Spacer(),
          if (app.draft.isNotEmpty) Text('draft kept', style: tt.labelSmall),
          InkWell(
            onTap: () => Navigator.push(
                context, MaterialPageRoute(builder: (_) => const ManualPage(open: 'Command mode'))),
            child: Padding(
                padding: const EdgeInsets.only(left: 8), child: Icon(Icons.help_outline, size: 18, color: cs.tertiary)),
          ),
        ]),
        const SizedBox(height: 4),
        Text(
          cmd.isEmpty ? '_' : cmd,
          style: TextStyle(
              fontSize: 26, fontFamily: 'monospace', letterSpacing: 2, color: cs.tertiary, fontWeight: FontWeight.bold),
        ),
        if (app.learn.open.isNotEmpty) // the letter being keyed (firmware v0.6.2)
          Text(morseGlyphs(app.learn.open),
              style: TextStyle(fontSize: 18, fontFamily: 'monospace', letterSpacing: 2, color: cs.primary)),
        Text(what ?? (cmd.isEmpty ? 'key a command letter (and its argument), then hold 2 s; hold 3 s to leave' : 'not a command'),
            style: tt.bodySmall),
        if (app.cmdEnd.isNotEmpty) Text(app.cmdEnd, style: tt.bodySmall?.copyWith(fontStyle: FontStyle.italic)),
        if (app.draft.isNotEmpty)
          Padding(
            padding: const EdgeInsets.only(top: 4),
            child: Text('Draft: ${app.draft}', style: tt.bodySmall, maxLines: 2, overflow: TextOverflow.ellipsis),
          ),
      ]);
    } else {
      children.addAll([
        Text('Keying', style: tt.labelSmall),
        const SizedBox(height: 4),
        // The letters with their Morse above them, the letter being keyed,
        // the last press's timing (firmware v0.6.2; older boards: letters only).
        KeyingView(app: app),
        if (app.cmdEnd.isNotEmpty) Text(app.cmdEnd, style: tt.bodySmall?.copyWith(fontStyle: FontStyle.italic)),
        if (app.learn.speeds != null) ...[const SizedBox(height: 6), SpeedsLine(speeds: app.learn.speeds)],
      ]);
    }
    return Container(
      width: double.infinity,
      margin: const EdgeInsets.fromLTRB(12, 4, 12, 8),
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        border: Border.all(color: inCmd ? cs.tertiary : cs.outlineVariant, width: inCmd ? 2 : 1),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: children),
    );
  }
}

class _ChatList extends StatelessWidget {
  final AppState app;

  const _ChatList({required this.app});

  @override
  Widget build(BuildContext context) {
    if (app.chat.isEmpty) {
      return const Center(child: Text('Messages and events appear here.'));
    }
    return ListView.builder(
      reverse: true,
      itemCount: app.chat.length,
      itemBuilder: (context, i) {
        final item = app.chat[app.chat.length - 1 - i];
        return _ChatTile(item: item, onReveal: () => app.revealChat(item));
      },
    );
  }
}

class _ChatTile extends StatelessWidget {
  final ChatItem item;
  final VoidCallback onReveal; // a message hidden for practice: show it

  const _ChatTile({required this.item, required this.onReveal});

  String _time() {
    final t = item.time;
    return '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    if (item.kind == ChatKind.event) {
      return Padding(
        padding: const EdgeInsets.symmetric(vertical: 4, horizontal: 16),
        child: Text('${_time()}  ${item.text}',
            textAlign: TextAlign.center, style: TextStyle(color: cs.outline, fontSize: 13)),
      );
    }
    if (item.kind == ChatKind.sos) {
      return Padding(
        padding: const EdgeInsets.symmetric(vertical: 4, horizontal: 16),
        child: Text('${_time()}  ${item.text}',
            textAlign: TextAlign.center, style: const TextStyle(color: Colors.red, fontWeight: FontWeight.bold)),
      );
    }
    final mine = item.kind == ChatKind.sent;
    IconData? icon;
    if (mine) {
      icon = item.state == SendState.delivered
          ? Icons.done
          : item.state == SendState.failed
              ? Icons.error_outline
              : Icons.schedule;
    }
    return Align(
      alignment: mine ? Alignment.centerRight : Alignment.centerLeft,
      child: Container(
        margin: const EdgeInsets.symmetric(vertical: 3, horizontal: 12),
        padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 12),
        constraints: const BoxConstraints(maxWidth: 320),
        decoration: BoxDecoration(
          color: mine ? cs.primaryContainer : cs.surfaceContainerHighest,
          borderRadius: BorderRadius.circular(12),
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(mine ? 'to ${item.who}' : 'from ${item.who}', style: Theme.of(context).textTheme.labelSmall),
            const SizedBox(height: 2),
            if (item.masked)
              // hidden while practising reading by feel (the Learn page's "Morse only" / "Hidden")
              InkWell(
                onTap: onReveal,
                child: Text('Hidden for practice: tap to show',
                    style: TextStyle(fontSize: 15, fontStyle: FontStyle.italic, color: cs.outline)),
              )
            else
              Text(item.text, style: const TextStyle(fontSize: 17)),
            const SizedBox(height: 2),
            Row(mainAxisSize: MainAxisSize.min, children: [
              Text(_time(), style: Theme.of(context).textTheme.labelSmall),
              if (icon != null) ...[
                const SizedBox(width: 4),
                Icon(icon, size: 14, color: item.state == SendState.failed ? Colors.red : null),
              ],
              if (mine && item.state == SendState.failed) const Text(' not delivered', style: TextStyle(fontSize: 11)),
            ]),
          ],
        ),
      ),
    );
  }
}
