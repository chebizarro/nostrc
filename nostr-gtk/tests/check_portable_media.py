#!/usr/bin/env python3
"""Portable nostr-gtk privacy lint (nostrc-8xfib.4).

Groundhog's check_privacy.py scans only gnome/groundhog/src, so the media
code it relies on in nostr-gtk (gn-*.c, linked into nostr_gtk_portable) is
checked here: no network or app plumbing, no remote URI handed to a media
backend, no gdk-pixbuf, and untrusted image bytes decoded only by
gn-media-decode.c (header-checked; the pixbuf fallback only on host opt-in).

Usage: check_portable_media.py SRC_DIR    (exit 1 on a violation)
       check_portable_media.py --self-test
"""
import pathlib
import re
import sys

RULES = [
    ("network", re.compile(r"\bsoup_|libsoup|\bGSocketClient\b|\bg_socket_client_"), None),
    ("app-settings", re.compile(r"\bg_settings_new\b"), None),
    ("gnostr-symbol", re.compile(r"\bgnostr_[a-z0-9_]+\s*\("), None),
    ("remote-uri-to-backend", re.compile(r"\bg_file_new_for_uri\b|\bgtk_media_file_new_for_filename\b"), None),
    ("no-gdk-pixbuf", re.compile(r"\bgdk_pixbuf_|gdk-pixbuf/"), None),
    ("unchecked-decode",
     re.compile(r"\bgdk_texture_new_from_bytes\b|\bgdk_texture_new_from_file\b|\bgdk_texture_new_from_filename\b"),
     {"gn-media-decode.c"}),
]

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
STRING = re.compile(r'"(?:\\.|[^"\\])*"')


def strip(text):
    text = COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return STRING.sub('""', text)


def check_text(name, text):
    found = []
    code = strip(text)
    for lineno, line in enumerate(code.splitlines(), 1):
        for rule, pattern, allowed in RULES:
            if allowed and name in allowed:
                continue
            if pattern.search(line):
                found.append(f"{name}:{lineno}: {rule}: {line.strip()}")
    return found


def check_dir(src):
    found = []
    for path in sorted(pathlib.Path(src).glob("gn-*.c")):
        found += check_text(path.name, path.read_text(encoding="utf-8"))
    return found


def self_test():
    bad = {
        "network": "void f(void) { soup_session_new(); }",
        "app-settings": "void f(void) { g_settings_new(\"x\"); }",
        "gnostr-symbol": "void f(void) { gnostr_is_remote_media_allowed(); }",
        "remote-uri-to-backend": "void f(const char *u) { g_file_new_for_uri(u); }",
        "no-gdk-pixbuf": "void f(void) { gdk_pixbuf_new_from_stream(0, 0, 0); }",
        "unchecked-decode": "void f(GBytes *b) { gdk_texture_new_from_bytes(b, 0); }",
    }
    for rule, code in bad.items():
        hits = check_text("gn-example.c", code)
        assert any(f": {rule}:" in h for h in hits), (rule, hits)
    assert not check_text("gn-media-decode.c", "void f(GBytes *b) { gdk_texture_new_from_bytes(b, 0); }")
    assert not check_text("gn-example.c", "/* soup_session_new() */ const char *s = \"g_settings_new\";")
    print("check_portable_media self-test: ok")
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    found = check_dir(argv[1])
    for line in found:
        print(line, file=sys.stderr)
    if not found:
        print("check_portable_media: ok")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
