#!/usr/bin/env python3
"""
MorseLink — the app's copy of the manual, from docs/MANUAL.md.

docs/MANUAL.md is the one source: this script embeds it, unchanged, in the
phone app as lib/manual.dart (the app's Manual page renders it), after two
checks that it still matches the firmware:

  - its Morse tables (section 13) list exactly the characters of
    main/morse.c, with the same codes;
  - its command table (section 4) names every letter that command mode
    accepts (cmd_valid() in main/ui.c), and no other.

    python tools/gen_manual.py [--app ../morselink_app] [--check]

--check writes nothing: it fails if the checks fail or the app's copy is
out of date (for a test run). Without --app, the app is looked for next to
this repository (../morselink_app).
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def firmware_morse():
    """{char: code} from main/morse.c's table."""
    src = open(os.path.join(ROOT, "main", "morse.c"), encoding="utf-8").read()
    table = src[src.index("s_table[] = {"):]
    table = table[:table.index("};")]
    out = {}
    for m in re.finditer(r"\{'(\\'|[^'])',\s*\"([.\-]+)\"\}", table):
        c = m.group(1).replace("\\'", "'")
        out[c] = m.group(2)
    return out


def manual_morse(md):
    """{char: code} from the manual's Morse tables: rows of
    | char | `code` | char | `code` |, in section 13."""
    sec = md[md.index("## 13. Morse code"):]
    sec = sec[:sec.index("\n## ", 5)]
    out = {}
    for line in sec.splitlines():
        if not line.startswith("|") or "`" not in line:
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        for i in range(0, len(cells) - 1, 2):
            ch, code = cells[i], cells[i + 1]
            m = re.fullmatch(r"`([.\-]+)`", code)
            if len(ch) == 1 and m:
                if ch in out:
                    raise SystemExit(f"MANUAL.md: {ch!r} is in the Morse tables twice")
                out[ch] = m.group(1)
    return out


def firmware_commands():
    """The command letters cmd_valid() in main/ui.c accepts."""
    src = open(os.path.join(ROOT, "main", "ui.c"), encoding="utf-8").read()
    body = src[src.index("static bool cmd_valid(char c, const char *arg)"):]
    body = body[:body.index("\n}\n")]
    return set(re.findall(r"case '([A-Z])':", body))


def manual_commands(md):
    """The letters in the first column of the manual's command table."""
    sec = md[md.index("## 4. Command mode"):]
    sec = sec[:sec.index("\n## ", 5)]
    out = set()
    for line in sec.splitlines():
        if line.startswith("| `"):
            first = line.strip().strip("|").split("|")[0]
            out |= set(re.findall(r"`([A-Z])`", first))
    return out


def dart_file(md):
    """lib/manual.dart: the manual as one raw string."""
    if "'''" in md:
        raise SystemExit("MANUAL.md contains ''' (it would end the Dart string)")
    return (
        "// MorseLink - the manual, for the app's Manual page (ui/manual_page.dart).\n"
        "//\n"
        "// GENERATED from the firmware's docs/MANUAL.md by tools/gen_manual.py: edit\n"
        "// that file and run the script, not this one. The script also checks the\n"
        "// Morse tables against main/morse.c and the commands against main/ui.c.\n"
        "\n"
        "const String manualMarkdown = r'''\n" + md + "''';\n"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", default=os.path.join(os.path.dirname(ROOT), "morselink_app"))
    ap.add_argument("--check", action="store_true", help="write nothing; fail if out of date")
    a = ap.parse_args()
    md = open(os.path.join(ROOT, "docs", "MANUAL.md"), encoding="utf-8").read()
    problems = []

    fw, man = firmware_morse(), manual_morse(md)
    for c in sorted(set(fw) | set(man)):
        if c not in man:
            problems.append(f"Morse: {c!r} ({fw[c]}) is missing from the manual")
        elif c not in fw:
            problems.append(f"Morse: {c!r} is in the manual but not in morse.c")
        elif fw[c] != man[c]:
            problems.append(f"Morse: {c!r} is {fw[c]} in morse.c but {man[c]} in the manual")

    fc, mc = firmware_commands(), manual_commands(md)
    for c in sorted(fc - mc):
        problems.append(f"command {c} is accepted by cmd_valid() but not in the manual's table")
    for c in sorted(mc - fc):
        problems.append(f"command {c} is in the manual's table but cmd_valid() doesn't accept it")

    for p in problems:
        print("  " + p)
    if problems:
        print(f"{len(problems)} problem(s): nothing written")
        return 1
    print(f"checks ok: {len(fw)} Morse characters, {len(fc)} commands")

    out = os.path.join(a.app, "lib", "manual.dart")
    want = dart_file(md)
    have = open(out, encoding="utf-8").read() if os.path.exists(out) else None
    if a.check:
        if have != want:
            print(f"{out} is out of date: run tools/gen_manual.py")
            return 1
        print(f"{out} is up to date")
        return 0
    if not os.path.isdir(os.path.dirname(out)):
        print(f"no app at {a.app} (--app PATH)")
        return 1
    if have == want:
        print(f"{out}: unchanged")
    else:
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(want)
        print(f"{out}: written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
