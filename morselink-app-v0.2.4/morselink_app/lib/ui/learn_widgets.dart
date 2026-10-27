// MorseLink - the learning view's pieces (app v0.2.2, firmware v0.6.2): the
// letters with their dots and dashes above them and the letter being keyed,
// the last press against the dot / dash boundary, the keying and reading
// speeds, the PARIS calibration helper, what is playing on the arm, and a
// Morse chart. The main screen uses them small; the Learn page large.
// Everything they show comes from AppState.learn (lib/learning.dart).

import 'dart:async';

import 'package:flutter/material.dart';

import '../app_state.dart';
import '../learning.dart';

const String _mono = 'monospace';

/// One letter with its Morse above it. `spans` (optional) replaces the plain
/// pattern, e.g. to colour the elements already played.
class MorseLetter extends StatelessWidget {
  final String char; // '' for none (a placeholder is shown)
  final String pattern; // '.-' (shown as dots and dashes); '' for none
  final double size; // the letter's font size
  final Color? color; // the letter's (and pattern's) colour
  final bool highlight; // a tinted background (the letter playing now)
  final List<InlineSpan>? spans;

  const MorseLetter({
    super.key,
    required this.char,
    required this.pattern,
    this.size = 26,
    this.color,
    this.highlight = false,
    this.spans,
  });

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final pSize = (size * 0.55).clamp(11.0, 22.0);
    final pStyle = TextStyle(fontFamily: _mono, fontSize: pSize, height: 1.1, color: color ?? cs.onSurfaceVariant);
    final s = spans;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 3, vertical: 2),
      decoration: highlight ? BoxDecoration(color: cs.primaryContainer, borderRadius: BorderRadius.circular(6)) : null,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          s != null
              ? Text.rich(TextSpan(style: pStyle, children: s))
              : Text(pattern.isEmpty ? ' ' : morseGlyphs(pattern), style: pStyle),
          Text(
            char.isEmpty ? ' ' : char,
            style: TextStyle(fontFamily: _mono, fontSize: size, height: 1.15, color: color),
          ),
        ],
      ),
    );
  }
}

/// The draft, letter by letter with each letter's Morse above it, then the
/// letter being keyed (its dots and dashes so far, in the accent colour).
/// `maxLetters` keeps the main screen's box small (the start is cut off).
class KeyedLetters extends StatelessWidget {
  final Learning learn;
  final String text; // the draft (or the command being keyed)
  final List<String>? patterns; // one per character (null: from the Morse table)
  final double size;
  final int? maxLetters;

  const KeyedLetters({
    super.key,
    required this.learn,
    required this.text,
    this.patterns,
    this.size = 26,
    this.maxLetters,
  });

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final pats = patterns;
    var from = 0;
    final cap = maxLetters;
    if (cap != null && text.length > cap) from = text.length - cap;
    final children = <Widget>[];
    if (from > 0) {
      children.add(MorseLetter(char: '…', pattern: '', size: size, color: cs.outline));
    }
    for (var i = from; i < text.length; i++) {
      final c = text[i];
      if (c == ' ') {
        children.add(SizedBox(width: size * 0.6)); // a word space
        continue;
      }
      final p = (pats != null && i < pats.length) ? pats[i] : (morseExact(c) ?? '');
      children.add(MorseLetter(char: c, pattern: p, size: size));
    }
    if (learn.open.isNotEmpty) {
      children.add(MorseLetter(char: '_', pattern: learn.open, size: size, color: cs.primary));
    }
    if (children.isEmpty) {
      return Text(
        '-',
        style: TextStyle(fontFamily: _mono, fontSize: size, color: cs.outline),
      );
    }
    return Wrap(spacing: 2, runSpacing: 6, crossAxisAlignment: WrapCrossAlignment.end, children: children);
  }
}

/// What the last letter keyed turned into: "·− → A", "·−·−− → ? (not a
/// letter)", "8 dots: deleted the last letter".
class LastLetterLine extends StatelessWidget {
  final Learning learn;
  final double size;

  const LastLetterLine({super.key, required this.learn, this.size = 14});

  @override
  Widget build(BuildContext context) {
    final k = learn.lastLetter;
    if (k == null) return const SizedBox.shrink();
    final cs = Theme.of(context).colorScheme;
    final mono = TextStyle(fontFamily: _mono, fontSize: size + 2);
    final plain = TextStyle(fontSize: size, color: cs.onSurfaceVariant);
    final List<InlineSpan> spans = switch (k.kind) {
      'error' => [
        TextSpan(
          text: '${k.pattern.length} dots',
          style: plain.copyWith(fontWeight: FontWeight.bold),
        ),
        TextSpan(text: ': the error sign, it deleted the last letter', style: plain),
      ],
      'unknown' => [
        TextSpan(text: 'Last: ', style: plain),
        TextSpan(text: morseGlyphs(k.pattern), style: mono),
        TextSpan(
          text: '  →  ?',
          style: mono.copyWith(color: cs.error),
        ),
        TextSpan(text: '  (not a letter)', style: plain),
      ],
      _ => [
        TextSpan(text: 'Last: ', style: plain),
        TextSpan(text: morseGlyphs(k.pattern), style: mono),
        TextSpan(
          text: '  →  ${k.char}',
          style: mono.copyWith(fontWeight: FontWeight.bold),
        ),
      ],
    };
    return Text.rich(TextSpan(children: spans));
  }
}

/// The last press against the dot / dash boundary: the dot zone, the
/// boundary, the dash zone, the person's average dot and dash (ticks), and
/// where this press landed. Says when a press was close to the boundary.
class TimingBar extends StatelessWidget {
  final KeyPress press;
  final Speeds? speeds;
  final bool large;

  const TimingBar({super.key, required this.press, this.speeds, this.large = false});

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final small = Theme.of(context).textTheme.bodySmall;
    final s = speeds;
    final what = press.dash ? 'dash' : 'dot';
    final note = press.close ? '  (close to the boundary)' : '';
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        Text(
          'Last press: $what, ${press.ms} ms$note',
          style: (large ? Theme.of(context).textTheme.bodyMedium : small)?.copyWith(
            color: press.close ? cs.error : null,
          ),
        ),
        const SizedBox(height: 4),
        SizedBox(
          height: large ? 30 : 22,
          width: double.infinity,
          child: CustomPaint(
            painter: _TimingPainter(
              ms: press.ms,
              boundary: press.boundary,
              dot: (s != null && s.valid) ? s.dot : 0,
              dash: (s != null && s.valid) ? s.dash : 0,
              dotZone: cs.secondaryContainer,
              dashZone: cs.tertiaryContainer,
              line: cs.onSurface,
              tick: cs.outline,
              mark: cs.primary,
            ),
          ),
        ),
        const SizedBox(height: 2),
        Row(
          children: [
            Text('dot', style: small),
            const Spacer(),
            Text('boundary ${press.boundary} ms', style: small),
            const Spacer(),
            Text('dash', style: small),
          ],
        ),
      ],
    );
  }
}

/// Draws TimingBar's track (see there). The scale runs from 0 to a bit
/// more than the longest of: twice the boundary, the average dash, this press.
class _TimingPainter extends CustomPainter {
  final int ms, boundary, dot, dash;
  final Color dotZone, dashZone, line, tick, mark;

  _TimingPainter({
    required this.ms,
    required this.boundary,
    required this.dot,
    required this.dash,
    required this.dotZone,
    required this.dashZone,
    required this.line,
    required this.tick,
    required this.mark,
  });

  @override
  void paint(Canvas canvas, Size size) {
    var top = boundary * 2.2;
    if (dash * 1.3 > top) top = dash * 1.3;
    if (ms * 1.1 > top) top = ms * 1.1;
    if (top <= 0) top = 1;
    double x(num v) => (v / top * size.width).clamp(0.0, size.width);
    final h = size.height;
    final track = Rect.fromLTWH(0, h * 0.25, size.width, h * 0.5);
    final bx = x(boundary);
    canvas.drawRRect(
      RRect.fromRectAndCorners(
        Rect.fromLTRB(track.left, track.top, bx, track.bottom),
        topLeft: const Radius.circular(4),
        bottomLeft: const Radius.circular(4),
      ),
      Paint()..color = dotZone,
    );
    canvas.drawRRect(
      RRect.fromRectAndCorners(
        Rect.fromLTRB(bx, track.top, track.right, track.bottom),
        topRight: const Radius.circular(4),
        bottomRight: const Radius.circular(4),
      ),
      Paint()..color = dashZone,
    );
    // the boundary
    canvas.drawLine(
      Offset(bx, 0),
      Offset(bx, h),
      Paint()
        ..color = line
        ..strokeWidth = 1.5,
    );
    // the person's average dot and dash
    final tp = Paint()
      ..color = tick
      ..strokeWidth = 2;
    for (final v in [dot, dash]) {
      if (v > 0) canvas.drawLine(Offset(x(v), track.top - 3), Offset(x(v), track.bottom + 3), tp);
    }
    // this press
    canvas.drawCircle(Offset(x(ms), h / 2), h * 0.28, Paint()..color = mark);
  }

  @override
  bool shouldRepaint(_TimingPainter o) =>
      o.ms != ms || o.boundary != boundary || o.dot != dot || o.dash != dash || o.mark != mark;
}

/// The keying box's body (not in command mode): the letters with their
/// Morse, the letter being keyed, what the last letter became, and the last
/// press's timing.
class KeyingView extends StatelessWidget {
  final AppState app;
  final bool large;

  const KeyingView({super.key, required this.app, this.large = false});

  @override
  Widget build(BuildContext context) {
    final l = app.learn;
    final p = l.last;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        KeyedLetters(
          learn: l,
          text: app.draft,
          patterns: l.draft == app.draft ? l.draftPatterns : null,
          size: large ? 34 : 24,
          maxLetters: large ? null : 14,
        ),
        if (l.lastLetter != null) ...[const SizedBox(height: 6), LastLetterLine(learn: l, size: large ? 15 : 13)],
        if (p != null && p.where == 'key') ...[
          const SizedBox(height: 8),
          TimingBar(press: p, speeds: l.speeds, large: large),
        ],
      ],
    );
  }
}

/// One line for the main screen: the keying speed, dot and dash, how
/// steady, and the reading speed.
class SpeedsLine extends StatelessWidget {
  final Speeds? speeds;

  const SpeedsLine({super.key, required this.speeds});

  @override
  Widget build(BuildContext context) {
    final s = speeds;
    final small = Theme.of(context).textTheme.bodySmall;
    if (s == null) return const SizedBox.shrink();
    final read = 'reading Q${s.q} ≈ ${s.readWpm.toStringAsFixed(0)} wpm';
    if (!s.valid) return Text('Keying: not calibrated yet · $read', style: small);
    return Text(
      'Keying ≈ ${s.keyWpm.toStringAsFixed(1)} wpm · dot ${s.dot} / dash ${s.dash} ms '
      '· ${s.healthText} · $read',
      style: small,
    );
  }
}

/// The speeds, in full: the person's dot, dash and speed as the
/// calibration has learned them, their gaps against the decoder's
/// thresholds, how steady the keying is, the reading speed and the
/// vibration strength.
class SpeedsCard extends StatelessWidget {
  final Speeds? speeds;

  const SpeedsCard({super.key, required this.speeds});

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;
    final s = speeds;
    Widget metric(String label, String value) => Expanded(
      child: Container(
        padding: const EdgeInsets.all(10),
        decoration: BoxDecoration(color: cs.surfaceContainerHighest, borderRadius: BorderRadius.circular(8)),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(label, style: tt.labelSmall),
            const SizedBox(height: 2),
            Text(value, style: tt.titleMedium),
          ],
        ),
      ),
    );
    final children = <Widget>[Text('Your timing', style: tt.titleSmall), const SizedBox(height: 8)];
    if (s == null) {
      children.add(Text('Waiting for the board (firmware v0.6.2 reports this).', style: tt.bodySmall));
    } else if (!s.valid) {
      children.add(
        Text('Not calibrated yet: key PARIS back when the board asks (C recalibrates).', style: tt.bodyMedium),
      );
    } else {
      children.addAll([
        Row(
          children: [
            metric('Dot', '${s.dot} ms'),
            const SizedBox(width: 8),
            metric('Dash', '${s.dash} ms'),
            const SizedBox(width: 8),
            metric('Speed', '≈ ${s.keyWpm.toStringAsFixed(1)} wpm'),
          ],
        ),
        const SizedBox(height: 8),
        Text(
          'Dashes are ${s.ratio.toStringAsFixed(1)} × your dots (textbook: 3). '
          'Presses over ${s.boundary} ms count as dashes.',
        ),
        const SizedBox(height: 2),
        Text(
          'Pause inside a letter ≈ ${s.gapIn} ms, between letters ≈ ${s.gapLetter} ms. '
          'A pause over ${s.letter} ms ends a letter; over ${s.word} ms adds a space.',
        ),
        const SizedBox(height: 2),
        Text(
          'Keying: ${s.healthText}.',
          style: TextStyle(color: s.health == 'collapsed' ? cs.error : null, fontWeight: FontWeight.w500),
        ),
      ]);
    }
    if (s != null) {
      final spacing = s.fx10 > 10 ? ', letters spaced ×${(s.fx10 / 10).toStringAsFixed(1)}' : '';
      children.addAll([
        const Divider(height: 18),
        Text(
          'Reading: Q${s.q}, dot ${s.unit} ms$spacing ≈ ${s.readWpm.toStringAsFixed(1)} wpm '
          '(Q1 slowest, Q9 fastest).',
        ),
        const SizedBox(height: 2),
        Text('Vibration strength: ${s.strengthText} (I1 weakest, I8 normal, I9 strongest).'),
        const SizedBox(height: 2),
        Text('Dashes: ${s.dashText}. Dots: on the wrist motor.'),
      ]);
    }
    return Card(
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: children),
      ),
    );
  }
}

/// PARIS, letter by letter with its Morse, and the attempt so far under
/// it: each press green where it matches, red where it doesn't, grey still
/// to come; then the board's verdict.
class CalibrationCard extends StatelessWidget {
  final Learning learn;

  const CalibrationCard({super.key, required this.learn});

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;
    final keyed = learn.calPattern;
    final targets = parisWord.split('').map((c) => morseExact(c)!).toList();
    final rowsTarget = <Widget>[];
    final rowsKeyed = <Widget>[];
    var k = 0; // index into the presses keyed
    for (var i = 0; i < targets.length; i++) {
      final t = targets[i];
      rowsTarget.add(MorseLetter(char: parisWord[i], pattern: t, size: 26));
      final spans = <InlineSpan>[];
      for (var e = 0; e < t.length; e++, k++) {
        if (k < keyed.length) {
          final ok = keyed[k] == t[e];
          spans.add(
            TextSpan(
              text: morseGlyphs(keyed[k]),
              style: TextStyle(color: ok ? Colors.green.shade700 : cs.error, fontWeight: FontWeight.bold),
            ),
          );
        } else {
          spans.add(
            TextSpan(
              text: morseGlyphs(t[e]),
              style: TextStyle(color: cs.outlineVariant),
            ),
          );
        }
      }
      rowsKeyed.add(
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 3),
          child: Text.rich(
            TextSpan(
              style: const TextStyle(fontFamily: _mono, fontSize: 18),
              children: spans,
            ),
          ),
        ),
      );
    }
    final extra = keyed.length > parisPresses.length ? keyed.length - parisPresses.length : 0;
    final res = learn.calResult;
    Widget? verdict;
    if (res == 'ok') {
      verdict = Text(
        'Accepted: the board has learned your timing.',
        style: TextStyle(color: Colors.green.shade700, fontWeight: FontWeight.bold),
      );
    } else if (res.isNotEmpty) {
      verdict = Text(calRetryText(res, learn.calTried), style: TextStyle(color: cs.error));
    }
    return Card(
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('Calibration: key PARIS back', style: tt.titleSmall),
            const SizedBox(height: 4),
            Text('Feel it first, then key it at your own pace and stop for 3 s (or hold 2 s).', style: tt.bodySmall),
            const SizedBox(height: 8),
            Wrap(spacing: 6, children: rowsTarget),
            const SizedBox(height: 6),
            Row(
              children: [
                Text('You: ', style: tt.bodySmall),
                Expanded(child: Wrap(spacing: 6, children: rowsKeyed)),
              ],
            ),
            const SizedBox(height: 4),
            Text(
              '${keyed.length > 14 ? 14 : keyed.length} of 14 presses${extra > 0 ? ' (+$extra too many)' : ''}',
              style: tt.bodySmall,
            ),
            if (verdict != null) ...[const SizedBox(height: 6), verdict],
          ],
        ),
      ),
    );
  }
}

/// What is playing on the arm: each character with its Morse, the one
/// buzzing now highlighted and, inside it, the elements played so far.
/// The screen shows as much as the mode says (letters and Morse / Morse
/// only / nothing until "Show"). Redraws itself 25 times a second while
/// something plays; afterwards it keeps the last playback, dimmed.
class NowPlayingCard extends StatefulWidget {
  final AppState app;
  final bool large;
  final bool modes; // offer the show-mode chips

  const NowPlayingCard({super.key, required this.app, this.large = false, this.modes = false});

  @override
  State<NowPlayingCard> createState() => _NowPlayingCardState();
}

class _NowPlayingCardState extends State<NowPlayingCard> {
  Timer? _tick;
  bool _wasPlaying = false;

  @override
  void initState() {
    super.initState();
    _tick = Timer.periodic(const Duration(milliseconds: 40), (_) {
      final now = widget.app.learn.isPlaying(DateTime.now());
      if (!mounted) return;
      if (now || _wasPlaying) setState(() {}); // (one more redraw when it ends)
      _wasPlaying = now;
    });
  }

  @override
  void dispose() {
    _tick?.cancel();
    super.dispose();
  }

  /// The pattern of a character, coloured: elements already played in the
  /// letter colour, the one buzzing now bold in the accent, the rest faint.
  List<InlineSpan> _spans(PlayChar c, PlayPos? pos, int partIdx, int charIdx, ColorScheme cs) {
    final out = <InlineSpan>[];
    final p = pos; // (a local, so the null check below promotes it)
    for (var e = 0; e < c.pattern.length; e++) {
      var state = 0; // 0 still to come, 1 played, 2 buzzing now
      if (p != null) {
        final here = p.part == partIdx && p.char == charIdx;
        final past = p.part > partIdx || (p.part == partIdx && p.char > charIdx);
        if (past || (here && (e < p.el || (e == p.el && !p.on)))) {
          state = 1;
        } else if (here && e == p.el && p.on) {
          state = 2;
        }
      }
      final col = switch (state) {
        1 => cs.onSurface,
        2 => cs.primary,
        _ => cs.outlineVariant,
      };
      final bold = state == 2;
      out.add(
        TextSpan(
          text: morseGlyphs(c.pattern[e]),
          style: TextStyle(color: col, fontWeight: bold ? FontWeight.w900 : FontWeight.normal),
        ),
      );
    }
    return out;
  }

  @override
  Widget build(BuildContext context) {
    final app = widget.app;
    final l = app.learn;
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;
    final now = DateTime.now();
    final playing = l.isPlaying(now);
    final ms = l.elapsedMs(now);
    final pos = playing ? l.posAt(ms) : null;
    final size = widget.large ? 34.0 : 24.0;
    final showLetters = l.mode == ShowMode.all || l.revealed || (l.mode == ShowMode.morse && !playing);
    final showMorse = l.mode != ShowMode.hidden || l.revealed;

    final body = <Widget>[];
    if (l.parts.isEmpty) {
      body.add(Text('Messages, IDs and prompts appear here as they play on your arm.', style: tt.bodySmall));
    } else if (!showMorse) {
      // hidden: only how far it has got
      final total = l.parts.fold<int>(0, (n, p) => n + p.chars.length);
      var done = 0;
      final p = pos;
      if (p != null) {
        for (var i = 0; i < p.part; i++) {
          done += l.parts[i].chars.length;
        }
        done += p.char + 1;
      }
      body.add(
        Text(
          playing
              ? (p == null ? 'Starting…' : 'Playing: character $done of $total')
              : 'Played $total characters. What was it?',
          style: tt.bodyLarge,
        ),
      );
      if (!playing) {
        body.add(const SizedBox(height: 6));
        body.add(OutlinedButton(onPressed: app.revealPlayback, child: const Text('Show')));
      }
    } else {
      // Every character of every part, in order; the main screen (not
      // large) shows only a window of them around the one playing, so a
      // long message can't push the chat off the screen.
      final all = <(int, int)>[
        for (var pi = 0; pi < l.parts.length; pi++)
          for (var ci = 0; ci < l.parts[pi].chars.length; ci++) (pi, ci),
      ];
      var from = 0, to = all.length;
      if (!widget.large && all.length > 10) {
        final p = pos;
        final cur = p == null ? 0 : all.indexWhere((e) => e.$1 == p.part && e.$2 == p.char);
        from = (cur - 3).clamp(0, all.length - 10);
        to = from + 10;
      }
      final letters = <Widget>[];
      if (from > 0) letters.add(MorseLetter(char: '…', pattern: '', size: size, color: cs.outline));
      for (var k = from; k < to; k++) {
        final (pi, ci) = all[k];
        final c = l.parts[pi].chars[ci];
        if (k > from) {
          final (ppi, pci) = all[k - 1];
          final newPart = ppi != pi;
          if (newPart || l.parts[ppi].chars[pci].index + 1 < c.index) {
            letters.add(SizedBox(width: size * (newPart ? 0.9 : 0.6))); // a space (wider between parts)
          }
        }
        final here = pos != null && pos.part == pi && pos.char == ci;
        letters.add(
          MorseLetter(
            char: showLetters ? c.char : '_',
            pattern: c.pattern,
            size: size,
            highlight: here,
            color: playing ? null : cs.onSurfaceVariant,
            spans: playing ? _spans(c, pos, pi, ci, cs) : null,
          ),
        );
      }
      if (to < all.length) letters.add(MorseLetter(char: '…', pattern: '', size: size, color: cs.outline));
      body.add(Wrap(spacing: 2, runSpacing: 6, crossAxisAlignment: WrapCrossAlignment.end, children: letters));
      if (playing && pos == null) {
        body.add(
          Padding(
            padding: const EdgeInsets.only(top: 4),
            child: Text('(a signal first: the Morse follows)', style: tt.bodySmall),
          ),
        );
      }
      if (!playing && l.mode == ShowMode.morse && !l.revealed) {
        body.add(
          Padding(
            padding: const EdgeInsets.only(top: 4),
            child: Text('The letters, now it has played.', style: tt.bodySmall),
          ),
        );
      }
    }

    return Card(
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(playing ? Icons.vibration : Icons.history, size: 18, color: playing ? cs.primary : cs.outline),
                const SizedBox(width: 6),
                Text(playing ? 'Now playing on your arm' : 'Last played', style: tt.titleSmall),
              ],
            ),
            if (widget.modes) ...[
              const SizedBox(height: 6),
              Wrap(
                spacing: 6,
                children: [
                  for (final m in ShowMode.values)
                    ChoiceChip(
                      label: Text(switch (m) {
                        ShowMode.all => 'Letters + Morse',
                        ShowMode.morse => 'Morse only',
                        ShowMode.hidden => 'Hidden',
                      }),
                      selected: l.mode == m,
                      onSelected: (_) => app.setShowMode(m),
                    ),
                ],
              ),
            ],
            const SizedBox(height: 8),
            ...body,
          ],
        ),
      ),
    );
  }
}

/// A reference chart: A-Z and 0-9 with their Morse.
class MorseChart extends StatelessWidget {
  const MorseChart({super.key});

  @override
  Widget build(BuildContext context) {
    const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
    final tt = Theme.of(context).textTheme;
    return Wrap(
      spacing: 4,
      runSpacing: 4,
      children: [
        for (final c in chars.split(''))
          SizedBox(
            width: 78,
            child: Row(
              children: [
                SizedBox(
                  width: 18,
                  child: Text(c, style: tt.titleSmall?.copyWith(fontFamily: _mono)),
                ),
                Text(morseGlyphs(morseExact(c)!), style: const TextStyle(fontFamily: _mono, fontSize: 15)),
              ],
            ),
          ),
      ],
    );
  }
}
