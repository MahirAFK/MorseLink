// MorseLink - quick checks of the line protocol, the learning view and the
// manual (run: flutter test). (Replaces the counter-app test that `flutter
// create` writes.)

import 'package:flutter_test/flutter_test.dart';
import 'package:morselink_app/bench.dart';
import 'package:morselink_app/learning.dart';
import 'package:morselink_app/manual.dart';
import 'package:morselink_app/markdown_lite.dart';
import 'package:morselink_app/protocol.dart';

void main() {
  test('device IDs from addresses', () {
    expect(addrToId(0xFFFFFF), 'ALL');
    expect(addrToId(0), '222');
    expect(addrToId(31 * 31 * 31 - 1), 'ZZZ');
  });

  test('test-mode lines', () {
    final t = TestLine.parse('T,5,1234,ACK,path=L,seq=3,rtt_ms=420,snr=9.5')!;
    expect(t.n, 5);
    expect(t.ms, 1234);
    expect(t.ev, 'ACK');
    expect(t.str('path'), 'L');
    expect(t.integer('seq'), 3);
    expect(t.number('snr'), 9.5);
    expect(TestLine.parse('HI K7Q 1 0.5.0'), isNull);
  });

  test('board IDs stay text', () {
    final t = TestLine.parse('T,9,5000,RX,type=MSG,src=234,dst=2E5,id=17,snr=-7.5')!;
    expect(t.fields['src'], '234');
    expect(t.fields['dst'], '2E5');
    expect(t.integer('id'), 17);
    expect(t.number('snr'), -7.5);
  });

  test('text fields keep their spaces', () {
    expect(splitN('3 K7Q HELLO THERE', 3), ['3', 'K7Q', 'HELLO THERE']);
    expect(splitN('', 2), ['']);
  });

  test('relay packets', () {
    expect(isBase64Packet('FwQAUqEAQ+xxYU9L'), isTrue);
    expect(isBase64Packet('not base64!'), isFalse);
    final p = PacketInfo.fromBase64('FwQAUqEAQ+xxYU9L')!;
    expect(p.type, 'MSG');
    expect(p.dst, 'M4X');
  });

  // ---- protocol 2 (firmware v0.6.0) ----

  test('the radio setting (RD)', () {
    final r = RadioInfo.parse('12 125 22')!;
    expect(r.sf, 12);
    expect(r.bwKhz, 125);
    expect(r.dbm, 22);
    expect(r.level, 4);
    expect(r.text, 'range level 4 (SF12), 22 dBm');
    expect(RadioInfo.parse('9 125'), isNull);
    expect(RadioInfo.parse('x 125 22'), isNull);
    expect(RadioInfo.fromJson({'cmd': 'radio', 'sf': 9, 'bw': 125, 'dbm': 20}), const RadioInfo(9, 125, 20));
    expect(RadioInfo.fromJson({'cmd': 'radio', 'board': false}), isNull);
    expect(RadioInfo.fromJson(null), isNull);
  });

  test('path states and events', () {
    expect(pathStateText('they-cant-hear', 'K7Q'), "one-way: K7Q can't hear you");
    expect(pathStateText('cant-hear-them', 'K7Q'), "one-way: you can't hear K7Q");
    expect(pathStateText('none', 'K7Q'), '-');
    expect(pathLevel('connected'), PathLevel.good);
    expect(pathLevel('lost'), PathLevel.bad);
    expect(pathLevel('checking'), PathLevel.neutral);
    expect(eventText('LOST', 'K7Q', 'no-receipt'), 'Link to K7Q lost: a message got no receipt');
    expect(eventText('LOST', 'K7Q'), 'Link to K7Q lost'); // protocol 1: no reason
    expect(eventText('LORA', 'K7Q', 'lost'), 'LoRa to K7Q: lost');
    expect(eventText('RELAY', 'K7Q', 'connected'), 'Relay to K7Q: back');
  });

  test('EV lines keep their detail', () {
    expect(splitN('LOST K7Q no-receipt', 3), ['LOST', 'K7Q', 'no-receipt']);
    expect(splitN('LINKED K7Q', 3), ['LINKED', 'K7Q']);
  });

  test('command mode', () {
    expect(commandNames['G'], contains('range level'));
    expect(commandEndText('done', 'Q9'), 'Q9: done');
    expect(commandEndText('error', 'Q0'), 'Q0: not understood');
    expect(commandEndText('timeout', ''), 'Command mode ended: 30 s untouched');
    expect(splitN('done', 2), ['done']); // CE without a command
  });

  test('test pace follows the SF', () {
    expect(testPaceText('baseline', 9), contains('every 20 s (40 min)'));
    expect(testPaceText('baseline', 12), startsWith('60 messages'));
    expect(testPaceText('dynamic', 11), contains('40-60 s'));
    expect(testPaceText('rundown-a', 9), '');
    expect(refusalText('lora-lost'), contains('range level'));
  });

  test('test lines of v0.6.0', () {
    final l = TestLine.parse('T,40,90000,LINK,state=lost,why=no-receipt,heard_ago_ms=31000')!;
    expect(l.str('why'), 'no-receipt');
    expect(l.integer('heard_ago_ms'), 31000);
    final p = TestLine.parse('T,41,90010,PATH,path=L,state=they-cant-hear,heard_s=31,they_s=-1')!;
    expect(p.str('path'), 'L');
    expect(p.integer('they_s'), -1);
  });

  // ---- protocol 3 (firmware v0.6.1) ----

  test('the point test: timetable as the firmware computes it', () {
    expect([for (final sf in planSfs) pointGapS(sf)], [15, 20, 25, 40]);
    expect([for (final sf in planSfs) pointStepS(sf)], [325, 430, 545, 860]); // the host test's numbers
    expect(planSeconds(defaultPlan), 1510); // the default sweep: 25 min 10 s
    expect(minutesText(1510), '25 min 10 s');
    expect(planText(defaultPlan), '9:17,9:22,12:22');
    expect(planText(defaultPlan, '/'), '9:17/9:22/12:22');
    expect(parsePlan('9:17/12:22'), [const PlanStep(9, 17), const PlanStep(12, 22)]);
    expect(parsePlan('9:17,13:22'), isNull);
    expect(parsePlan('9:1'), isNull);
    expect(testPaceText('point', 12), contains('every 40 s'));
  });

  test('point test lines', () {
    final st = TestLine.parse('T,3,10500,STEP,i=2,sf=12,dbm=22,n=20,step_ms=860000')!;
    expect(st.integer('i'), 2);
    expect(st.integer('step_ms'), 860000);
    final s0 = TestLine.parse('T,2,10400,START,session=20261021-101500,role=rover,test=point,paths=lora,seed=1,'
        'tx_dbm=22,sf=9,fw=0.6.1,own=DEA,peer=ABC,lb=0,plan=9:17/9:22/12:22,n=20,size=25')!;
    expect(s0.str('plan'), '9:17/9:22/12:22');
    expect(parsePlan(s0.str('plan')), defaultPlan);
    final g = TestLine.parse('T,9,20000,GEN,seq=3,size=25,step=1,late_ms=1200,lora=queued,relay=off')!;
    expect(g.integer('step'), 1);
    expect(g.integer('late_ms'), 1200);
  });

  test('the meter session: the same timetable as the board', () {
    expect(benchSteps.map((s) => s.id).join(' '), '1 1b 1c 2 3a 3b 3c 3d 4a 4b 4c 5a 5b 5c 5d 6');
    expect(benchTotalMs(22), 543800); // firmware: bench_power_ms(22), checked by its host test
    expect(benchTotalMs(20), 532700); // an SX1276 skips 4c
    final tt = benchTimetable(22);
    expect(tt.first.signalMs, benchSettleMs);
    expect(tt.first.startMs, benchSettleMs + 2300); // one buzz and the pause before step 1
    expect(benchAt(tt, 0).slot, isNull);
    expect(benchAt(tt, benchSettleMs + 100).signal, isTrue);
    final at3b = tt.firstWhere((s) => s.step.id == '3b');
    expect(benchAt(tt, at3b.startMs + 1000).slot!.step.id, '3b');
    expect(benchAt(tt, at3b.startMs + 1000).signal, isFalse);
    expect(benchAt(tt, 10 * 60 * 1000).slot!.step.id, '6'); // after the last step: off
  });

  test('W and the meter lines', () {
    expect(commandNames['W'], 'Bluetooth off / on');
    expect(eventText('BLE_OFF', 'DEA'), contains('(W)'));
    expect(splitN('start 543 22', 2), ['start', '543 22']);
    expect(splitN('wait-usb', 2), ['wait-usb']);
    expect(appProtocol, 4);
    expect(refusalText('unknown test'), contains('v0.6.1'));
  });

  // ---- the learning view (firmware v0.6.2, protocol 4; lib/learning.dart) ----

  test('the playback timeline matches the board (hseq.c)', () {
    // The element starts and lengths below were printed by the firmware's
    // own generator (main/hseq.c), for the same texts and settings.
    List<String> els(List<PlayChar> cs) =>
        [for (final c in cs) for (var e = 0; e < c.elStart.length; e++) '${c.elStart[e]}+${c.elLen[e]}'];
    expect(els(playTimeline('PARIS', 0, 200, 10)).join(' '),
        '0+200 400+600 1200+600 2000+200 2800+200 3200+600 4400+200 4800+600 5600+200 6400+200 6800+200 '
        '7600+200 8000+200 8400+200');
    expect(playTimeline('PARIS', 0, 200, 10).last.end, 8600);
    expect(els(playTimeline('A B', 0, 120, 20)).join(' '), '0+120 240+360 2280+360 2760+120 3000+120 3240+120');
    // leading / trailing spaces, a character without Morse (plays as '?'), Farnsworth 1.5
    final t = playTimeline(' E#T ', 0, 250, 15);
    expect(els(t).join(' '), '0+250 1375+250 1875+250 2375+750 3375+750 4375+250 4875+250 6250+750');
    expect(t.map((c) => c.index).toList(), [1, 2, 3]);
    expect(t[1].pattern, '..--..');
    expect(t.last.end, 7000);
    // a part starting later is the same, shifted
    expect(playTimeline('E', 1250, 200, 10).first.start, 1250);
  });

  test('PL lines and where the playback is', () {
    final l = Learning();
    final t0 = DateTime(2026, 10, 21, 12);
    l.onPL('0 1250 200 10 DEA', t0);
    l.onPL('1 6450 200 10 HI THERE', t0); // DEA ends at 5050, then a word gap (1400)
    expect(l.parts.length, 2);
    expect(l.parts[1].text, 'HI THERE');
    expect(l.playing, isTrue);
    expect(l.posAt(1000), isNull); // the attention tone: no Morse yet
    final p = l.posAt(1250 + 50)!; // D's first element (a dash) buzzing
    expect([p.part, p.char, p.el, p.on], [0, 0, 0, true]);
    final q = l.posAt(1250 + 650)!; // D: the gap after the dash
    expect([q.part, q.char, q.el, q.on], [0, 0, 0, false]);
    expect(l.posAt(5000)!.part, 0);
    expect(l.posAt(6450)!.part, 1);
    expect(l.parts[0].end, 5050);
    expect(l.isPlaying(t0.add(const Duration(seconds: 3))), isTrue);
    expect(l.isPlaying(t0.add(Duration(milliseconds: l.playEndMs + 2000))), isFalse); // PL end lost: it ends anyway
    l.onPL('end', t0);
    expect(l.playing, isFalse);
    l.onPL('0 0 120 10 E', t0); // a new playback replaces the last
    expect(l.parts.length, 1);
  });

  test('keying: KY, KC and the patterns above the draft', () {
    final l = Learning();
    final now = DateTime(2026, 10, 21, 12);
    l.onKY('key . 110 330', now);
    expect(l.open, '.');
    l.onKY('key .- 540 330', now);
    expect(l.open, '.-');
    expect(l.last!.dash, isTrue);
    expect(l.last!.close, isFalse);
    l.onKC('char .- A', now);
    expect(l.open, '');
    l.onDraft('A');
    expect(l.draftPatterns, ['.-']);
    // an unknown pattern shows what was really keyed above the '?'
    l.onKC('unknown .-.-- ?', now);
    l.onDraft('A?');
    expect(l.draftPatterns, ['.-', '.-.--']);
    // a word space, then a letter; the error sign deletes it again
    l.onDraft('A? ');
    l.onKC('char - T', now);
    l.onDraft('A? T');
    expect(l.draftPatterns, ['.-', '.-.--', '', '-']);
    l.onKC('error ........', now);
    expect(l.lastLetter!.kind, 'error');
    l.onDraft('A?');
    expect(l.draftPatterns, ['.-', '.-.--']);
    // text that comes back from a failed send, in front: patterns from the table
    l.onDraft('HI A?');
    expect(l.draftPatterns, ['....', '..', '', '.-', '']);
    l.onKY('', now);
    expect(l.open, '');
    // a press near the boundary
    l.onKY('key - 350 330', now);
    expect(l.last!.close, isTrue);
  });

  test('calibration: KY cal, CB, SP', () {
    final l = Learning();
    final now = DateTime(2026, 10, 21, 12);
    l.onSP('1 1 180 560 170 620 330 420 1300 healthy 5 200 10 4 3300');
    expect(l.calibrating, isTrue);
    l.onKY('cal . 120 300', now);
    l.onKY('cal .- 400 300', now);
    expect(l.calPattern, '.-');
    l.onCB('retry count', now);
    expect(l.calResult, 'count');
    expect(calRetryText(l.calResult, l.calTried), contains('had 2'));
    l.onKY('cal . 120 300', now); // a new attempt
    expect(l.calResult, '');
    l.onKY('cal $parisPresses 100 300', now);
    l.onCB('ok', now);
    expect(l.calResult, 'ok');
    expect(l.showCalibration(now.add(const Duration(seconds: 5))), isTrue);
    l.onSP('1 0 180 560 170 620 330 420 1300 healthy 5 200 10 4 3300');
    expect(l.calibrating, isFalse);
    expect(l.showCalibration(now.add(const Duration(seconds: 9))), isFalse);
  });

  test('speeds: SP and words per minute', () {
    final s = Speeds.parse('1 0 100 300 100 300 200 400 1000 healthy 5 200 10 4 3300')!;
    expect(s.valid, isTrue);
    expect(s.keyWpm, closeTo(12.0, 0.01)); // textbook timing at a 100 ms dot: 12 wpm
    expect(s.readWpm, closeTo(6.0, 0.01)); // Q5: 200 ms unit
    expect(s.ratio, 3.0);
    expect(s.healthText, 'steady');
    expect(s.strengthText, 'I4 (3.3 V)');
    final f = Speeds.parse('0 0 120 360 120 360 240 480 1200 none 3 250 20 2 0')!;
    expect(f.valid, isFalse);
    expect(f.readWpm, closeTo(60000 / (250 * 69), 0.01)); // Farnsworth x2 stretches the 19 gap units
    expect(f.strengthText, 'I2');
    expect(f.dashStyle, 1); // (15 fields: a v0.6.2 board)
    final h2 = Speeds.parse('1 0 100 300 100 300 200 400 1000 healthy 5 200 10 4 3300 2')!;
    expect(h2.dashStyle, 2);
    expect(h2.dashText, contains('forearm motor only'));
    expect(commandNames['H'], contains('forearm'));
    expect(commandNames['K'], contains('keep'));
    expect(commandNames['I'], contains('1-9'));
    expect(commandEndText('left', ''), contains('kept'));
    expect(Speeds.parse('1 0 100'), isNull);
  });

  test('the Morse table is the manual\'s (and the board\'s)', () {
    final s = parseMarkdown(manualMarkdown);
    final morse = s.firstWhere((x) => x.title == '13. Morse code');
    final pairs = <String, String>{};
    for (final b in morse.blocks.where((b) => b.kind == MdKind.table)) {
      for (final r in b.rows.skip(1)) {
        for (var i = 0; i + 1 < r.length; i += 2) {
          final c = r[i].replaceAll('`', '').trim();
          if (c.isNotEmpty) pairs[c] = r[i + 1].replaceAll('`', '').trim();
        }
      }
    }
    expect(morseCode.length, 54);
    for (final e in morseCode.entries) {
      if (pairs.containsKey(e.key)) expect(pairs[e.key], e.value, reason: e.key);
    }
    for (final c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'.split('')) {
      expect(pairs[c], morseCode[c], reason: c);
    }
    expect(morseGlyphs('.-'), '\u00B7\u2212');
    expect(parisPresses, parisWord.split('').map((c) => morseCode[c]).join());
    expect(parisPresses.length, 14);
    expect(commandNames['I'], contains('strength'));
  });

  // ---- the manual (docs/MANUAL.md) ----

  test('the manual parses into its sections', () {
    final s = parseMarkdown(manualMarkdown);
    expect(s.first.blocks.first.kind, MdKind.title);
    final titles = s.map((x) => x.title).toList();
    expect(titles, contains('4. Command mode'));
    expect(titles, contains('5. Range level (G)'));
    expect(titles, contains('13. Morse code'));
    // The command table names G, and the range level section starts with the note.
    final cmd = s.firstWhere((x) => x.title == '4. Command mode');
    final table = cmd.blocks.firstWhere((b) => b.kind == MdKind.table);
    expect(table.rows.first, ['Command', 'Does']);
    expect(table.rows.any((r) => r.first.startsWith('`G`')), isTrue);
    final g = s.firstWhere((x) => x.title == '5. Range level (G)');
    expect(g.blocks.first.kind, MdKind.note);
    expect(g.blocks.first.text, contains('same range level'));
    // Morse: three tables, 26 + 10 + 18 characters.
    final morse = s.firstWhere((x) => x.title == '13. Morse code');
    final cells = morse.blocks
        .where((b) => b.kind == MdKind.table)
        .expand((b) => b.rows.skip(1))
        .expand((r) => [for (var i = 0; i + 1 < r.length; i += 2) r[i]])
        .toList();
    expect(cells.length, 54);
  });

  test('inline marks', () {
    final r = inlineRuns('hold **2 s**, then `G4` (*ID*)');
    expect(r.map((x) => x.mark).toList(), [MdMark.plain, MdMark.bold, MdMark.plain, MdMark.code, MdMark.plain, MdMark.italic, MdMark.plain]);
    expect(r[1].text, '2 s');
    expect(r[3].text, 'G4');
    expect(r[5].text, 'ID');
  });
}
