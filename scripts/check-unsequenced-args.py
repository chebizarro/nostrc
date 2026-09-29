#!/usr/bin/env python3
"""Flag calls whose arguments both modify and use the same variable (nostrc-td3b).

usage: check-unsequenced-args.py [--root DIR] [--self-test] [FILE ...]

C leaves the evaluation order of a call's arguments unspecified. Apple clang
and GCC on arm64 evaluate them left to right; GCC on x86_64 goes right to
left. So a call that changes a variable in one argument and uses it in another
passes every local test and breaks on the hosted x86_64 runners, as twice in
nostrc-qp24.91:

    g_socket_address_enumerator_next_async (self->inner,
        g_task_get_cancellable (task), on_inner_next, g_steal_pointer (&task));
    copy = g_memdup2 (g_bytes_get_data (bytes, &size), size);

On x86_64 g_steal_pointer(&task) ran first and g_task_get_cancellable got NULL;
`size` was read as 0 before g_bytes_get_data stored it.

The rule: in any call `f(a1, ..., an)`, a variable path P (an identifier with
optional ->member, .member and [index] parts, e.g. task, self->task, buf[i])
is *modified* by argument ai when ai contains

  - &P inside a call nested in ai (g_steal_pointer(&P), g_clear_*(&P),
    g_bytes_get_data(b, &P), any out-parameter): the callee may write P while
    the other arguments are being evaluated. &P passed to f itself is not a
    modification: f runs after all its arguments;
  - P++, ++P, P--, --P, or an assignment P = / P += ... .

It is a finding when a sibling argument aj (j != i) reads P, or reads through
it (P->x, P[i]), or modifies it too. A sibling that reads a member of a struct
object P (P.x) is not: handing a helper the address of a fixture or context
struct while another argument reads one of its fields is the common, safe case
(`check (f.store, now (&f))`). The check therefore misses a struct out-parameter
whose field a sibling reads (`use (fill (&range), range.start)`). Nothing inside sizeof, _Alignof,
typeof/__typeof__, offsetof, G_N_ELEMENTS or G_STRUCT_OFFSET is evaluated, and
the arguments of the macros in SEQUENCED_MACROS are evaluated in order, so
neither counts. Only call argument lists are checked: the operands of &&, ||,
?:, the comma operator and for/if/while headers are sequenced.

This is a token-level check, not a compiler: it cannot see a const out-pointer
or a macro that sequences its arguments. Fix a finding by hoisting the side
effect into its own statement first (clearer anyway); if the call really is
safe, add (path, callee, variable) -> justification to EXCEPTIONS. An
exception that no longer matches, or has no justification, fails the check.

Files: every tracked *.c and *.h (git ls-files) outside the vendored trees in
VENDORED, or the FILE arguments.

--self-test runs the fixtures below (the historical bugs and near misses), then
checks the tree.
"""

import argparse
from collections import namedtuple
from pathlib import Path
import re
import subprocess
import sys

VENDORED = ("third_party/", "gnome/nostr-homed/third_party/", "libnostr/generated/")

# Arguments are evaluated in order: each is its own initialised declaration
# (GLib: "gint64 __n1 = (n1), __n2 = (n2);").
SEQUENCED_MACROS = re.compile(r"^g_assert_cmp\w+$")

# Nothing inside these is evaluated.
UNEVALUATED = {"sizeof", "_Alignof", "alignof", "__alignof__", "typeof", "__typeof__",
               "__typeof", "offsetof", "G_N_ELEMENTS", "G_STRUCT_OFFSET", "__builtin_offsetof",
               "_Generic_type"}
# Parentheses after these open no call.
KEYWORDS = {"if", "while", "for", "switch", "return", "do", "else", "case", "defined",
            "__attribute__", "__attribute", "__asm__", "asm", "__declspec", "_Static_assert",
            "static_assert", "_Generic", "__extension__"} | UNEVALUATED

# (path relative to the root, callee, variable) -> justification.
EXCEPTIONS = {
    ("libmarmot/tests/test_commits.c", "CHECK", "err"):
        "CHECK(cond, ...) is a macro that evaluates cond in an if and only then passes "
        "the message arguments to fprintf, so err is read after deliver(..., &err) stored it.",
    ("gnome/groundhog/tests/store/test_store_conversations.c", "gh_conversation_is_unread", "f"):
        "room(&f, id) only reads f->model (gh_conversation_store_lookup); both calls return "
        "the same conversation in either order.",
    ("libgo/examples/arrays_demo.c", "printf", "ia"):
        "int_array_get and int_array_size only read the array (array->size, array->data).",
    ("libgo/examples/arrays_demo.c", "printf", "sa"):
        "string_array_get and string_array_size only read the array (array->size, array->data).",
}

Finding = namedtuple("Finding", "path line callee variable detail")

TOKEN_RE = re.compile(
    r"[A-Za-z_]\w*|\d[\w.]*|->|\+\+|--|<<=|>>=|&&|\|\||<<|>>|[-+*/%&|^!=<>]=|\.\.\.|\S")
ASSIGN_OPS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}
# After one of these a & or * is unary.
OPERAND_END = re.compile(r"^(?:[A-Za-z_]\w*|\d[\w.]*|\)|\]|\+\+|--)$")
LEX_SPECIAL_RE = re.compile(r"//|/\*|[\"']")


def blank(text):
    return re.sub(r"[^\n]", " ", text)


def code_view(text):
    """`text` with comments and the contents of string and character literals
    blanked (offsets and line breaks kept), and preprocessor directives other
    than #define blanked (their parentheses are not calls)."""
    out, i, n = [], 0, len(text)
    while i < n:
        m = LEX_SPECIAL_RE.search(text, i)
        if not m:
            out.append(text[i:])
            break
        out.append(text[i:m.start()])
        i = m.start()
        token = m.group()
        if token == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(blank(text[i:j]))
        elif token == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(blank(text[i:j]))
        else:
            j = i + 1
            while j < n and text[j] != token and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(token + blank(text[i + 1:j - 1]) + (text[j - 1] if j - 1 > i else ""))
        i = j
    code = "".join(out)
    return re.sub(r"(?m)^[ \t]*#[ \t]*(?!define\b)\w+(?:[^\n]*\\\n)*[^\n]*",
                  lambda m: blank(m.group()), code)


class Tokens:
    def __init__(self, code):
        self.code = code
        matches = list(TOKEN_RE.finditer(code))
        self.text = [m.group() for m in matches]
        self.pos = [m.start() for m in matches]
        n = len(self.text)
        self.match = [-1] * n
        self.is_call = [False] * n
        self.inner_call = [-1] * n   # innermost enclosing call "(" of each token
        self.unevaluated = [False] * n
        stack, calls, unevaluated_depth, sizeof_operand = [], [], 0, False
        for k, tok in enumerate(self.text):
            self.inner_call[k] = calls[-1] if calls else -1
            self.unevaluated[k] = unevaluated_depth > 0 or sizeof_operand
            sizeof_operand = False
            if tok in "([{":
                prev = self.text[k - 1] if k else ""
                call = tok == "(" and (prev in (")", "]") or (
                    re.match(r"^[A-Za-z_]\w*$", prev) and prev not in KEYWORDS))
                unevaluated = tok == "(" and prev in UNEVALUATED
                stack.append((k, call, unevaluated))
                self.is_call[k] = bool(call)
                if call:
                    calls.append(k)
                if unevaluated:
                    unevaluated_depth += 1
            elif tok in ")]}" and stack:
                opened, call, unevaluated = stack.pop()
                self.match[opened], self.match[k] = k, opened
                if call and calls and calls[-1] == opened:
                    calls.pop()
                if unevaluated:
                    unevaluated_depth -= 1
            # `sizeof x` without parentheses: the next operand is unevaluated.
            if tok == "sizeof" and k + 1 < n and self.text[k + 1] != "(":
                sizeof_operand = True

    def line(self, k):
        return self.code.count("\n", 0, self.pos[k]) + 1

    def is_ident(self, k):
        return 0 <= k < len(self.text) and re.match(r"^[A-Za-z_]\w*$", self.text[k]) is not None

    def unary(self, k):
        """Is the & or * (or ++/--) at k a prefix operator?"""
        if k == 0 or not OPERAND_END.match(self.text[k - 1]) or self.text[k - 1] in KEYWORDS:
            return True
        return self.text[k - 1] == ")" and self.is_cast(self.match[k - 1], k - 1)

    def is_cast(self, open_k, close_k):
        """Do the parentheses open_k..close_k hold a type name, as in
        `(gpointer *) &out`? A pointer type, or one to three identifiers."""
        if open_k < 0 or self.is_call[open_k]:
            return False
        inner = self.text[open_k + 1:close_k]
        if not inner:
            return False
        if inner[-1] == "*":
            return all(t == "*" or self.is_ident(open_k + 1 + i) for i, t in enumerate(inner))
        return len(inner) <= 3 and all(self.is_ident(open_k + 1 + i) for i in range(len(inner)))

    def path(self, k, end):
        """Parse a variable path starting at identifier k. Returns (parts, next):
        parts are (prefix, how the path goes on from it: "->", "[", "." or
        None for the whole path), shortest first, prefixes normalised."""
        if not self.is_ident(k) or self.text[k] in KEYWORDS:
            return [], k
        parts, current, j = [], self.text[k], k + 1
        while j < end:
            tok = self.text[j]
            if tok in ("->", ".") and self.is_ident(j + 1):
                parts.append((current, tok))
                current += tok + self.text[j + 1]
                j += 2
            elif tok == "[" and self.match[j] > j:
                parts.append((current, "["))
                current += "[" + " ".join(self.text[j + 1:self.match[j]]) + "]"
                j = self.match[j] + 1
            else:
                break
        parts.append((current, None))
        return parts, j


def split_args(tokens, open_k):
    """Token ranges [start, end) of the arguments of the call at open_k."""
    close = tokens.match[open_k]
    args, start, k = [], open_k + 1, open_k + 1
    while k < close:
        tok = tokens.text[k]
        if tok in "([{" and tokens.match[k] > k:
            k = tokens.match[k] + 1
            continue
        if tok == ",":
            args.append((start, k))
            start = k + 1
        k += 1
    if start < close or args:
        args.append((start, close))
    return args


def uses(tokens, open_k, start, end):
    """(modified, read) variable paths of one argument."""
    modified, read = {}, set()
    k = start
    while k < end:
        tok = tokens.text[k]
        if tokens.unevaluated[k]:
            k += 1
            continue
        if tokens.is_ident(k) and tok not in KEYWORDS and (
                k == 0 or tokens.text[k - 1] not in ("->", ".")):
            parts, nxt = tokens.path(k, end)
            prev = tokens.text[k - 1] if k > start else ""
            prev2 = tokens.text[k - 2] if k - 1 > start else ""
            address_of = prev == "&" and tokens.unary(k - 1)
            if prev == "(" and prev2 == "&" and tokens.unary(k - 2):
                address_of = True  # &(v)
            whole = parts[-1][0]
            # A prefix is read when the path dereferences or indexes it; a
            # struct member access (P.x) reads P's member, not P.
            read.update(prefix for prefix, how in parts[:-1] if how in ("->", "["))
            if address_of:
                # &P reads nothing more; inside a nested call it hands the
                # callee a way to write P.
                if tokens.inner_call[k] != open_k:
                    modified.setdefault(whole, f"&{whole} passed to a nested call")
            else:
                read.add(whole)
                after = tokens.text[nxt] if nxt < end else ""
                if after in ("++", "--") or prev in ("++", "--") and tokens.unary(k - 1):
                    modified.setdefault(whole, f"{whole} incremented or decremented")
                elif after in ASSIGN_OPS:
                    modified.setdefault(whole, f"{whole} assigned")
            k += 1  # member names are skipped above; index expressions are read
            continue
        k += 1
    return modified, read


def check_text(text, rel="<text>"):
    tokens = Tokens(code_view(text))
    found = []
    for k, call in enumerate(tokens.is_call):
        if not call or tokens.match[k] < 0:
            continue
        callee = tokens.text[k - 1] if k else ""
        if SEQUENCED_MACROS.match(callee):
            continue
        args = split_args(tokens, k)
        if len(args) < 2:
            continue
        per_arg = [uses(tokens, k, s, e) for s, e in args]
        reported = set()
        for i, (modified, _) in enumerate(per_arg):
            for var, how in modified.items():
                for j, (other_mod, other_read) in enumerate(per_arg):
                    if j == i or var in reported:
                        continue
                    if var in other_read or var in other_mod:
                        reported.add(var)
                        found.append(Finding(
                            rel, tokens.line(k), callee, var,
                            f"{callee}(): argument {i + 1} modifies {var} ({how}) and argument "
                            f"{j + 1} {'modifies' if var in other_mod else 'uses'} it; their "
                            "order is unspecified (x86_64 GCC goes right to left). Hoist the "
                            "side effect into its own statement"))
    return found


def tree_files(root):
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--", "*.c", "*.h"],
                             check=True, capture_output=True).stdout.decode()
        files = [f for f in out.split("\0") if f]
    except (OSError, subprocess.CalledProcessError):
        files = [p.relative_to(root).as_posix() for p in Path(root).rglob("*")
                 if p.suffix in (".c", ".h") and p.is_file()]
    return sorted(f for f in files if not f.startswith(VENDORED) and "/third_party/" not in f)


def check(root, files=None, exceptions=None):
    root = Path(root)
    exceptions = EXCEPTIONS if exceptions is None else exceptions
    raw = []
    for rel in files if files is not None else tree_files(root):
        path = root / rel
        if path.is_file():
            raw += check_text(path.read_bytes().decode("utf-8", errors="replace"), rel)
    found, used = [], set()
    for f in raw:
        key = (f.path, f.callee, f.variable)
        if (exceptions.get(key) or "").strip():
            used.add(key)
        else:
            found.append(f)
    if files is None:
        for key, justification in exceptions.items():
            if not (justification or "").strip():
                found.append(Finding(key[0], 0, key[1], key[2],
                                     f"exception {key} has no justification"))
            elif key not in used:
                found.append(Finding(key[0], 0, key[1], key[2],
                                     f"stale exception {key}: nothing matches it any more"))
    return found


# --- self-test ---------------------------------------------------------------

# Each must be flagged for exactly the variables listed.
BAD = {
    # nostrc-qp24.91: gh-net-http.c, g_task_get_cancellable got NULL on x86_64.
    "steal-task": ("g_socket_address_enumerator_next_async (self->inner,\n"
                   "    g_task_get_cancellable (task), on_inner_next, g_steal_pointer (&task));",
                   {"task"}),
    # nostrc-qp24.91: test_media.c, test_attachments.c, size read as 0.
    "memdup-size": ("copy = g_memdup2 (g_bytes_get_data (bytes, &size), size);", {"size"}),
    "steal-member": ("g_task_return_pointer (self->task, g_steal_pointer (&self->task), NULL);",
                     {"self->task"}),
    "steal-deref": ("run (task->cancellable, g_steal_pointer (&task));", {"task"}),
    "clear": ("emit (self, self->name, g_clear_pointer (&self->name, g_free));", {"self->name"}),
    "clear-object": ("finish (g_clear_object (&conn), conn);", {"conn"}),
    "cast-out-param": ("use (read_into ((gpointer *) &buf, len), buf);", {"buf"}),
    "paren-address": ("use (read_into (&(buf)), buf);", {"buf"}),
    "postincrement": ("printf (\"%d %d\\n\", i++, i);", {"i"}),
    "predecrement": ("put (--n, items[n]);", {"n"}),
    "assignment": ("pair (x = next (), x);", {"x"}),
    "two-writers": ("both (g_steal_pointer (&a), g_clear_pointer (&a, g_free));", {"a"}),
    "index": ("show (fill (&cells[i]), cells[i]);", {"cells[i]"}),
    "nested-outer": ("outer (inner (g_bytes_get_data (b, &n), n));", {"n"}),
    "macro-body": ("#define TAKE(x) consume (peek (x), g_steal_pointer (&(x)))", {"x"}),
}
# None may be flagged.
GOOD = (
    "g_assert_cmpint (read_into (&n), ==, n);",
    "g_assert_cmpmem (g_bytes_get_data (b, &size), size, want, want_len);",
    "g_clear_pointer (&self->name, g_free);",
    "consume (&task, g_task_get_cancellable (task));",
    "f (g_steal_pointer (&a), b);",
    "f (g_steal_pointer (&s.x), s.y);",
    "f (obj->size, g_bytes_get_data (b, &size));",
    "f (sizeof (task), g_steal_pointer (&task));",
    "f (G_N_ELEMENTS (buf), fill (&buf));",
    "f (sizeof buf, fill (&buf));",
    "if (read_into (&n) && n > 0) go ();",
    "for (i = 0; i < n; i++) step (i);",
    "while ((c = next ()) != EOF) put (c);",
    "f ((read_into (&n), n));",
    "f (a == b, a);",
    "f (a <= b, a >= b, a != b, a);",
    "f (x & mask, g (x));",
    "f (\"g_steal_pointer (&task)\", task);",
    "f (task /* g_steal_pointer (&task) */, 1);",
    "static void run (GTask *task, GCancellable *c, gpointer *out);",
    "gsize size = 0; gconstpointer data = g_bytes_get_data (b, &size); copy = g_memdup2 (data, size);",
    "GCancellable *c = g_task_get_cancellable (task);\nnext_async (c, g_steal_pointer (&task));",
    "#if defined (A) && defined (B)\n#endif",
    "f (a->b, g (&a->c));",
    "f (&x, &x);",
    # A fixture struct handed to a helper while a sibling reads its fields.
    "g_assert_true (gh_store_purge (f.store, now (&f) - 30, &stats, NULL));",
    "gh_window_open_item (f.window, room_of (&f, x));",
    "send (&w.alice, bob, canary (&w, \"id\", FALSE));",
    "append (outs, KEYS[i].key, FIELD (r, &KEYS[i]));",
)


def self_test():
    failures = []
    for name, (code, want) in BAD.items():
        got = {f.variable for f in check_text(code)}
        if got != want:
            failures.append(f"BAD {name}: flagged {sorted(got)}, want {sorted(want)}")
    for code in GOOD:
        got = check_text(code)
        if got:
            failures.append(f"GOOD {code!r}: flagged {[f.variable for f in got]}")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        sys.exit(1)
    print(f"check-unsequenced-args self-test passed ({len(BAD)} bad, {len(GOOD)} good)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("files", nargs="*", help="files relative to --root (default: the tree)")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    found = check(args.root, args.files or None)
    for f in found:
        print(f"{f.path}:{f.line}: {f.detail}")
    if found:
        print(f"{len(found)} unsequenced argument use(s); see the header of {Path(__file__).name}",
              file=sys.stderr)
        return 1
    print("No call modifies and uses a variable in different arguments")
    return 0


if __name__ == "__main__":
    sys.exit(main())
