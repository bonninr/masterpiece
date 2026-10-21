#!/usr/bin/env python3
"""Keeps the translation files in step with the program's text.

Every string a player reads goes through TRANS("...") in the sources. This
script finds those strings and compares them with the language files in
resources/translations/.

  python tools/i18n.py            report what each language lacks
  python tools/i18n.py --write    write resources/translations/strings.txt
  python tools/i18n.py --check    as the report, and exit 1 if anything is
                                          missing, stale or inconsistent

strings.txt is the list translators start from: a copy of it, with the
language: and countries: lines filled in and the right-hand side of each line
translated, is a new language. It is made on demand and kept out of the tree
(.gitignore): the sources are the list, so two changes that each add a string
never collide on it. The program never reads it.

Placeholders are written %1, %2 ... in the English text. A translation must
use the same ones, in whatever order its language wants.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIRS = ["src", "apps"]
SOURCE_SUFFIXES = {".cpp", ".h", ".mm"}
TRANSLATIONS = ROOT / "resources" / "translations"
TEMPLATE = TRANSLATIONS / "strings.txt"

LITERAL = r'"(?:[^"\\\n]|\\.)*"'
CALL = re.compile(r"\bTRANS\s*\(\s*(" + LITERAL + r"(?:\s*" + LITERAL + r")*)\s*\)")
PIECE = re.compile(LITERAL)
PLACEHOLDER = re.compile(r"%\d")


def c_unescape(body):
    """The text a C++ string literal's contents stand for."""
    out = bytearray()
    i = 0
    simple = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "'": "'", "\\": "\\", "0": "\0"}
    while i < len(body):
        c = body[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        i += 1
        e = body[i]
        if e == "x":
            j = i + 1
            while j < len(body) and j < i + 3 and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(body[i + 1:j], 16))
            i = j
        elif e == "u":
            out += chr(int(body[i + 1:i + 5], 16)).encode("utf-8")
            i += 5
        elif e in simple:
            out += simple[e].encode("utf-8")
            i += 1
        else:
            raise ValueError("unsupported escape \\" + e)
    return out.decode("utf-8")


def file_escape(text):
    """The form a language file holds a string in (what JUCE reads back)."""
    return text.replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r")


def file_unescape(text):
    """The inverse, as JUCE's LocalisedStrings does it."""
    return (text.replace('\\"', '"').replace("\\'", "'").replace("\\t", "\t")
            .replace("\\r", "\r").replace("\\n", "\n"))


def strip_comments(text):
    """Blanks // and /* */ comments, leaving strings and line numbers alone."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            m = re.compile(LITERAL).match(text, i)
            if m:
                out.append(m.group(0))
                i = m.end()
                continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def extract():
    """{key: [source files]} in order of first appearance, and a problem list."""
    keys = {}
    problems = []
    files = []
    for d in SOURCE_DIRS:
        files += [p for p in sorted((ROOT / d).rglob("*")) if p.suffix in SOURCE_SUFFIXES]
    for path in files:
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for m in CALL.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            rel = path.relative_to(ROOT).as_posix()
            try:
                key = "".join(c_unescape(p[1:-1]) for p in PIECE.findall(m.group(1)))
            except ValueError as e:
                problems.append(f"{rel}:{line}: {e}")
                continue
            if not key.strip():
                problems.append(f"{rel}:{line}: empty TRANS string")
                continue
            if "\\" in key:
                problems.append(f"{rel}:{line}: a backslash cannot be held in a language file: {key!r}")
            keys.setdefault(key, [])
            if rel not in keys[key]:
                keys[key].append(rel)
    return keys, problems


def parse_language_file(path):
    """(header dict, {key: translation}, [problems]) as JUCE would read it."""
    header = {}
    table = {}
    problems = []
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if line.startswith('"'):
            m = re.fullmatch(r'"((?:[^"\\]|\\.)*)"\s*=\s*"((?:[^"\\]|\\.)*)"', line)
            if not m:
                problems.append(f"{path.name}:{n}: cannot read the line")
                continue
            key, value = file_unescape(m.group(1)), file_unescape(m.group(2))
            if key in table:
                problems.append(f"{path.name}:{n}: {key!r} appears twice")
            table[key] = value
        elif line.lower().startswith("language:"):
            header["language"] = line[9:].strip()
        elif line.lower().startswith("countries:"):
            header["countries"] = line[10:].strip()
    return header, table, problems


def template_text(keys):
    out = [
        "# Every string the program shows, in English. Copy this file to <code>.txt,",
        "# fill in language: and countries:, and put the translation on the right.",
        "# %1, %2 ... stand for numbers and names put in by the program; keep them,",
        "# in whatever order the language wants. Lines beginning with # are ignored.",
        "",
        "language: English",
        "countries: gb us",
        "",
    ]
    by_file = {}
    for key, files in keys.items():
        by_file.setdefault(files[0], []).append(key)
    for f in sorted(by_file):
        out.append("# " + f)
        for key in by_file[f]:
            out.append(f'"{file_escape(key)}" = "{file_escape(key)}"')
        out.append("")
    return "\n".join(out)


def main(argv):
    write = "--write" in argv
    check = "--check" in argv
    keys, problems = extract()
    ok = not problems

    for p in problems:
        print("problem:", p)

    text = template_text(keys)
    if write:
        TEMPLATE.write_bytes(text.encode("utf-8"))
        print(f"wrote {TEMPLATE.relative_to(ROOT)}: {len(keys)} strings")

    print(f"{len(keys)} strings in the sources")
    for path in sorted(TRANSLATIONS.glob("*.txt")):
        if path == TEMPLATE:
            continue
        header, table, file_problems = parse_language_file(path)
        for p in file_problems:
            print("problem:", p)
            ok = False
        missing = [k for k in keys if k not in table]
        stale = [k for k in table if k not in keys]
        empty = [k for k, v in table.items() if not v.strip()]
        bad = [k for k in table if k in keys and sorted(PLACEHOLDER.findall(k)) != sorted(PLACEHOLDER.findall(table[k]))]
        if not header.get("language"):
            print(f"{path.name}: no language: line")
            ok = False
        done = len(keys) - len(missing)
        print(f"{path.name} ({header.get('language', '?')}): {done}/{len(keys)} translated")
        for label, items in (("missing", missing), ("not in the sources", stale),
                             ("empty", empty), ("placeholders differ", bad)):
            if items:
                ok = False
                print(f"  {label}: {len(items)}")
                for k in items[:50]:
                    print("    " + repr(k))
                if len(items) > 50:
                    print(f"    ... and {len(items) - 50} more")
    if check and not ok:
        return 1
    return 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv[1:]))
