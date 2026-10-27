#!/usr/bin/env bash
#
# check-grotto-names.sh — the signer app is Grotto (org.nostr.Grotto); its old
# names must not come back (W31, nostrc-8otj).
#
# Fails when a tracked file outside the historical documents uses an old app
# name. Not old names, and allowed:
#  - the D-Bus protocol name org.nostr.Signer (the wire contract);
#  - storage-format identifiers kept so keys stored before the rename are
#    still found: org.gnostr.Signer/identity, /key, /migration,
#    org.gnostr.Signer.Session, .ClientSessions, .HardwareKeystore, and the
#    Keychain service "Gnostr Signer Migration";
#  - gnostr's own namespace: its modules gnostr-signer-service, -bridge,
#    -bridge-install and -availability, and GnostrSigner*/gnostr_signer_*
#    identifiers (the client side of the protocol, not the app).
#
# One narrow exception: a test that must create the pre-rename config
# directory to prove migration still works marks that single line with
# "grotto-names: allow legacy-fixture". The marker is honored only on its own
# line and only in files under apps/grotto/tests/, and it excuses only the bare
# legacy config name; any other old name on that line, or the marker anywhere
# else, is still reported.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
git ls-files -z | python3 -c '
import re, sys
kept = re.compile(r"org\.gnostr\.Signer/(identity|key|migration)|org\.gnostr\.Signer\.(Session|ClientSessions|HardwareKeystore)|Gnostr Signer Migration")
old = re.compile(r"gnostr-signer(?!-(availability|bridge-install|bridge|service)\b)|org\.gnostr\.Signer|G[Nn]ostr Signer|GNOSTR Signer")
legacy = re.compile(r"gnostr-signer(?![-\w])")
allow = "grotto-names: allow legacy-fixture"
allow_dirs = ("apps/grotto/tests/",)
skip = ("docs/", ".beads/", "third_party/", "VERSION_MANIFEST.md", "scripts/check-grotto-names.sh")
bad = 0
for path in sys.stdin.buffer.read().split(b"\0"):
    path = path.decode()
    if not path or path.startswith(skip):
        continue
    if old.search(kept.sub("", path)):
        print(f"{path}: file name uses an old signer name"); bad += 1
    try:
        text = open(path, encoding="utf-8").read()
    except (UnicodeDecodeError, IsADirectoryError, FileNotFoundError):
        continue
    for n, line in enumerate(text.split("\n"), 1):
        line_ = kept.sub("", line)
        if allow in line and path.startswith(allow_dirs):
            line_ = legacy.sub("", line_)
        if old.search(line_):
            print(f"{path}:{n}: {line.strip()[:120]}"); bad += 1
            if bad > 40: break
if bad:
    print("check-grotto-names: old signer names found (the app is Grotto, org.nostr.Grotto)", file=sys.stderr)
    sys.exit(1)
print("check-grotto-names: ok")
'
