#!/usr/bin/env python3
"""Reject direct errno reads after a potential fiber park in fipsd/host code.

usage: check-errno-after-park.py [--root DIR] [--self-test] [FILE ...]

Lexical, function-scoped guard, not a C control-flow or interprocedural proof.
All gof_* calls are conservative park boundaries except known non-parking
introspection/registration helpers; channel/select/blocking-executor waits count
as well. Loops, unsequenced expressions and mutually exclusive branches are conservative.
A justified read can carry an adjacent comment:
    /* errno-after-park: allow -- thread-only fallback; fiber branch returns */
Use fipsd_io_errno() for C errno after a park (raw Winsock errors instead use
libgo's gof_socket_errno()). Audit callers of parking wrappers separately.
"""
import argparse
from collections import namedtuple
import importlib.util
from pathlib import Path
import re
import sys

# Share the existing checker's comment/string masking and balanced C tokenizer.
_spec = importlib.util.spec_from_file_location(
    "unsequenced_args", Path(__file__).with_name("check-unsequenced-args.py"))
_lex = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_lex)
Finding = namedtuple("Finding", "path line detail")
WAIT = {"go_channel_send", "go_channel_receive", "go_channel_send_with_context",
        "go_channel_receive_with_context", "go_select", "go_select_timeout",
        "go_blocking_submit", "go_wait_group_wait"}
NON_PARKING = {"gof_socket_errno", "gof_in_fiber", "gof_current", "gof_hook_current",
               "gof_hook_make_runnable", "gof_io_cancel_fd", "gof_io_reset_fd", "gof_io_wake_writers",
               "gof_set_name", "gof_spawn"}
COMMENTS = re.compile(r'''//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*' ''', re.X)
ALLOW = re.compile(r"errno-after-park:\s*allow\s*--\s*([A-Za-z0-9][^\n]*?)(?:\*/|$)")


def function_bodies(tokens):
    """Yield balanced top-level function bodies; ignore prototypes/initializers."""
    depth = 0
    for k, tok in enumerate(tokens.text):
        if tok == "{":
            if depth == 0 and k and tokens.text[k - 1] == ")" and tokens.match[k] > k:
                yield k, tokens.match[k]
            depth += 1
        elif tok == "}":
            depth -= 1


def check_text(text, rel="<text>"):
    t = _lex.Tokens(_lex.code_view(text))
    lines = text.splitlines()
    found = []
    for start, end in function_bodies(t):
        parks = [k for k in range(start, end) if t.is_call[k] and not t.unevaluated[k] and
                 (t.text[k - 1] in WAIT or
                  t.text[k - 1].startswith("gof_") and t.text[k - 1] not in NON_PARKING)]
        if not parks:
            continue
        # A read earlier in a loop can follow the park on its next iteration.
        loops = any(x in ("for", "while", "do") for x in t.text[start:end])
        for k in range(start, end):
            if t.text[k] != "errno" or t.unevaluated[k]:
                continue
            if t.text[k - 1] in (".", "->"):
                continue  # a member named errno is not TLS
            if t.text[k + 1] == "=":
                continue  # store only; compound assignment still reads
            if k < parks[0] and not loops and ";" in t.text[k:parks[0]]:
                continue  # earlier full expression; same-expression order is unsafe
            line = t.line(k)
            adjacent = "\n".join(lines[max(0, line - 2):line])
            # Only comments, not strings/code, authorize an exception.
            comments = (m.group() for m in COMMENTS.finditer(adjacent)
                        if m.group().startswith(("/*", "//")))
            if any(ALLOW.search(c) for c in comments):
                continue
            found.append(Finding(rel, line,
                f"direct errno read in a function that can park ({t.text[parks[0] - 1]}); "
                "use fipsd_io_errno() or justify with errno-after-park: allow -- reason"))
    return found


def check(root, files=None):
    root = Path(root)
    if files is None:
        # Include new/untracked platform backends too; never scan build/vendor trees.
        files = sorted(p.relative_to(root).as_posix()
                       for sub in ("fipsd/src", "libfips-core")
                       for p in (root / sub).rglob("*")
                       if p.suffix in (".c", ".h", ".m", ".mm") and p.is_file())
    return [f for rel in files for f in check_text(
        (root / rel).read_text(encoding="utf-8", errors="replace"), rel)]


def self_test():
    bad = [
        "int f(void) { gof_read(fd,b,n); return errno; }",
        "int f(void) { if(gof_recvmsg(fd,m,0,-1)<0) return errno==EINTR; }",
        "int f(void) { gof_sleep_ms(1); return errno; }",
        "int f(void) { go_channel_receive(c,&v); return errno; }",
        "int f(void) { go_select(c,2); return errno; }",
        "int f(void) { go_channel_receive_with_context(c,&v,ctx); return errno; }",
        "int f(void) { go_channel_send_with_context(c,v,ctx); return errno; }",
        "int f(void) { go_wait_group_wait(g); return errno; }",
        "int f(void) { gof_write(fd,b,n); errno += 1; }",
        "int f(void) { for (;;) { if(errno) break; gof_read(fd,b,n); } }",
        "int f(void) { gof_read(fd,b,n); } int g(void) { gof_accept(fd,0,0,-1); return errno; }",
        "int f(void) { gof_new_parking_api(); return errno; }",
        "int f(void) { gof_read(fd,b,n); /* errno-after-park: allow -- */ return errno; }",
        'int f(void) { gof_read(fd,b,n); puts("errno-after-park: allow -- fake"); return errno; }',
    ]
    bad += [
        "int f(void) { consume(errno, gof_read(fd,b,n)); }",
        "int f(void) { return errno + gof_read(fd,b,n); }",
    ]
    bad.append('int f(void) { gof_read(fd,b,n); puts("/* errno-after-park: allow -- fake */"); return errno; }')
    good = [
        "int f(void) { int e=errno; gof_read(fd,b,n); return e; }",
        "int f(void) { gof_read(fd,b,n); errno=EIO; return fipsd_io_errno(); }",
        "int f(void) { gof_read(fd,b,n); } int g(void) { return errno; }",
        "int f(void); int g(void) { return errno; }",
        'int f(void) { gof_read(fd,b,n); puts("errno"); /* errno */ return 0; }',
        "int f(void) { gof_io_reset_fd(fd); return errno; }",
        "int f(void) { gof_in_fiber(); return errno; }",
        "int f(void) { sizeof(gof_read(fd,b,n)); return errno; }",
        "int f(void) { gof_read(fd,b,n); return s->errno; }",
        "int f(void) { gof_read(fd,b,n); return sizeof(errno); }",
        "int f(void) { gof_read(fd,b,n); /* errno-after-park: allow -- thread-only fallback */ return errno; }",
        "int f(void) { gof_read(fd,b,n);\n // errno-after-park: allow -- thread-only fallback\n return errno; }",
    ]
    for code in bad:
        assert check_text(code), f"missed: {code}"
    for code in good:
        assert not check_text(code), f"false positive: {code}"
    print(f"check-errno-after-park self-test passed ({len(bad)} bad, {len(good)} good)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("files", nargs="*")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    found = check(args.root, args.files or None)
    for f in found:
        print(f"{f.path}:{f.line}: {f.detail}")
    if found:
        print(f"{len(found)} errno read(s) after potential fiber park", file=sys.stderr)
        return 1
    print("No direct errno reads after potential fiber parks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
