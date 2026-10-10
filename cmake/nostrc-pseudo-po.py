#!/usr/bin/env python3
"""Generate a pseudo-locale catalog (e.g. en@pseudo) from a POT file.

Every translation is the msgid with ASCII letters swapped for accented look-
alikes and wrapped in "⟦…⟧", so untranslated strings, truncation and
concatenation stand out at runtime and the catalog exercises the UTF-8 codeset
binding. printf directives, markup tags, entities, {placeholders}, escapes and
mnemonic underscores are preserved so `msgfmt --check` accepts the result.
"""
import re
import sys

ACCENTS = str.maketrans(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "àƀçđéƒĝĥîĵķļɱñöþǫŕšţûṽŵẋýžÀßÇĐÉƑĜĤÎĴĶĻṀÑÖÞǪŔŠŢÛṼŴẊÝŽ")
KEEP = re.compile(
    r"%(?:\d+\$)?[-#0 +']*(?:\d+|\*)?(?:\.(?:\d+|\*))?"
    r"(?:hh|h|ll|l|L|q|j|z|t|I64|<PRI[a-zA-Z0-9_]+>)?[diouxXeEfFgGaAcspn%]"
    r"|<[^>]*>|&[A-Za-z#0-9]+;|\{[^}]*\}|\\.")
OPEN, CLOSE = "⟦", "⟧"


def pseudo(text):
    out, pos = [], 0
    for m in KEEP.finditer(text):
        out.append(text[pos:m.start()].translate(ACCENTS))
        out.append(m.group(0))
        pos = m.end()
    out.append(text[pos:].translate(ACCENTS))
    body = "".join(out)
    if not body:
        return body
    lead = re.match(r"^(?:\\n)*", body).group(0)
    trail = re.search(r"(?:\\n)*$", body[len(lead):]).group(0)
    core = body[len(lead):len(body) - len(trail)]
    return f"{lead}{OPEN}{core}{CLOSE}{trail}"


def parse(lines):
    """Yield POT entries as dicts of keyword -> escaped string, plus comments."""
    entry, key = {"comments": []}, None
    for raw in lines + [""]:
        line = raw.rstrip("\n")
        if not line.strip():
            if "msgid" in entry:
                yield entry
            entry, key = {"comments": []}, None
            continue
        if line.startswith("#"):
            entry["comments"].append(line)
            continue
        if line.startswith('"'):
            entry[key] += line[1:-1]
            continue
        word, _, rest = line.partition(" ")
        key = word
        entry[key] = rest.strip()[1:-1]


def quote(s):
    return f'"{s}"'


def main(pot_path, out_path, lang):
    with open(pot_path, encoding="utf-8") as f:
        entries = list(parse(f.readlines()))
    with open(out_path, "w", encoding="utf-8") as out:
        out.write("# Generated pseudo-locale catalog. Do not translate or edit.\n")
        out.write('msgid ""\nmsgstr ""\n')
        out.write('"Project-Id-Version: pseudo\\n"\n')
        out.write('"PO-Revision-Date: 2026-01-01 00:00+0000\\n"\n')
        out.write('"Last-Translator: generated\\n"\n')
        out.write('"Language-Team: none\\n"\n')
        out.write('"MIME-Version: 1.0\\n"\n')
        out.write('"Content-Type: text/plain; charset=UTF-8\\n"\n')
        out.write('"Content-Transfer-Encoding: 8bit\\n"\n')
        out.write(f'"Language: {lang}\\n"\n')
        out.write('"Plural-Forms: nplurals=2; plural=(n != 1);\\n"\n\n')
        for e in entries:
            if e.get("msgid") == "" and "msgctxt" not in e:
                continue  # POT header
            for c in e["comments"]:
                if c.startswith("#,"):
                    flags = [x.strip() for x in c[2:].split(",")
                             if x.strip() and x.strip() != "fuzzy"]
                    if flags:
                        out.write("#, " + ", ".join(flags) + "\n")
            if "msgctxt" in e:
                out.write(f"msgctxt {quote(e['msgctxt'])}\n")
            out.write(f"msgid {quote(e['msgid'])}\n")
            if "msgid_plural" in e:
                out.write(f"msgid_plural {quote(e['msgid_plural'])}\n")
                out.write(f"msgstr[0] {quote(pseudo(e['msgid']))}\n")
                out.write(f"msgstr[1] {quote(pseudo(e['msgid_plural']))}\n\n")
            else:
                out.write(f"msgstr {quote(pseudo(e['msgid']))}\n\n")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: nostrc-pseudo-po.py INPUT.pot OUTPUT.po LANG")
    main(*sys.argv[1:])
