// MorseLink - the Learn page (app v0.2.2, firmware v0.6.2): everything a
// learner (or a test participant) needs on one screen, large: the PARIS
// calibration while the board asks for it, the letters as they are keyed
// with their dots and dashes above them and the last press's timing, what
// is playing on the arm (with how much of it to show), the keying and
// reading speeds, and a Morse chart. No radio or test details here.

import 'package:flutter/material.dart';

import '../app_state.dart';
import '../protocol.dart';
import 'learn_widgets.dart';
import 'manual_page.dart';

class LearnPage extends StatelessWidget {
  final AppState app;

  const LearnPage({super.key, required this.app});

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: app,
      builder: (context, _) {
        final cs = Theme.of(context).colorScheme;
        final tt = Theme.of(context).textTheme;
        final l = app.learn;
        final connected = app.bleState == 'connected' && app.accepted;
        final cmd = app.cmd;
        return Scaffold(
          appBar: AppBar(
            title: const Text('Learn Morse'),
            actions: [
              IconButton(
                icon: const Icon(Icons.help_outline),
                tooltip: 'Manual',
                onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => const ManualPage())),
              ),
            ],
          ),
          body: ListView(
            padding: const EdgeInsets.all(12),
            children: [
              if (!connected)
                _Note(
                  text: app.settings.deviceId.isEmpty
                      ? 'No board chosen yet: pair one from the main screen first.'
                      : 'The board is not connected (${app.bleState}).',
                  color: cs.error,
                ),
              if (connected && app.protocol > 0 && app.protocol < appProtocol)
                _Note(
                  text:
                      'This board runs firmware ${app.fw}: the live dots and dashes, the speeds and the '
                      'playback need v0.6.2.',
                  color: cs.error,
                ),
              if (l.showCalibration(DateTime.now())) ...[CalibrationCard(learn: l), const SizedBox(height: 12)],
              Card(
                margin: EdgeInsets.zero,
                child: Padding(
                  padding: const EdgeInsets.all(12),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(cmd != null ? 'Command mode' : 'You are keying', style: tt.titleSmall),
                      const SizedBox(height: 8),
                      if (cmd != null) ...[
                        // command mode: the command with its Morse, then what it does
                        KeyedLetters(learn: l, text: cmd, size: 34),
                        const SizedBox(height: 4),
                        Text(
                          cmd.isEmpty
                              ? 'Key a command letter, then hold 2 s (hold 3 s to leave)'
                              : (commandNames[cmd[0]] ?? 'not a command'),
                          style: tt.bodySmall,
                        ),
                      ] else
                        KeyingView(app: app, large: true),
                    ],
                  ),
                ),
              ),
              const SizedBox(height: 12),
              NowPlayingCard(app: app, large: true, modes: true),
              const SizedBox(height: 12),
              SpeedsCard(speeds: l.speeds),
              const SizedBox(height: 12),
              Card(
                margin: EdgeInsets.zero,
                child: ExpansionTile(
                  title: Text('Morse chart', style: tt.titleSmall),
                  childrenPadding: const EdgeInsets.fromLTRB(12, 0, 12, 12),
                  children: const [MorseChart()],
                ),
              ),
              const SizedBox(height: 8),
              Text(
                'Short press = dot, long press = dash. Hold 2 s to send, 3 s for command mode, 5 s to cancel. '
                'Eight dots delete the last letter. Q sets the reading speed, I the vibration strength.',
                style: tt.bodySmall,
              ),
            ],
          ),
        );
      },
    );
  }
}

/// A one-line notice at the top of the page.
class _Note extends StatelessWidget {
  final String text;
  final Color color;

  const _Note({required this.text, required this.color});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Text(text, style: TextStyle(color: color)),
    );
  }
}
