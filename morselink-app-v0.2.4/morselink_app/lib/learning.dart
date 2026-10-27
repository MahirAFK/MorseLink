// MorseLink - the learning view (app v0.2.2, firmware v0.6.2, phone
// protocol 4): Morse as it is keyed and as it is felt.
//
// What the board tells the app (the firmware's main/phone.h):
//   KY <where> <pattern> <ms> <boundary>   a press was released and counted:
//                                         the letter keyed so far (where =
//                                         key), or the whole PARIS attempt so
//                                         far (where = cal); its length and
//                                         the dot / dash boundary it was
//                                         judged by
//   KY                                    nothing is open any more
//   KC <kind> <pattern> [<char>]          the board closed a letter: char /
//                                         unknown / error (the error prosign)
//   CB ok | CB retry <why>                a PARIS attempt was judged
//   SP <15 fields>                        the keying and reading speeds
//   PL <i> <offset> <unit> <fx10> <text>  a playback with Morse starts now
//   PL end                                ... it is over (or was stopped)
//
// This file is plain Dart (no Flutter): the model the screens draw from
// (lib/ui/learn_widgets.dart), and what test/widget_test.dart checks.

/// Morse code, exactly as the board's table (main/morse.c) has it.
const Map<String, String> morseCode = {
  'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.', //
  'F': '..-.', 'G': '--.', 'H': '....', 'I': '..', 'J': '.---',
  'K': '-.-', 'L': '.-..', 'M': '--', 'N': '-.', 'O': '---',
  'P': '.--.', 'Q': '--.-', 'R': '.-.', 'S': '...', 'T': '-',
  'U': '..-', 'V': '...-', 'W': '.--', 'X': '-..-', 'Y': '-.--',
  'Z': '--..', '0': '-----', '1': '.----', '2': '..---', '3': '...--',
  '4': '....-', '5': '.....', '6': '-....', '7': '--...', '8': '---..',
  '9': '----.', '.': '.-.-.-', ',': '--..--', '?': '..--..', "'": '.----.',
  '!': '-.-.--', '/': '-..-.', '(': '-.--.', ')': '-.--.-', '&': '.-...',
  ':': '---...', ';': '-.-.-.', '=': '-...-', '+': '.-.-.', '-': '-....-',
  '_': '..--.-', '"': '.-..-.', r'$': '...-..-', '@': '.--.-.',
};

/// A character's Morse, or null if it has none.
String? morseExact(String c) => morseCode[c.toUpperCase()];

/// A character's Morse as the motors play it: one without Morse plays as
/// '?' (the firmware's hseq.c does the same).
String morsePlayed(String c) => morseExact(c) ?? morseCode['?']!;

/// PARIS, the calibration word: its letters, and its 14 presses in a row.
const String parisWord = 'PARIS';
const String parisPresses = '.--..-.-......'; // 14 (a test checks it against the table)

/// Dots and dashes for the screen: a middle dot and a minus sign read better
/// than '.' and '-' at a glance.
String morseGlyphs(String pattern) => pattern.replaceAll('.', '·').replaceAll('-', '−');

/// One press, as a KY line reported it.
class KeyPress {
  final String where; // key / cal
  final String pattern; // the letter so far (key) or the attempt so far (cal), this press last
  final int ms; // how long it was held
  final int boundary; // presses longer than this are dashes (ms)
  final DateTime at;

  KeyPress(this.where, this.pattern, this.ms, this.boundary, this.at);

  /// Was this press a dash? (The last element of the pattern is this press.)
  bool get dash => pattern.endsWith('-');

  /// Within 15 % of the boundary: it could easily have gone the other way.
  bool get close => boundary > 0 && (ms - boundary).abs() < boundary * 0.15;
}

/// A letter the board closed (KC).
class KeyedLetter {
  final String kind; // char / unknown / error
  final String pattern; // what was keyed
  final String char; // the letter ('?' for unknown, '' for error)
  final DateTime at;

  KeyedLetter(this.kind, this.pattern, this.char, this.at);
}

/// The speeds an SP line reports: the person's keying as the calibration has
/// learned it, the decoder's thresholds, the reading speed and the vibration
/// strength. All times in ms.
class Speeds {
  final bool valid; // calibrated (else the timings are the board's starting guesses)
  final bool calibrating; // PARIS is being asked for now
  final int dot, dash; // the average dot and dash, as keyed
  final int gapIn, gapLetter; // the average gap inside a letter, and between letters
  final int boundary; // presses longer than this are dashes
  final int letter; // this much silence ends a letter
  final int word; // ... and this much adds a word space
  final String health; // none / healthy / marginal / collapsed
  final int q, unit, fx10; // reading speed: Q digit, unit (one dot), Farnsworth x10
  final int strength, strengthMv; // vibration strength: I level, motor supply (0 = unknown)
  final int dashStyle; // how dashes feel (H, firmware v0.6.3): 1 = sweep M1 -> M2, 2 = M2 only (1 if not reported)

  const Speeds({
    required this.valid,
    required this.calibrating,
    required this.dot,
    required this.dash,
    required this.gapIn,
    required this.gapLetter,
    required this.boundary,
    required this.letter,
    required this.word,
    required this.health,
    required this.q,
    required this.unit,
    required this.fx10,
    required this.strength,
    required this.strengthMv,
    this.dashStyle = 1,
  });

  /// Read an SP line's fields (after "SP "); null if it is short.
  static Speeds? parse(String rest) {
    final f = rest.trim().split(' ');
    if (f.length < 15) return null;
    int n(int i) => int.tryParse(f[i]) ?? 0;
    return Speeds(
      valid: f[0] == '1',
      calibrating: f[1] == '1',
      dot: n(2),
      dash: n(3),
      gapIn: n(4),
      gapLetter: n(5),
      boundary: n(6),
      letter: n(7),
      word: n(8),
      health: f[9],
      q: n(10),
      unit: n(11),
      fx10: n(12),
      strength: n(13),
      strengthMv: n(14),
      dashStyle: f.length > 15 && f[15] == '2' ? 2 : 1, // (a 16th field since v0.6.3)
    );
  }

  /// How many times longer a dash is than a dot (3 in textbook Morse).
  double get ratio => dot > 0 ? dash / dot : 0;

  /// Words per minute as keyed: the standard word PARIS (10 dots, 4 dashes,
  /// 9 gaps inside letters, 4 between letters and a word gap) timed with this
  /// person's own averages. The board doesn't measure word gaps, so one is
  /// taken as 7/3 of their letter gap (as in textbook timing).
  double get keyWpm {
    final t = 10 * dot + 4 * dash + 9 * gapIn + 4 * gapLetter + 7 * gapLetter / 3;
    return t > 0 ? 60000 / t : 0;
  }

  /// Words per minute as played (the reading speed): PARIS is 31 units of
  /// elements and gaps inside letters, and 19 units of gaps between letters
  /// and words, which the Farnsworth factor stretches.
  double get readWpm {
    final t = unit * (31 + 19 * fx10 / 10);
    return t > 0 ? 60000 / t : 0;
  }

  /// The calibration's health in plain words.
  String get healthText => switch (health) {
    'healthy' => 'steady',
    'marginal' => 'a little uneven',
    'collapsed' => 'uneven: C recalibrates',
    _ => 'not calibrated yet',
  };

  /// "I4 (3.3 V)", or "I4" without a power chip reading.
  /// How dashes feel, in words.
  String get dashText => dashStyle == 2 ? 'H2: on the forearm motor only' : 'H1: a sweep from wrist to forearm';

  String get strengthText => strengthMv > 0 ? 'I$strength (${(strengthMv / 1000).toStringAsFixed(1)} V)' : 'I$strength';
}

/// One character of a playback, with when each of its elements plays (ms
/// from the playback's start).
class PlayChar {
  final int index; // its position in the part's text
  final String char;
  final String pattern; // as played ('?' for a character without Morse)
  final int start; // its first element
  final int end; // the end of its last element
  final List<int> elStart; // each element's start
  final List<int> elLen; // ... and length

  const PlayChar(this.index, this.char, this.pattern, this.start, this.end, this.elStart, this.elLen);
}

/// Where each character of `text` plays, from t0 (ms), with the firmware's
/// timing (main/hseq.c, and the host tests' phone_model(), which this
/// mirrors line for line): spaces skipped at the start, dot 1U, dash 3U, 1U
/// inside a letter, 3U x fx10/10 between letters, 7U x fx10/10 between
/// words, integer arithmetic as on the board.
List<PlayChar> playTimeline(String text, int t0, int unit, int fx10) {
  final out = <PlayChar>[];
  var t = t0;
  var i = 0;
  final n = text.length;
  while (i < n && text[i] == ' ') {
    i++;
  }
  while (i < n) {
    final c = text[i];
    final pat = morsePlayed(c);
    final starts = <int>[];
    final lens = <int>[];
    final charStart = t;
    for (var e = 0; e < pat.length; e++) {
      final d = pat[e] == '-' ? 3 * unit : unit;
      starts.add(t);
      lens.add(d);
      t += d;
      if (e + 1 < pat.length) t += unit; // the gap inside the letter
    }
    out.add(PlayChar(i, c, pat, charStart, t, starts, lens));
    i++;
    var space = false;
    while (i < n && text[i] == ' ') {
      space = true;
      i++;
    }
    if (i < n) t += space ? 7 * unit * fx10 ~/ 10 : 3 * unit * fx10 ~/ 10;
  }
  return out;
}

/// One Morse part of a playback (a PL line): an ID, a message, PARIS...
class PlayPart {
  final int offset; // ms from the playback's start to its first element
  final int unit;
  final int fx10;
  final String text;
  late final List<PlayChar> chars = playTimeline(text, offset, unit, fx10);

  PlayPart(this.offset, this.unit, this.fx10, this.text);

  /// When its last element ends (ms from the playback's start).
  int get end => chars.isEmpty ? offset : chars.last.end;
}

/// Where a playback is at some moment.
class PlayPos {
  final int part; // which part
  final int char; // which character of it (an index into PlayPart.chars)
  final int el; // the element that started last
  final bool on; // ... and it is buzzing now (false: in a gap)

  const PlayPos(this.part, this.char, this.el, this.on);
}

/// How much the screen shows of what plays (for practising reading by feel).
enum ShowMode {
  all, // letters and their dots and dashes
  morse, // dots and dashes only; the letters once it has finished
  hidden, // nothing while it plays; "Show" afterwards
}

/// Everything the learning view knows, fed by the board's lines.
class Learning {
  // ---- keying ----
  String open = ''; // the letter being keyed (KY key)
  KeyPress? last; // the latest press (key or cal)
  KeyedLetter? lastLetter; // the latest letter the board closed (KC)
  KeyedLetter? _pending; // ... until the DR line that adds it to the draft
  String draft = '';
  List<String> draftPatterns = []; // one per draft character ('' for a space or one not known)

  // ---- calibration ----
  bool calibrating = false; // PARIS is being asked for (SP, or a KY cal)
  String calPattern = ''; // the attempt so far (KY cal)
  String calResult = ''; // '' / 'ok' / why it was refused (count, range, elements, gaps)
  int calTried = 0; // presses in the attempt that was judged
  DateTime? calResultAt;

  // ---- speeds ----
  Speeds? speeds;

  // ---- playback ----
  final List<PlayPart> parts = [];
  DateTime? playStart; // when PL 0 arrived
  bool playing = false; // between PL 0 and PL end
  ShowMode mode = ShowMode.all;
  bool revealed = false; // the last playback's text shown (morse / hidden modes)

  /// KY: a press, or (alone) nothing open any more.
  void onKY(String rest, DateTime now) {
    final f = rest.trim().split(' ');
    if (rest.trim().isEmpty || f.length < 4) {
      open = '';
      return;
    }
    final p = KeyPress(f[0], f[1], int.tryParse(f[2]) ?? 0, int.tryParse(f[3]) ?? 0, now);
    last = p;
    if (p.where == 'cal') {
      if (p.pattern.length == 1) {
        calResult = ''; // a new attempt
        calResultAt = null;
      }
      calibrating = true;
      calPattern = p.pattern;
    } else {
      open = p.pattern;
    }
  }

  /// KC: the board closed a letter. Its pattern goes above it once the DR
  /// line that adds it arrives (right after this one).
  void onKC(String rest, DateTime now) {
    final f = rest.trim().split(' ');
    if (f.length < 2) return;
    final k = KeyedLetter(f[0], f[1], f.length > 2 ? f[2] : '', now);
    lastLetter = k;
    _pending = (k.kind == 'char' || k.kind == 'unknown') ? k : null;
    open = '';
  }

  /// CB: the PARIS attempt was judged.
  void onCB(String rest, DateTime now) {
    final f = rest.trim().split(' ');
    calTried = calPattern.length;
    calResult = f[0] == 'ok' ? 'ok' : (f.length > 1 ? f[1] : 'other');
    calResultAt = now;
  }

  /// SP: the speeds. A calibration that starts clears the last attempt.
  void onSP(String rest) {
    final s = Speeds.parse(rest);
    if (s == null) return;
    if (s.calibrating && !calibrating) {
      calPattern = '';
      calResult = '';
      calResultAt = null;
    }
    calibrating = s.calibrating;
    speeds = s;
  }

  /// PL: a playback starts (part 0 starts a new one), or ends.
  void onPL(String rest, DateTime now) {
    final r = rest.trim();
    if (r == 'end') {
      playing = false;
      return;
    }
    final f = _split(r, 5);
    if (f.length < 5) return;
    final i = int.tryParse(f[0]) ?? 0;
    if (i == 0) {
      parts.clear();
      playStart = now;
      revealed = false;
    }
    parts.add(PlayPart(int.tryParse(f[1]) ?? 0, int.tryParse(f[2]) ?? 200, int.tryParse(f[3]) ?? 10, f[4]));
    playing = true;
  }

  /// DR: the draft changed. What it shares with the old draft keeps its
  /// patterns; a letter just keyed (the KC before) gets its pattern as
  /// keyed (that is how an unknown '?' shows what was really keyed); any
  /// other new character gets its Morse from the table.
  void onDraft(String d) {
    var p = 0;
    while (p < draft.length && p < d.length && draft[p] == d[p] && p < draftPatterns.length) {
      p++;
    }
    final pats = draftPatterns.sublist(0, p);
    for (var i = p; i < d.length; i++) {
      final c = d[i];
      final k = _pending;
      if (c == ' ') {
        pats.add('');
      } else if (k != null && i == d.length - 1 && k.char == c) {
        pats.add(k.pattern);
      } else {
        pats.add(c == '?' ? '' : (morseExact(c) ?? '')); // ('?' from a send that failed: what was keyed is unknown)
      }
    }
    _pending = null;
    draft = d;
    draftPatterns = pats;
  }

  /// The board went away: nothing is open or playing any more (the speeds
  /// and the last playback stay on screen).
  void boardGone() {
    open = '';
    _pending = null;
    playing = false;
  }

  /// Show the calibration helper: PARIS is being asked for, or an attempt
  /// was judged in the last 8 seconds.
  bool showCalibration(DateTime now) =>
      calibrating || (calResultAt != null && now.difference(calResultAt!).inMilliseconds < 8000);

  /// Milliseconds since the playback started (0 without one).
  int elapsedMs(DateTime now) => playStart == null ? 0 : now.difference(playStart!).inMilliseconds;

  /// When the playback's last element ends (ms from its start).
  int get playEndMs => parts.fold(0, (m, p) => p.end > m ? p.end : m);

  /// Is it playing at `now`? PL end says so; failing that (the line was
  /// lost), 1.5 s after the last element.
  bool isPlaying(DateTime now) => playing && elapsedMs(now) <= playEndMs + 1500;

  /// Where the playback is `ms` after its start: the character whose first
  /// element has started most recently, and in it the element; null before
  /// the first element (a tone, the attention signal) or without one.
  PlayPos? posAt(int ms) {
    for (var p = parts.length - 1; p >= 0; p--) {
      final chars = parts[p].chars;
      for (var c = chars.length - 1; c >= 0; c--) {
        final ch = chars[c];
        if (ch.start > ms) continue;
        var el = 0;
        for (var e = 0; e < ch.elStart.length; e++) {
          if (ch.elStart[e] <= ms) el = e;
        }
        final on = ms < ch.elStart[el] + ch.elLen[el];
        return PlayPos(p, c, el, on);
      }
    }
    return null;
  }

  /// Split into at most n fields, the last keeping the rest (spaces and all).
  static List<String> _split(String s, int n) {
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
}

/// Why a PARIS attempt was refused (a CB line's reason), in plain words;
/// `tried` = how many presses it had.
String calRetryText(String why, int tried) => switch (why) {
  'count' => 'PARIS has 14 presses; that attempt had $tried. Key it once more after the prompt.',
  'range' => 'A press was too short (under 20 ms) or too long (over 1.5 s).',
  'elements' => 'Your dots and dashes were too close in length: make each dash clearly longer (about 3 dots).',
  'gaps' => 'Pause longer between letters than inside them.',
  _ => 'Not accepted: key PARIS once more after the prompt.',
};
