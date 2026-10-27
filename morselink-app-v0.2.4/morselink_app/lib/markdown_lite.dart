// MorseLink - a small reader for the Markdown the manual uses (lib/manual.dart,
// from the firmware's docs/MANUAL.md): # / ## / ### headings, paragraphs,
// "- " and "1. " list items (continued on indented lines), | tables |
// (the |---| line skipped), "> " notes, and inline **bold**, *italic* and
// `code`. Anything else is a paragraph. Pure Dart (tested in
// test/widget_test.dart); ui/manual_page.dart draws it.

/// What a block is.
enum MdKind { title, subheading, para, bullet, number, table, note }

/// One block of a section.
class MdBlock {
  final MdKind kind;
  final String text; // everything but tables: the text, inline marks kept
  final String marker; // number: "1."
  final List<List<String>> rows; // table: the rows' cells, the header first

  const MdBlock(this.kind, {this.text = '', this.marker = '', this.rows = const []});
}

/// A "## " section: its heading and blocks ("" for the part before the first).
class MdSection {
  final String title;
  final List<MdBlock> blocks = [];

  MdSection(this.title);
}

final RegExp _numbered = RegExp(r'^(\d+)\. ');
final RegExp _ruleCell = RegExp(r'^:?-+:?$');

/// Does this line start a block of its own (so it ends a paragraph)?
bool _startsBlock(String l) =>
    l.startsWith('#') || l.startsWith('|') || l.startsWith('>') || l.startsWith('- ') || _numbered.hasMatch(l);

/// The cells of a table line "| a | b |".
List<String> _cells(String line) {
  var r = line.trim();
  if (r.startsWith('|')) r = r.substring(1);
  if (r.endsWith('|')) r = r.substring(0, r.length - 1);
  return r.split('|').map((c) => c.trim()).toList();
}

/// Split the manual into sections of blocks.
List<MdSection> parseMarkdown(String md) {
  final out = <MdSection>[MdSection('')];
  final lines = md.split('\n');
  var i = 0;
  while (i < lines.length) {
    final t = lines[i].trimRight();
    if (t.trim().isEmpty) {
      i++;
      continue;
    }
    if (t.startsWith('## ')) {
      out.add(MdSection(t.substring(3).trim()));
      i++;
      continue;
    }
    if (t.startsWith('# ')) {
      out.last.blocks.add(MdBlock(MdKind.title, text: t.substring(2).trim()));
      i++;
      continue;
    }
    if (t.startsWith('### ')) {
      out.last.blocks.add(MdBlock(MdKind.subheading, text: t.substring(4).trim()));
      i++;
      continue;
    }
    if (t.startsWith('|')) {
      final rows = <List<String>>[];
      while (i < lines.length && lines[i].trimRight().startsWith('|')) {
        final cells = _cells(lines[i]);
        if (!cells.every((c) => _ruleCell.hasMatch(c))) rows.add(cells);
        i++;
      }
      out.last.blocks.add(MdBlock(MdKind.table, rows: rows));
      continue;
    }
    if (t.startsWith('>')) {
      final buf = <String>[];
      while (i < lines.length && lines[i].trimRight().startsWith('>')) {
        buf.add(lines[i].trimRight().substring(1).trim());
        i++;
      }
      out.last.blocks.add(MdBlock(MdKind.note, text: buf.join(' ').trim()));
      continue;
    }
    final isBullet = t.startsWith('- ');
    final n = _numbered.firstMatch(t);
    if (isBullet || n != null) {
      final buf = <String>[(isBullet ? t.substring(2) : t.substring(n!.end)).trim()];
      i++;
      while (i < lines.length && lines[i].startsWith('  ') && lines[i].trim().isNotEmpty) {
        buf.add(lines[i].trim());
        i++;
      }
      out.last.blocks.add(MdBlock(isBullet ? MdKind.bullet : MdKind.number,
          text: buf.join(' '), marker: isBullet ? '' : '${n!.group(1)}.'));
      continue;
    }
    final buf = <String>[t.trim()];
    i++;
    while (i < lines.length && lines[i].trim().isNotEmpty && !_startsBlock(lines[i].trimRight())) {
      buf.add(lines[i].trim());
      i++;
    }
    out.last.blocks.add(MdBlock(MdKind.para, text: buf.join(' ')));
  }
  return out;
}

/// How a piece of inline text is marked.
enum MdMark { plain, bold, italic, code }

/// A piece of inline text and its mark.
class MdRun {
  final String text;
  final MdMark mark;

  const MdRun(this.text, this.mark);
}

final RegExp _inline = RegExp(r'\*\*(.+?)\*\*|\*([^*\s][^*]*?)\*|`([^`]+)`');

/// Split text at its **bold**, *italic* and `code` marks.
List<MdRun> inlineRuns(String text) {
  final out = <MdRun>[];
  var pos = 0;
  for (final m in _inline.allMatches(text)) {
    if (m.start > pos) out.add(MdRun(text.substring(pos, m.start), MdMark.plain));
    if (m.group(1) != null) {
      out.add(MdRun(m.group(1)!, MdMark.bold));
    } else if (m.group(2) != null) {
      out.add(MdRun(m.group(2)!, MdMark.italic));
    } else {
      out.add(MdRun(m.group(3)!, MdMark.code));
    }
    pos = m.end;
  }
  if (pos < text.length) out.add(MdRun(text.substring(pos), MdMark.plain));
  return out;
}
