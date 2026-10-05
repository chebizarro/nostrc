#!/usr/bin/env python3
"""Require the narrow GTK skipped-frame tolerance in every GUI test main.

The policy is source-based so a newly added test is checked at configure time,
regardless of its CTest name or which component is enabled in that build.
Compiling each test verifies that its CMake target exposes tests/common.
"""

import argparse
import re
from pathlib import Path

GUI_INIT = re.compile(r"\b(?:gtk_test_init|gtk_init|gtk_init_check|adw_init)\s*\(")
TEST_INIT = re.compile(r"\b(?:g_test_init|gtk_test_init)\s*\(")
INCLUDE = re.compile(r'^\s*#\s*include\s+"nostrc-test-gdk-frame\.h"\s*$', re.M)
TOLERATE = re.compile(r"\bnostrc_test_tolerate_gdk_frame_warning\s*\(\s*\)\s*;")
def c_tokens(text, blank_strings=True):
    """Blank C comments and optionally quoted literals, preserving offsets."""
    out = []
    state = "code"
    index = 0
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                out.extend("  ")
                index += 2
                state = "line"
                continue
            if char == "/" and next_char == "*":
                out.extend("  ")
                index += 2
                state = "block"
                continue
            if char in ("\"", "'"):
                state = "string" if char == "\"" else "char"
                out.append(" " if blank_strings else char)
            else:
                out.append(char)
        elif state == "line":
            out.append("\n" if char == "\n" else " ")
            if char == "\n":
                state = "code"
        elif state == "block":
            if char == "*" and next_char == "/":
                out.extend("  ")
                index += 2
                state = "code"
                continue
            out.append("\n" if char == "\n" else " ")
        else:
            if char == "\\" and next_char:
                out.extend((" " if blank_strings else char,
                            "\n" if next_char == "\n" else
                            (" " if blank_strings else next_char)))
                index += 2
                continue
            out.append("\n" if char == "\n" else (" " if blank_strings else char))
            if char == ("\"" if state == "string" else "'"):
                state = "code"
        index += 1
    return "".join(out)


def violations(text):
    code = c_tokens(text)
    if not GUI_INIT.search(code):
        return []
    errors = []
    if not INCLUDE.search(c_tokens(text, blank_strings=False)):
        errors.append('missing #include "nostrc-test-gdk-frame.h"')
    init = TEST_INIT.search(code)
    call = TOLERATE.search(code)
    if not init:
        errors.append("GUI test has no g_test_init()/gtk_test_init()")
    if not call:
        errors.append("missing nostrc_test_tolerate_gdk_frame_warning()")
    elif init and call.start() < init.start():
        errors.append("tolerance call must follow test init")
    return errors


def self_test():
    good = '''#include "nostrc-test-gdk-frame.h"
int main(void) { gtk_test_init(NULL, NULL, NULL);
  nostrc_test_tolerate_gdk_frame_warning(); gtk_window_new(); }
'''
    assert not violations(good)
    assert violations(good.replace('#include "nostrc-test-gdk-frame.h"', ""))
    assert violations(good.replace('  nostrc_test_tolerate_gdk_frame_warning();', ""))
    assert violations(good.replace('gtk_test_init(NULL, NULL, NULL);', 'gtk_init();'))
    assert violations(good.replace('gtk_test_init(NULL, NULL, NULL);\n  nostrc_test_tolerate_gdk_frame_warning();',
                                   'nostrc_test_tolerate_gdk_frame_warning();\n  gtk_test_init(NULL, NULL, NULL);'))
    assert not violations('int main(void) { g_test_init(NULL, NULL, NULL); }')
    hidden = 'const char *url = "https://example.invalid"; gtk_test_init(NULL, NULL, NULL);'
    assert violations(hidden)
    commented_header = '/*\n#include "nostrc-test-gdk-frame.h"\n*/\n' + hidden
    assert violations(commented_header)
    helper = '''static void start_gui(void) { gtk_init(); }
int main(void) { g_test_init(0, 0, 0); start_gui(); }
'''
    assert violations(helper)
    helper_ok = '''#include "nostrc-test-gdk-frame.h"
static void start_gui(void) { gtk_init(); }
int main(void) {
  g_test_init(0, 0, 0);
  nostrc_test_tolerate_gdk_frame_warning();
  start_gui();
}
'''
    assert not violations(helper_ok)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    failures = []
    checked = 0
    for dirname in ('gnome/groundhog/tests', 'apps/gnostr/tests',
                    'nostr-gtk/tests', 'apps/grotto/tests',
                    'apps/gnostr/plugins'):
        for path in sorted((args.root / dirname).rglob('*.c')):
            if dirname == 'apps/gnostr/plugins' and 'tests' not in path.parts:
                continue
            source = path.read_text()
            if GUI_INIT.search(c_tokens(source)):
                checked += 1
            for error in violations(source):
                failures.append(f'{path.relative_to(args.root)}: {error}')
    if failures:
        print('\n'.join(failures))
        return 1
    print(f'GUI GDK frame tolerance: {checked} test sources checked')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
