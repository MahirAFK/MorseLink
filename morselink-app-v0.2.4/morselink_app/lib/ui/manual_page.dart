// MorseLink - the manual (the firmware's docs/MANUAL.md, embedded as
// lib/manual.dart). Each section is a tile that opens and closes, so the
// page stays short on a phone; `open` names one to open at the start (the
// keying box's command help opens "Command mode").

import 'package:flutter/material.dart';

import '../manual.dart';
import '../markdown_lite.dart';

class ManualPage extends StatelessWidget {
  final String? open; // a word of the section title to open first

  const ManualPage({super.key, this.open});

  @override
  Widget build(BuildContext context) {
    final sections = parseMarkdown(manualMarkdown);
    final o = open;
    return Scaffold(
      appBar: AppBar(title: const Text('Manual')),
      body: ListView(
        padding: const EdgeInsets.only(bottom: 24),
        children: [
          // The part before the first section: the title and the intro.
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 4),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [for (final b in sections.first.blocks) _MdBlockView(block: b)],
            ),
          ),
          for (final s in sections.skip(1))
            ExpansionTile(
              title: Text(s.title, style: Theme.of(context).textTheme.titleMedium),
              initiallyExpanded: o != null && s.title.contains(o),
              childrenPadding: const EdgeInsets.fromLTRB(16, 0, 16, 12),
              expandedCrossAxisAlignment: CrossAxisAlignment.start,
              children: [for (final b in s.blocks) _MdBlockView(block: b)],
            ),
        ],
      ),
    );
  }
}

/// Inline text with its marks, as spans.
TextSpan _spans(BuildContext context, String text, {TextStyle? style}) {
  final cs = Theme.of(context).colorScheme;
  final code = TextStyle(fontFamily: 'monospace', fontWeight: FontWeight.w600, color: cs.primary);
  return TextSpan(style: style, children: [
    for (final r in inlineRuns(text))
      TextSpan(
        text: r.text,
        style: switch (r.mark) {
          MdMark.bold => const TextStyle(fontWeight: FontWeight.bold),
          MdMark.italic => const TextStyle(fontStyle: FontStyle.italic),
          MdMark.code => code,
          MdMark.plain => null,
        },
      ),
  ]);
}

/// One block: a heading, a paragraph, a list item, a note or a table.
class _MdBlockView extends StatelessWidget {
  final MdBlock block;

  const _MdBlockView({required this.block});

  @override
  Widget build(BuildContext context) {
    final tt = Theme.of(context).textTheme;
    final cs = Theme.of(context).colorScheme;
    final b = block;
    switch (b.kind) {
      case MdKind.title:
        return Padding(
          padding: const EdgeInsets.only(bottom: 8),
          child: Text(b.text, style: tt.headlineSmall),
        );
      case MdKind.subheading:
        return Padding(
          padding: const EdgeInsets.only(top: 10, bottom: 4),
          child: Text.rich(_spans(context, b.text), style: tt.titleSmall),
        );
      case MdKind.para:
        return Padding(
          padding: const EdgeInsets.only(top: 6),
          child: Text.rich(_spans(context, b.text), style: tt.bodyMedium),
        );
      case MdKind.bullet:
      case MdKind.number:
        return Padding(
          padding: const EdgeInsets.only(top: 4, left: 4),
          child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
            SizedBox(width: 22, child: Text(b.kind == MdKind.bullet ? '•' : b.marker, style: tt.bodyMedium)),
            Expanded(child: Text.rich(_spans(context, b.text), style: tt.bodyMedium)),
          ]),
        );
      case MdKind.note:
        return Container(
          margin: const EdgeInsets.only(top: 8),
          padding: const EdgeInsets.all(10),
          decoration: BoxDecoration(
            color: cs.tertiaryContainer,
            border: Border(left: BorderSide(color: cs.tertiary, width: 4)),
            borderRadius: BorderRadius.circular(4),
          ),
          child: Text.rich(_spans(context, b.text), style: tt.bodyMedium?.copyWith(color: cs.onTertiaryContainer)),
        );
      case MdKind.table:
        return Padding(padding: const EdgeInsets.only(top: 8), child: _table(context, b.rows));
    }
    return const SizedBox.shrink(); // (not reached: every kind is drawn above)
  }

  /// A table: the header row shaded. Up to three columns it fits the width
  /// (the last column widest); wider tables keep each cell on one line and
  /// scroll sideways.
  Widget _table(BuildContext context, List<List<String>> rows) {
    if (rows.isEmpty) return const SizedBox.shrink();
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;
    final cols = rows.map((r) => r.length).reduce((a, b) => a > b ? a : b);
    final wide = cols > 3;
    Widget cell(String text, bool head) => Padding(
          padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 4),
          child: Text.rich(
            _spans(context, text, style: head ? const TextStyle(fontWeight: FontWeight.bold) : null),
            style: tt.bodySmall,
            softWrap: !wide,
          ),
        );
    final table = Table(
      border: TableBorder.all(color: cs.outlineVariant, width: 0.5),
      defaultVerticalAlignment: TableCellVerticalAlignment.top,
      defaultColumnWidth: wide ? const IntrinsicColumnWidth() : const FlexColumnWidth(),
      columnWidths: wide ? null : {for (var i = 0; i < cols; i++) i: FlexColumnWidth(i == cols - 1 ? 2.0 : 1.0)},
      children: [
        for (var r = 0; r < rows.length; r++)
          TableRow(
            decoration: r == 0 ? BoxDecoration(color: cs.surfaceContainerHighest) : null,
            children: [for (var c = 0; c < cols; c++) cell(c < rows[r].length ? rows[r][c] : '', r == 0)],
          ),
      ],
    );
    return wide ? SingleChildScrollView(scrollDirection: Axis.horizontal, child: table) : table;
  }
}
