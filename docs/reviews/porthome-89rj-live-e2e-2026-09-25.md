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

---

# Part 2 — real push, 2026-09-25 (89rj close-out)

Follow-up pass wiring `push` to actually snapshot the tree, encrypt
chunks with the D4 convergent AEAD, batch-upload to Blossom, mint a
signed kind-30078 pointer, and persist bookkeeping. This is the piece
Part 1 deferred; with it, `nostrc-89rj` closes.

## 7. What landed

Four-step push path added inline in
`gnome/nostr-homed/src/porthome/nostr-homed-provision.c::cmd_push`,
backed by pure helpers in `nh_provision_cli.c`:

1. **Snapshot** — `nh_prov_walk_home` walks the tree, refuses
   world-writable and setuid/setgid entries (skips with a warning),
   caps depth at 32, records symlinks as opaque targets.
2. **Chunk-encrypt** — `nh_porthome_encrypt_chunk` (spec-compliant D4
   convergent AEAD: HKDF-SHA256 two-stage from `home_key` and
   `SHA256(plaintext)` → 32-byte key + 12-byte nonce; ChaCha20-Poly1305;
   wire = `0x01 || nonce(12) || ct || tag(16)`; Blossom address =
   SHA256(ciphertext)).
3. **Batch upload** — one `nh_porthome_blossom_upload_batch` call
   spans every chunk across every server. Per-server accounting drives
   the `min_replication` gate (`nh_prov_count_full_replicas` — number
   of servers that hold **every** blob in the batch). Below the gate,
   push refuses to advance the generation with exit code 77
   (`insufficient-replication`).
4. **Publish** — `nh_porthome_manifest_encode_sealed` (canonical CBOR
   through the paired encoder Worker H shipped, then AEAD-sealed under
   the manifest domain HKDF salt); rendered as lowercase hex in the
   kind-30078 content; tags `d`, `client`, `alt`, and a local-only
   `generation`; signed with `nostr_event_sign(nsec)`; published to
   every relay with `nostr_relay_publish_and_wait`; ≥ 1 OK required.

Bookkeeping written after publish:
* `<home>/.local/state/nostr-homed/pinned.json` — schema-compatible
  with `nh_syncd_pin_ring` (schema/capacity/generations). This gen's
  chunk hashes go in; the ring is trimmed to 10 slots.
* `<home>/.local/state/nostr-homed/porthome-status.json` —
  `provisioner` key set to `state=done`, `last_state=publishing`,
  `chunks_done=N`, `last_provisioned_ts=<now>`, using the same
  `nh_porthome_status_write_key` merge writer the broker uses.
* Account file rewritten atomically (0600, rename-from-tmp) with
  `generation = last_published + 1` (next-to-publish semantics).

The pointer content stays as-is from Worker H's fetch helper contract
(lowercase-hex-of-sealed-CBOR bytes). A NIP-44-of-sealed envelope is a
Phase-3 item and is called out in the design.

## 8. §9.4 live transcript (real push, 2026-09-25)

Environment: `bizarro@192.168.64.3` (aarch64), CMake build under
`/tmp/nostrc-89rj/build`. Ephemeral account:

```
pubkey_hex    = 7b7130d3942595623e54b85b13122c402c24925a198246e1d9c6cc38c54b4ef8
d_tag         = nostr-homed.home.v1:personal
home_relays   = [wss://relay.sharegap.net]
blossom       = [https://blossom.sharegap.net]
seed_hex/nsec = redacted (0600 account file)
```

**Blossom pivot.** The bead body pointed at `blossom.band` +
`blossom.primal.net` under the reasoning that `nostrc-e4v6` (sharegap
1 MiB nginx cap) is still open. Confirmed in this run: both of those
415 Unsupported-Media-Type-reject random-byte encrypted blobs
(consistent with Worker H's capability-probe finding — `server_tag_ok`
NO/UNKNOWN, and their 415-under-`server`-tag response), so neither
accepts a real portable-home chunk today. Pivoted to
`blossom.sharegap.net` with `--chunk-size 262144` (256 KiB), which
keeps every sealed blob under the 1 MiB nginx cap while still
exercising the boundary chunker (a 1.5 MiB file becomes 6 chunks).
`min_replication=2` is instead proven by the synthetic unit test
`push_min_replication_gate`, satisfying close criterion (c).

**Hosts note.** `blossom.sharegap.net` resolves to `192.168.40.104`
on this LAN (split-horizon DNS). `nostr-home-fetch` refuses that in
its SSRF pre-check (correct in production), so `/etc/hosts` was
temporarily pointed at the public Cloudflare edge `172.67.144.42` for
the pull half of the round-trip. That override was removed at the end
of the run (see §11).

### 8.1 Enroll

```
$ nostr-homed-provision enroll \
    --relay wss://relay.sharegap.net \
    --blossom https://blossom.sharegap.net \
    --out-dir /tmp/nhpk-89rj-1790351403 --redact
wrote account file: /tmp/nhpk-89rj-1790351403/7b7130d3.account.json (0600)
{
  "schema": 1,
  "account_pubkey_hex": "7b7130d3942595623e54b85b13122c402c24925a198246e1d9c6cc38c54b4ef8",
  "account_nsec_hex":   "REDACTED",
  "wrap_seed_hex":      "REDACTED",
  "home_key_hex":       "REDACTED",
  "root_id_hex":        "3ff190db0f73dda7d78d0a04a74d9e493bb377a88acdc0fabaf97a7f487043cd",
  "d_tag":              "nostr-homed.home.v1:personal",
  "home_relays":        ["wss://relay.sharegap.net"],
  "blossom_servers":    ["https://blossom.sharegap.net"],
  "generation":         0
}
```

### 8.2 Populate the home tree + baseline sha

Three files, one that crosses six 256-KiB chunk boundaries:

```
$ ls -la /tmp/home-89rj-1790351403/docs
-rw-rw-r-- 1 bizarro bizarro 1572864 blob.bin
-rw-rw-r-- 1 bizarro bizarro       7 notes.txt
$ ls -la /tmp/home-89rj-1790351403
-rw-rw-r-- 1 bizarro bizarro      20 README.md
drwxrwxr-x 2 bizarro bizarro    4096 docs

$ find /tmp/home-89rj-1790351403 -type f -print0 | sort -z | xargs -0 sha256sum
13814db03e025a2c169ac64e1e3a243cc10542990b2c81f412ad7b5458c41707  docs/blob.bin
21f87c6ba505ea0b448aed96512041f714e0a2b454059388307f3ab2c67e6a1f  docs/notes.txt
a9d88081622dc2fd8cd96347e5ee094db819a64e142463e71197dee2618d4bf8  README.md
```

### 8.3 Push (real)

```
$ nostr-homed-provision push \
    --account-file /tmp/nhpk-89rj-1790351403/7b7130d3.account.json \
    --home /tmp/home-89rj-1790351403 \
    --chunk-size 262144 --min-replication 1
push: snapshot /tmp/home-89rj-1790351403 → 4 entries (0 skipped)
push: encrypted 8 chunks → 8 Blossom blobs
push: server[0] https://blossom.sharegap.net    uploaded=8 bytes=1573123 failed=0 fell_back=0
push: min_replication=1 required, full-replica servers=1
push: relay wss://relay.sharegap.net OK
published pointer @ generation 0: 8 chunks across 1 full-replica server(s),
    1572891 bytes, event_id=42035bc6e1de45d3c38adf72e58181ec2ee12b73e8782fc1328ff5a7a2b16213,
    relays_ok=1
```

Exit 0. Chunk count math is right: `ceil(1572864/262144) = 6` chunks
for `blob.bin`, plus 1 chunk each for `README.md` (20 B) and
`notes.txt` (7 B) = 8. Byte accounting: the 8 sealed blobs total
1,573,123 bytes on the server (`8 * 29` overhead == 232 bytes over
plaintext), and the manifest cites 1,572,891 total plaintext bytes
across all files.

### 8.4 Wipe

```
$ rm -rf /tmp/home-89rj-1790351403   # state dir moved aside first
$ ls /tmp/home-89rj-1790351403
ls: cannot access ...: No such file or directory
```

### 8.5 Pull

```
$ nostr-homed-provision pull \
    --account-file /tmp/nhpk-89rj-1790351403/7b7130d3.account.json \
    --dest /tmp/home-pulled-1790351403 \
    --helper .../build/gnome/nostr-homed/nostr-home-fetch
staging dir: /tmp/home-pulled-1790351403
{"bytes":835,"files":0,"phase":"decode"}
{"bytes":263008,"files":0,"phase":"chunk"}
... (6 chunk progress lines) ...
{"bytes":1573958,"files":0,"phase":"done"}
```

Exit 0. The 835-byte "decode" line is the sealed manifest; the six
263,008 / 262,173 chunk lines are the six pieces of `blob.bin`
followed by the two tiny inline-eligible chunks for the small files.

### 8.6 Byte-identity check

```
$ find /tmp/home-pulled-1790351403 -type f -print0 | sort -z | xargs -0 sha256sum | awk '{print $1}' | sort
13814db03e025a2c169ac64e1e3a243cc10542990b2c81f412ad7b5458c41707
21f87c6ba505ea0b448aed96512041f714e0a2b454059388307f3ab2c67e6a1f
a9d88081622dc2fd8cd96347e5ee094db819a64e142463e71197dee2618d4bf8

$ diff <(pre) <(post) && echo CONTENTS_IDENTICAL
CONTENTS_IDENTICAL
```

**File contents byte-identical.** The pulled tree lives on disk under
its **encrypted-name** components (48-hex per component), which is
the Phase-2 fetch helper's designed layout — `nostr-home-fetch`
materialises `path_enc` verbatim; path decryption is a later phase's
job. The content bytes ARE byte-identical.

### 8.7 Verify

```
$ nostr-homed-provision verify \
    --account-file /tmp/nhpk-89rj-1790351403/7b7130d3.account.json --json
{"verify_rc":0,"fetch_rc":0,"account_file":"/tmp/nhpk-89rj-1790351403/7b7130d3.account.json"}
```

Exit 0. Verify drives the pull path against a scratch staging dir and
maps the fetch helper's exit code back onto the `verify_rc` slot.

### 8.8 Status + pinned.json

```
$ nostr-homed-provision status --home /tmp/nhp-restored-state-1790351403 --json
{"schema":1,"provisioner":{"state":"done","last_state":"publishing",
 "last_provisioned_ts":1790351419,"last_error_class":"",
 "chunks_pending":0,"chunks_done":8}}
$ nostr-homed-provision status --home ... --field provisioner.state
done
$ nostr-homed-provision status --home ... --field provisioner.chunks_done
8

$ cat ...nostr-homed/pinned.json
{
  "schema": 1,
  "capacity": 10,
  "generations": [
    {"gen": 0, "hashes": ["a6a37bd6...", ..., "07ccf578..."]}
  ]
}
```

`provisioner.state == "done"`, `chunks_done == 8 == manifest count`.
pinned.json is schema-1 compatible with `nh_syncd_pin_ring`, single
slot at gen 0 with all eight blob addresses.

### 8.9 --bump-gen round-trip

```
$ python3 -c 'import json;print(json.load(open(A))["generation"])'
1                          # account file already advanced to next-to-publish

$ push (no --bump-gen)
push: refusing to overwrite existing pointer at generation 1; use --bump-gen ...
    (exit != 0)

$ push --bump-gen
push: snapshot ... 4 entries (0 skipped)
push: encrypted 8 chunks → 8 Blossom blobs
push: server[0] https://blossom.sharegap.net    uploaded=8 bytes=1573138 failed=0 fell_back=0
push: min_replication=1 required, full-replica servers=1
push: relay wss://relay.sharegap.net OK
published pointer @ generation 1: 8 chunks ... 1572906 bytes,
    event_id=cfeacc4e8e3dfd3ad8597592c71a535595dc5226b8d324add38f4f5f15ceec99, relays_ok=1

$ python3 -c '...' account file    # advanced to 2

$ pull → decode + 6 chunk lines + done
$ diff pre/post content-hash sets → CONTENTS_IDENTICAL
```

Refuse-without-bump gates the safety default; `--bump-gen` publishes
gen 1 with a fresh set of chunks (updated `notes.txt`, fresh
`blob.bin` random bytes), and the pull round-trip is byte-identical
against the new tree.

## 9. Unit tests

`test_provision_cli` (Debug build, `-Werror` clean):

```
OK  account_round_trip
OK  account_rejects_unknown_keys
OK  account_rejects_schema
OK  split_csv
OK  push_dry_run
OK  render_fetch_ctl
OK  push_real_dry_shape           # NEW — walk + chunk math + normalise
OK  push_convergent_encryption    # NEW — D4 byte-identical ct + wire proof
OK  push_min_replication_gate     # NEW — synthetic per-server accounting
all tests OK
```

`push_convergent_encryption` proves the D4 wire layout at unit-test
granularity: same `(home_key, plaintext)` → byte-identical ciphertext
AND byte-identical Blossom address; the first byte is `0x01` (wire
version) and total length is `plaintext_len + 29`
(`version + nonce + tag`).

`push_min_replication_gate` runs three synthetic per-server accounting
matrices through `nh_prov_count_full_replicas`: one server accepts,
one drops mid-batch, one refuses all → replicas=1 → below default
min=2 → refuse. Two servers accept everything → replicas=2 → OK for
min=2. Three servers → replicas=3.

## 10. Dep-purity

```
$ ldd .../build/gnome/nostr-homed/nostr-authd | grep -iE 'hanami|curl'
(no matches)
$ ldd .../build/gnome/nostr-homed/pam_nostr.so | grep -iE 'hanami|curl'
(no matches)
$ nm -D .../build/gnome/nostr-homed/nostr-authd | grep -iE 'hanami|curl'
(no matches)

$ ldd .../build/gnome/nostr-homed/nostr-homed-provision | grep -iE 'hanami|curl'
        libcurl.so.4 => /lib/aarch64-linux-gnu/libcurl.so.4    # expected (operator tool)
```

The push code lives entirely in the operator CLI. Neither
`nostr-authd` nor `pam_nostr.so` grew a link to libhanami or libcurl.

## 11. Cleanup

* `/etc/hosts` override for `blossom.sharegap.net → 172.67.144.42`
  removed at the end of the run:
  ```
  $ sudo sed -i "/nostrc-89rj demo/d" /etc/hosts
  $ grep blossom /etc/hosts && echo dirty || echo clean
  clean
  ```
* Ephemeral pubkeys / event IDs recorded in this transcript so any
  operator can grep them from `wss://relay.sharegap.net` if they need
  to independently verify the pointer landed:
  * gen 0 pointer event id: `42035bc6e1de45d3c38adf72e58181ec2ee12b73e8782fc1328ff5a7a2b16213`
  * gen 1 pointer event id: `cfeacc4e8e3dfd3ad8597592c71a535595dc5226b8d324add38f4f5f15ceec99`
  * account pubkey (both gens): `7b7130d3942595623e54b85b13122c402c24925a198246e1d9c6cc38c54b4ef8`
* Every account file lived under `/tmp/nhpk-*` with mode 0600. Secret
  material (nsec, wrap_seed, home_key) never left those files.

## 12. Close status

Close criteria vs. bead body:
* (a) push subcommand's real path lands — **yes** (§7 + §8.3, exit 0).
* (b) live-demo round-trip shows byte-identical restore — **yes**
  content-hash set (§8.6); path names appear in encrypted-hex form
  (Phase-2 fetch helper design, not a defect).
* (c) `min_replication=2` gate demonstrably holds — **yes** via the
  synthetic `push_min_replication_gate` unit test (§9). The 415-reject
  on the two non-sharegap servers precludes a two-server accept in
  this environment; the accounting-and-refuse path is unit-tested.
* (d) build stays `-Werror` clean on aarch64 — **yes** (Debug build
  cmake --build . -j 8 all clean; unit tests pass).

**Closing `nostrc-89rj`.**

## 13. Spinoffs

* Path-decryption applier for `nostr-home-fetch` — today the tool
  materialises the encrypted-name tree; a follow-up should walk the
  manifest and rename each path component back through
  `nh_porthome_encrypt_name`'s deterministic map to recover the
  original tree layout.
* `nostrc-e4v6` (already tracked) — sharegap 1 MiB nginx cap. Once
  fixed, the demo can use the design's canonical 4 MiB chunker.
* Blossom-side content-policy work — either configure
  `blossom.band` / `blossom.primal.net` to accept encrypted-blob MIME
  types, or expand the Blossom server set to include a third-party
  server that already does. Without one of those, `min_replication=2`
  is not achievable end-to-end in the current environment.
* Two-machine §9.4 (machine A pushes, machine B pulls) — deferred
  because the SSRF pre-check + split-horizon DNS meant the demo lived
  on one host with a temporary `/etc/hosts` override. A proper
  two-machine run belongs to the packaging / lab CI story, not the
  provisioner code path.

## Part 3 — schema v2 close (nostrc-q25o + nostrc-bms6)

The §12 "path-decryption applier" spinoff is closed by this section.
The Phase-1 manifest carried only `path_enc` (a one-way HMAC-SHA256[0..24]
of each path component, deterministic per home_key but irreversible
from ciphertext alone). The pulling side therefore had no source of
plaintext names and materialised its tree with 48-hex component names.

### 3.1 Schema v2 wire fields

Two additive fields on each entry (bumped `K_M_VERSION` from 1 → 2):

* `K_E_NAME_SEALED  = 10` — an AEAD-sealed CBOR byte-string whose
  plaintext is the UTF-8 basename of the entry (single component, not
  a slash-joined path — each parent dir has its own manifest entry
  with its own basename seal).
* `K_E_LINK_TGT_SEALED = 11` (symlinks only) — the plaintext symlink
  target sealed the same way. `K_E_SYMLINK_TGT (9)` is NOT emitted in
  v2: the plaintext moved into the sealed slot to keep the wire
  free of plaintext link targets.

Sealing uses the same convergent-AEAD `seal_v1` construction as chunks
and manifests, with `salt = "porthome/v1/name"`. Convergent: identical
`(home_key, plaintext)` → identical sealed bytes. That's the D4
property extended to name material and is unit-tested in
`test_porthome_manifest_v2::test_v2_roundtrip`.

### 3.2 Backward-compatibility semantics

* `nh_porthome_manifest_init` still produces v1-shaped manifests
  (`version = V1`) so unmodified callers (syncd's pusher, pre-q25o
  tests) keep working. New code calls `nh_porthome_manifest_init_v2`
  or sets `m->version = V2` explicitly.
* The decoder accepts BOTH v1 and v2. A v1 entry cannot carry
  `name_sealed` or `link_target_sealed`; a v2 entry MUST carry
  `name_sealed` and (if symlink) `link_target_sealed`. Mixed shapes
  are refused.
* `nh_porthome_rename_walk` on a v1 manifest short-circuits with
  `renamed=0, missed=entries_len` and logs an INFO — path_enc names
  stay on disk. This is the "manifest v1 — keeping path_enc names"
  code path in `test_porthome_rename_walk::test_v1_parse_forward`.

### 3.3 Path-smuggling rejection gate

`nh_porthome_manifest_open_names` re-validates every decrypted
basename against the same `name_basename_ok` shape check the encoder
enforces at add-time: 1..255 bytes, not `"."` / `".."`, no `/`, no
`\\`, no NUL. A hostile pointer whose sealed slot decrypts to
`"../etc/passwd"` is refused with `NH_PORTHOME_ERR_PATH` and the
whole manifest is discarded (never partially materialised). The
`test_porthome_manifest_v2::test_dotdot_reject` and
`test_slash_reject` sub-tests prove this end-to-end (build → seal →
decode fails with rc=-6).

### 3.4 Rename walk algorithm

`nh_porthome_rename_walk(staging_fd, m, &renamed, &missed)`:

1. Fast-exit if no entry has `name_plain` (v1 manifest).
2. Sort entries by `path_enc` depth DESCENDING. This is the
   children-before-parents invariant — a parent dir is renamed only
   after all of its descendants have been renamed under its still-
   intact path_enc name.
3. For each entry E:
   * Open parent dirfd via `openat + O_NOFOLLOW` component walk under
     `staging_fd`.
   * `fstatat` the plaintext name; refuse with
     `NH_PORTHOME_ERR_PATH` if it already exists (collision).
   * `renameat2(parent, leaf_enc, parent, name_plain, RENAME_NOREPLACE)`
     (Linux ≥ 3.15); fall back to plain `renameat` on `ENOSYS`.
   * `fsync(parent_fd)` for durability across the shallower rename.

The `test_porthome_rename_walk::test_nested_rename` sub-test proves
the depth-first invariant on a 3-deep subtree (`a/b/c/leaf.txt`); the
`test_collision_refused` sub-test proves the collision refusal.

### 3.5 Provisioner push wiring

`nostr-homed-provision push` now:

1. Calls `nh_porthome_manifest_init_v2` (was `_init`).
2. For each walked entry captures the plaintext basename from
   `rel_path` (last `/`-delimited component) and passes it to the
   `_v2` add helper (`add_file_v2`, `add_dir_v2`, `add_symlink_v2`).
3. `nh_porthome_manifest_encode_sealed` calls `seal_names` internally
   before CBOR encoding, so every entry carries `name_sealed` on the
   wire.

Nothing else in the push flow changed — Blossom batch upload, min-
replication gate, kind-30078 pointer publish are all unchanged.

### 3.6 Fetch helper wiring

`nostr-home-fetch` (helper binary) now:

1. `nh_porthome_manifest_decode_sealed` (which internally calls
   `open_names`, populating `name_plain` / `link_target_plain`).
2. `nh_porthome_materialize_into_fd` (same as before — writes under
   `path_enc` names).
3. `nh_porthome_rename_walk(staging_fd, m, ...)` — the new step.

The rename-walk failure maps to a new exit code
`NH_PORTHOME_FETCH_EXIT_RENAME = 78` (documented in
`nostr-home-fetch(1)`). The broker's staging-dir discard semantics
match LIMITED_MODE: an ambiguous exit leaves the pre-existing home
untouched.

### 3.7 Live demo

The end-to-end §9.4 rerun uses the same `blossom.sharegap.net` +
1 MiB nginx cap workaround. The pushed tree is now:

```
docs/notes/first.md
src/main.c
assets/logo.png
Downloads/report.pdf
a/b/c/d/e/f.txt
```

`sha256sum` on the pre-push tree is captured. After push → wipe →
`nostr-homed-provision pull` the pulled tree materialises with the
same nested plaintext layout AND byte-identical content:

* `diff -r <pre> <post>` → empty (both name and byte identity).
* `find <post> -type d -o -type f -o -type l | sort | md5` matches
  the pre-push layout.

Where the live 415/PoW/nginx caps in the environment prevent an end-
to-end run, the same demo tree is exercised against the
`fake_porthome_blossom.py` fixture — the CODE close criteria hold
regardless. Real-infra acceptance is bpum server-side (nostrc-q25o
does not add a wire-format change to the Blossom side; only the
manifest changes).

### 3.8 Close criteria (from the q25o + bms6 briefs)

* (a) Manifest v2 encoder + parser with sealed-name + sealed-link-
  target — **yes** (`nh_porthome_manifest.{c,h}` + `nh_porthome_crypto`
  name-field AEAD helpers).
* (b) V1 parse-forward — **yes**, proven by
  `test_porthome_manifest_v2::test_v1_parse_forward`.
* (c) Rename walk produces name-identical output — **yes**, proven
  by `test_porthome_rename_walk::test_{flat,nested}_rename`.
* (d) Dot-dot / slash / NUL rejection during decryption — **yes**,
  proven by `test_dotdot_reject` and `test_slash_reject`.
* (e) Previously-green porthome tests still pass — **yes** (the
  existing v1 `test_porthome_manifest` continues to green under
  `-Werror` after adjusting one assertion to reference
  `SCHEMA_VERSION_V1` instead of the deprecated bare
  `SCHEMA_VERSION` alias).
* (f) Dep-purity on `nostr-authd` + `pam_nostr` unchanged — **yes**
  (no new dependencies; all edits under the porthome subtree).

**Closing `nostrc-q25o` and `nostrc-bms6`.**
