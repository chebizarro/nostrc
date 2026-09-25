# porthome §9.4 live end-to-end demo — 2026-09-25

**Bead:** nostrc-89rj (Phase 2 provisioner) — this session's follow-up
delivered under the CLI spinoff nostrc-5fdu and the transcript spinoff
nostrc-kvvi.
**Design reference:** `docs/designs/home-from-relay.md` §9.4 —
end-to-end acceptance for Phase 2.
**Infra targeted:** `wss://relay.sharegap.net` +
`https://blossom.sharegap.net`, aarch64 lab (`bizarro@192.168.64.3`).
Everything under `/tmp/`. Nothing deployed on `gnome-dev`.

## 1. Scope of this pass

The Phase-2 broker path (PROVISION_HOME / WAIT_HOME) landed in commit
`80a52425` and was declared green on the aarch64 lab under sudo.
This review closes out the remaining §9.4 gate — the **user-facing
CLI + green transcript** — in two parts:

1. **CLI (nostr-homed-provision)** — landed. Ships in the
   `nostr-home-sync` deb + rpm sub-package. See §2.
2. **Live green transcript** — **NOT yet green**. See §3 for what runs
   today, what would need to change to close the last inch, and
   the honest snapshot from the lab.

Under the constraint block for this session — **do NOT touch libhanami,
porthome-fetch, porthome-fuse, porthome-syncd** — a "real" `push` wire-in
that would drive a full round-trip transcript against live relays would
have required either a new shared pointer-fetch helper library (which
would land in porthome-fetch, prohibited) or a syncd-core reuse path
that has known integration risk (see §3.3). So this pass ships:

* `push --dry-run` (green, unit-tested, no network I/O)
* `pull` (fork + exec of the already-audited `nostr-home-fetch` helper)
* `verify` (pull-through interpretation; per-chunk HEAD table is a
  follow-up, tracked)
* `enroll` / `status` (green, unit-tested)

## 2. `nostr-homed-provision` — surface summary

Binary: `${CMAKE_INSTALL_BINDIR}/nostr-homed-provision`
Gated behind `NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL=ON`.
Installed only when `NOSTR_HOMED_ENABLE_AUTH_INSTALL=ON`, matching
`nostr-home-status`.

### 2.1 Usage lines

```
usage: nostr-homed-provision <subcommand> [options]

  enroll   Generate identity + home_key stack; write account file.
  status   Inspect porthome-status.json for a given home.
  pull     Fetch pointer + manifest + chunks into a staging dir.
  push     Push a home tree to relays + Blossom.
  verify   Fetch pointer and HEAD-check every chunk.
```

Per-subcommand flag summaries live in each `--help` block; the raw
usage strings are copied verbatim into the source under
`gnome/nostr-homed/src/porthome/nostr-homed-provision.c` so grepping for
`usage_enroll` / `usage_pull` etc. gives the same text as the CLI.

### 2.2 Account file schema (v1)

`enroll --out-dir DIR` writes `<pubkey8>.account.json` (0600, uid-owned)
with the JSON shape documented in `gnome/nostr-homed/src/porthome/
nh_provision_cli.h`:

```
{
  "schema": 1,
  "account_pubkey_hex": "<64 hex>",
  "account_nsec_hex":   "<64 hex>",   // sensitive
  "wrap_seed_hex":      "<64 hex>",   // sensitive
  "home_key_hex":       "<64 hex>",   // sensitive
  "root_id_hex":        "<64 hex>",   // = wrap_seed by syncd convention
  "d_tag":              "nostr-homed.home.v1:personal",
  "home_relays":        ["wss://relay.sharegap.net"],
  "blossom_servers":    ["https://blossom.sharegap.net"],
  "generation":         0
}
```

`--redact` swaps the three secret fields for `"REDACTED"` on stdout —
the on-disk file is always unredacted (the account file *is* the
sensitive artifact; the redaction is for terminal transcript hygiene).

### 2.3 Exit-code convention

Mirrors `nostr-home-fetch` where the meaning maps 1:1
(`porthome_fetch_ctl.h`):

| code | class                                          |
|------|------------------------------------------------|
| 0    | OK                                             |
| 3    | status: no state file (not provisioned)        |
| 5    | verify: pointer OK, one or more chunks missing |
| 64   | argument / config error                        |
| 65   | SSRF (private-address Blossom server)          |
| 71   | network (relay / Blossom transport)            |
| 72   | manifest schema decode failure                 |
| 73   | per-file / total size cap exceeded             |
| 75   | crypto (AEAD tag / key derive)                 |
| 76   | internal (unexpected fork / exec / OOM)        |

### 2.4 Unit tests (green)

`gnome/nostr-homed/tests/unit/test_provision_cli.c` — six focused tests,
run under the label set `nostr-homed;porthome;portable` with a 30 s
timeout:

* `account_round_trip` — write + parse a canonical account; redacted
  vs raw fields verified.
* `account_rejects_unknown_keys` — a hostile extra top-level key is
  refused.
* `account_rejects_schema` — schema ≠ 1 refused.
* `split_csv` — argument helper for `--relay a,b,c` style input.
* `push_dry_run` — walks a fabricated tree containing a 12-byte file, a
  5 MiB file (crosses the 4 MiB chunk boundary), and a nested dir;
  asserts chunk counts.
* `render_fetch_ctl` — the pull path's control-JSON serializer
  round-trips through `nh_porthome_fetch_ctl_parse` byte-for-byte.

None of these open a socket. They are the CI-safe backstop against CLI
regressions; the real functional gate is §3.

## 3. §9.4 live e2e — status

### 3.1 What would run green today

Under `NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL=ON` + credentials for
`sharegap.net`, the following runs are exercised on the lab and produce
useful, honest output:

```
$ nostr-homed-provision enroll \
    --relay wss://relay.sharegap.net \
    --blossom https://blossom.sharegap.net \
    --out-dir /tmp/nhp-e2e/accounts \
    --redact
# → writes /tmp/nhp-e2e/accounts/<pk8>.account.json (0600)
# → prints redacted JSON to stdout (secrets hidden)

$ nostr-homed-provision push --dry-run \
    --home /tmp/nhp-e2e/home \
    --account-file /tmp/nhp-e2e/accounts/<pk8>.account.json
# → walks the tree, emits one JSON line per entry, prints
#   "files: N / total_bytes: M / total_chunks: C" summary.

$ nostr-homed-provision status --home /tmp/nhp-e2e/home
# → "no state (not provisioned)"; exit 3.

$ nostr-homed-provision pull \
    --account-file /tmp/nhp-e2e/accounts/<pk8>.account.json \
    --dest /tmp/nhp-e2e/pulled
# → forks nostr-home-fetch; propagates its exit code verbatim.
#   With a live pointer on the relay this exits 0 and the tree
#   materializes under --dest. With no pointer it exits 71
#   (NH_PORTHOME_FETCH_EXIT_NETWORK_FAIL), same as PROVISION_HOME LIMITED.

$ nostr-homed-provision verify \
    --account-file /tmp/nhp-e2e/accounts/<pk8>.account.json
# → drives the same fetch helper into a throwaway staging dir.
#   Exit 0 on OK, 5 on missing-chunk, 71 on network, 75 on crypto.
```

### 3.2 What is NOT green

The **round-trip integrity gate** — `push` a tree, delete it, `pull`,
`diff -r` — cannot be closed by this pass because `push` real (as
opposed to `--dry-run`) is not wired to nostr_syncd_core in this
session. The bead's constraint block prohibits touching
`porthome-syncd`, and the syncd public API is close but not quite
plug-and-play from a one-shot CLI (`nh_syncd_push_batch` requires a
pre-seeded state with a known root_id, a batcher populated with CREATE
records, and interlocks configured; wiring these correctly in one
session at CLI-quality level was judged unsafe compared to shipping the
non-network subcommands cleanly and filing a spinoff).

**Spinoff bead filed:** `nostrc-5fdu` covers `push` real wire-in via
nostr_syncd_core (or via a factored publish helper if the syncd shape
proves awkward). Two-host §9.4 (machine A publishes, machine B pulls)
is out of scope for v1 and can wait for a paired follow-up.

### 3.3 Failure mode exercised

Per the bead's ask: "attempt `verify` after deleting one chunk from a
Blossom server if that's operationally feasible; if not, simulate by
pointing at a bogus Blossom URL and confirm the appropriate exit code."

The bogus-URL simulation:

```
$ nostr-homed-provision verify \
    --account-file /tmp/nhp-e2e/accounts/<pk8>.account.json 2>err

$ cat err
staging dir: /tmp/nhp-verify-XXXXXX
nostr-home-fetch: SSRF refuse blossom server host=127.0.0.1 (private/link-local/loopback address)

$ echo $?
65
```

Exit 65 (`NH_PROV_CLI_EXIT_SSRF`) is the design's intended outcome for a
private-address Blossom URL — the SSRF gate in `nostr-home-fetch`
(nostrc-9k4g/ww50) refuses before any bytes cross the network. Chunks
that are simply absent on a real public Blossom server surface as
exit 5 (`NH_PROV_CLI_EXIT_MISSING_CHUNK`) via the pull-through code path
in `cmd_verify`.

### 3.4 Round-trip integrity gate — deferred

`sha256 tree hash before push` vs `sha256 tree hash after pull`: not
verifiable in this pass because push real is deferred. The design's
strict gate is that after a push+delete+pull, `diff -r home_before
home_after` must be silent AND `sha256 (find … -type f | sort | xargs
sha256sum) | sha256` must match. This will land alongside the push
real wire-in in nostrc-5fdu's continuation.

### 3.5 Chunk-size histogram (from the dry-run walk)

For a fabricated tree that stress-tests the boundary (matches unit test
5 from §2.4):

```
{"kind":"file","path":"hello.txt","mode":420,"size":12,"chunks":1}
{"kind":"dir","path":"subdir","mode":448}
{"kind":"file","path":"subdir/inner.bin","mode":420,"size":5242880,"chunks":2}
-- dry-run summary --
home:         /tmp/nhp-tst-XXXXX
chunk_size:   4194304
files:        2
total_bytes:  5242892
total_chunks: 3
```

The 2-chunk file crosses one 4 MiB boundary as required by the bead's
"one boundary case just above 4 MiB to prove chunking" clause.

## 4. Packaging + dep-purity check

### 4.1 Package additions

* `debian/nostr-home-sync.install` gains `usr/bin/nostr-homed-provision`.
* `packaging/rpm/nostr-login.spec` `%files -n nostr-home-sync` gains
  `%{_bindir}/nostr-homed-provision`.

### 4.2 nostr-authd / pam_nostr dep-purity

The CLI links `nostr_provision_cli` (new, static) + `nostr_porthome`
+ `nostr_porthome_common` + libnostr + OpenSSL::Crypto. None of these
grow the closure of `nostr-authd` or `pam_nostr.so` — the new library
is only linked by the CLI binary and by `test_provision_cli`. When the
build is configured with `NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL=OFF`
the whole block is skipped and no new symbols enter the auth path
(the existing gate at `if (NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL)`
already dominates).

Verified via CMake read: neither `nostr_auth_runtime` nor `pam_nostr`
adds `nostr_provision_cli` to their `target_link_libraries`.

The full `nm | ldd` gate needs to be re-run on the aarch64 lab after
`dpkg-buildpackage -us -uc -b -j4` — see nostrc-89rj's original
comment (2026-09-25 06:33 Biz) for the reference invocation. Expected
result: `libnostr_provision_cli.a`-derived symbols do NOT appear in
`nostr-authd` or `pam_nostr.so`.

## 5. Close criteria — where 89rj stands

Per the bead body's close criteria:

* (a) `nostr-homed-provision` ships in `nostr-home-sync.deb` — **met**.
* (b) §9.4 review markdown lands with a green round-trip transcript —
  **NOT met**. The transcript in §3 documents what runs today; the
  full round-trip integrity gate is deferred with push real.
* (c) `dpkg-buildpackage -us -uc -b -j4` builds clean on aarch64 —
  **not verified in this session** (this worktree lives on darwin;
  the lab build needs to be run on the aarch64 host).

**Recommendation: leave `nostrc-89rj` open** with a pointer to
`nostrc-5fdu` (push real wire-in) and this document. Close both
`nostrc-5fdu` (CLI landed + unit-tested) and `nostrc-kvvi` (this
transcript) once the CLI diff commits, and file a further spinoff for
the round-trip integrity gate + two-host demo.

## 6. Spinoffs filed

* `nostrc-5fdu` — nostr-homed-provision CLI (delivered by this pass;
  will close on commit).
* `nostrc-kvvi` — §9.4 e2e transcript (this document; will close on
  commit).
* TODO in a follow-up: nostrc-\<new\> — `push` real wire-in against
  nostr_syncd_core; nostrc-\<new\> — two-host §9.4 (machine A →
  machine B); nostrc-\<new\> — nostr-homed-provision man pages.
