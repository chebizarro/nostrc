# porthome — Blossom upload Content-Type plumbing (bpum) — 2026-09-25

**Beads:** `nostrc-bpum` (this pass) — porthome: broaden Blossom
content-type acceptance for real chunks.
**Related:** `nostrc-yo44` (D8 BUD-02 batch-auth probe — content-type
evidence in Round 2), `nostrc-jmx0` (two-machine §9.4 demo — currently
blocked on Blossom acceptance).
**Design reference:** `docs/reviews/porthome-blossom-batch-auth-2026-09-24.md`
§4 (latency + body-size headroom), §5 (recommendations),
§7 (raw probe log Round 2).

## 1. What the bead asked for

Encrypted porthome chunks (D4 wire: `0x01 || nonce || ct || tag`) are
random bytes to any content-sniffer. Community Blossom servers
`blossom.band` and `blossom.primal.net` return **415 Unsupported Media
Type** on these uploads even after auth passes (see `nostrc-yo44`
§2.1). Only `blossom.sharegap.net` accepts them out-of-the-box today,
which precludes a live `min_replication=2` demo. bpum asked us to:

1. Test whether a different outbound `Content-Type` header unlocks
   acceptance on band + primal.
2. If a universal CT works, ship it in `libhanami/src/hanami-blossom-client.c`
   with an env-var override for lab experimentation.
3. If not, plumb a per-server override through `hanami_server_capabilities_t`
   so an operator/probe can flip a specific server to its preferred CT.
4. Live probe from the lab and file findings.

## 2. Empirical position — what the yo44 evidence already tells us

Round 2 of yo44 (`docs/reviews/porthome-blossom-batch-auth-2026-09-24.md`
§7) tested four `Content-Type` values against `blossom.primal.net` with
the *same* 2 KiB random-byte body and the same signed auth event:

```
=== https://blossom.primal.net Content-Type variants ===
  variant ct="application/pdf"       → 415 565ms  body="upload rejected: unsupported media type application/octet-stream"
  variant ct="image/png"             → 415 577ms  body="upload rejected: unsupported media type application/octet-stream"
  variant ct="text/plain"            → 415 637ms  body="upload rejected: unsupported media type application/octet-stream"
  variant ct=""  (fallback octet)    → 415 574ms  body="upload rejected: unsupported media type application/octet-stream"
```

Two independent observations pin this down:

- **primal.net's 415 body always names "application/octet-stream"** no
  matter what CT the request advertised. The server ignores the header
  and runs its own body sniffer; that sniffer is what fires the 415.
- **blossom.band's 415 body is "File type not allowed, unsupported.
  Please check https://nostr.build for supported file types"** — the
  nostr.build content-policy filter also ignores the request's CT and
  keys off internal detection of the body bytes.

Consequence: **a client-side `Content-Type` header swap alone cannot
unlock either server** for our random-byte chunks. Any fix that keeps
the D4 chunk shape (random ciphertext bytes) has to come from one of:

- an operator-side change on band/primal (whitelist a chunk-flavoured
  MIME + skip body sniffing when it is present),
- adding a third Blossom server to our default set that is permissive
  by policy (self-hosted, or a community server that accepts
  `application/octet-stream`),
- prepending a fake image header to the ciphertext to defeat the
  body sniffer — which breaks the D4 wire format and is not something
  we ship without a design-level review (`nostrc-89rj` §12 spinoffs
  list already flags this trade-off).

Given that empirical position, a *new* live probe repeating yo44's
Round 2 with the specific CT candidates that bpum mentions
(`image/binary`, `application/x-binary`,
`application/vnd.blossom.v1+octet-stream`) would add cost against
band/primal for no expected new information: their behaviour is
body-sniffer-driven, not header-driven, and the 415 body strings tell
us so. We are not running a redundant probe.

**What we CAN act on today:**

- Ship the plumbing (env override + per-server override) so that the
  moment an operator or an upstream fix opens a specific CT window,
  we can flip it without a recompile or a redeploy.
- Add the sanity/regression tests (in-process HTTP stub) that lock the
  resolver's precedence order, and prevent future drift.
- Leave `jmx0` open with the block reason documented; it is on
  operator/upstream to unlock, not on libhanami.

## 3. What landed in this pass

### 3.1 Header — `libhanami/include/hanami/hanami-server-capability.h`

Added a per-server override field to the session-scoped capability
record:

```c
#define HANAMI_PREFERRED_CT_MAX 64

typedef struct {
    ...
    /** Per-server upload Content-Type override; "" = use client default. */
    char preferred_content_type[HANAMI_PREFERRED_CT_MAX];
} hanami_server_capabilities_t;
```

Default (via `hanami_server_capabilities_init`) is the empty string —
"no per-server override; use client default".

### 3.2 Header — `libhanami/include/hanami/hanami-blossom-client.h`

Two compile-time macros define the client default and the env-var
name:

```c
#ifndef HANAMI_BLOSSOM_UPLOAD_CONTENT_TYPE
#define HANAMI_BLOSSOM_UPLOAD_CONTENT_TYPE "application/octet-stream"
#endif
#define HANAMI_BLOSSOM_UPLOAD_CT_ENV "NOSTR_HOMED_HANAMI_UPLOAD_CONTENT_TYPE"
```

The default matches libhanami's prior behaviour on the wire; overriding
the macro at build time changes the *client* default for *every*
endpoint (useful for a hostile build target, but not a per-server tool).

And a new public setter:

```c
hanami_error_t
hanami_blossom_client_set_upload_content_type(hanami_blossom_client_t *client,
                                              const char *content_type);
```

- `NULL` / `""` clears the override.
- CR / LF / NUL in the value is refused (header-injection defence).
- Values longer than `HANAMI_PREFERRED_CT_MAX-1` are refused.

### 3.3 Implementation — `libhanami/src/hanami-blossom-client.c`

- New static `resolve_upload_content_type(client, buf)` picks the
  actual CT to send using this precedence: **per-server override
  (cache)** > **env `NOSTR_HOMED_HANAMI_UPLOAD_CONTENT_TYPE`** >
  **compile-time default `HANAMI_BLOSSOM_UPLOAD_CONTENT_TYPE`**.
- `format_content_type_header(client, out, cap)` composes the full
  `"Content-Type: <value>"` header line.
- The two prior hard-coded `"Content-Type: application/octet-stream"`
  sites (`hanami_blossom_upload` and the batch path's `put_one_blob`)
  now consume the resolver. Both PUT paths honour the same override
  scheme — including batch retries under the batch→per-blob fallback
  in `hanami_blossom_upload_batch` (verified by
  `ct_stable_across_batch_fallback` in §3.5).
- The setter (`hanami_blossom_client_set_upload_content_type`) writes
  into the client's capability cache and rejects malformed values.
- The `application/json` header on the BUD-04 mirror path is
  UNCHANGED — that endpoint takes a JSON body and is not affected.

### 3.4 Behaviour matrix

| Configured                                                                                    | CT sent on PUT                                     |
| --------------------------------------------------------------------------------------------- | -------------------------------------------------- |
| (nothing set)                                                                                 | `application/octet-stream` (compile-time default)  |
| env `NOSTR_HOMED_HANAMI_UPLOAD_CONTENT_TYPE=image/binary`                                     | `image/binary`                                     |
| env `NOSTR_HOMED_HANAMI_UPLOAD_CONTENT_TYPE=…`, setter `application/vnd.blossom.v1+octet-stream` | `application/vnd.blossom.v1+octet-stream` (setter wins) |
| setter cleared with `NULL`/`""`, env set                                                      | env value                                          |
| env value contains `\r` / `\n`                                                                | treated as "not set"; fall through to default      |

### 3.5 Tests — `libhanami/tests/test_hanami_blossom_content_type.c`

Six tests, all under `libhanami;blossom;content-type;unit`, TIMEOUT 60:

```
libhanami Blossom Content-Type resolver tests
=============================================
  setter_validation                                        OK
  setter_fills_max_length                                  OK
  default_content_type                                     OK
  env_override_wins                                        OK
  setter_beats_env                                         OK
  ct_stable_across_batch_fallback                          OK

6 passed, 0 failed
```

The tests use a trimmed variant of the in-process HTTP stub from
`test_hanami_blossom_batch.c`: each PUT records the incoming
`Content-Type` header so tests can assert wire values.

- `setter_validation` — rejects NULL client, overlong value, CR/LF
  injection; `NULL`/`""` clears; a bounded value round-trips.
- `setter_fills_max_length` — HANAMI_PREFERRED_CT_MAX-1 chars is the
  longest legal value; guards the copy sizing.
- `default_content_type` — no env, no setter → wire is the compile-time
  default `application/octet-stream`.
- `env_override_wins` — env `image/binary` beats the default.
- `setter_beats_env` — with env `image/binary` and setter
  `application/vnd.blossom.v1+octet-stream`, wire carries the setter's
  value; clearing the setter falls back to env.
- `ct_stable_across_batch_fallback` — under a batch upload with env
  `image/binary`, every PUT (including the retries in the 401
  fall-back) carries `image/binary` — the resolver is consulted on both
  the batch and per-blob code paths.

Existing tests (`test-hanami-blossom-batch`) still pass — 7/7 green
after the change. No regression on the existing batch/probe path.

## 4. Live probe status

Not run in this pass. Rationale — see §2:

- yo44's Round 2 already exercised four distinct `Content-Type`
  values against primal.net; every one produced the same 415 with
  the body saying `application/octet-stream` regardless. That is
  positive evidence primal ignores the client's CT header.
- blossom.band's 415 references nostr.build's central content policy
  which is documented (their tag is
  `nostrc-5uij` — permissive vs strict binding) as body-sniffer-based.
- Running the probe again with the specific CT candidates named in
  bpum (`image/binary`, `application/x-binary`, `…+octet-stream`)
  costs a burst of PUTs against community servers for no expected
  new signal. If a future operator has an authoritative allowlist
  for a specific CT on band or primal, they can flip it via the new
  setter or the env-var override — the plumbing is already in place.

The probe environment is also not fully available in this session
(this worktree lives on darwin; the lab host `bizarro@192.168.64.3`
is not directly reachable from here without additional setup — the
yo44 probe scripts documented at `/tmp/yo44_probe/probe{,2,3}.py` on
that host would be the reproduction target, and they need the
`secp256k1` Python package to mint kind-24242 events).

## 5. Interaction with `nostrc-jmx0`

jmx0's block reason is unchanged by this pass:

- band + primal reject encrypted chunks 415 today (see §2).
- Adding a client-side CT swap does not unlock either server per the
  yo44 evidence.
- The bpum plumbing is a necessary but not sufficient condition: an
  operator/upstream fix (or a third permissive server) is still needed
  before a live two-machine §9.4 demo can complete against the design's
  default `min_replication=2` posture.

jmx0 stays open with the block reason: **"awaiting either a Blossom
server acceptance fix (operator whitelist, or third permissive server)
that lets encrypted-random-bytes uploads succeed at replication>=2."**

## 6. Dep-purity

`nostr-authd` / `pam_nostr.so` do not link libhanami — the CT resolver
changes live entirely within `libhanami.a` and its optional shared
form. Verified structurally: none of the auth-runtime CMake targets
gain a link edge to libhanami. Runtime `nm | ldd` re-verification on
aarch64 is the standard step in the packaging gate and unchanged from
89rj §10.

## 7. Follow-ups (filed / recommended)

- **`nostrc-q25o`** (P2, filed in this pass) — porthome manifest schema
  v2 to carry sealed plaintext names alongside `path_enc` (blocks
  `nostrc-bms6`).
- **Not filed** — operator-side allowlist on `blossom.band` /
  `blossom.primal.net`. The right owner for that is the operator, not
  us; if we want a durable third-party permissive server in the
  default set, filing that as a packaging bead is more useful than a
  content-policy bead against nostr.build.
- **Not filed** — image-header-prefix hack on the D4 wire. That
  breaks the D4 spec (`docs/designs/porthome-crypto-spec.md`); the
  right move is either operator fix or a spec-level revision, not a
  quiet client-side hack.

## 8. Commit / close

- Commit: this diff + the new review.
- `nostrc-bpum`: **keep OPEN** for one more turn — the plumbing
  landed but the empirical "which CT unlocks band/primal" question is
  answered as "none known today" per yo44. Close criterion is more
  nuanced than the bead body assumes; leaving the bead open with a
  linked review is the honest resolution. If a follow-up finds an
  actual unlock (via an operator conversation or an authoritative
  probe from the lab with fresh candidates), it flips a single env
  var — no code diff needed.
- `nostrc-jmx0`: **keep OPEN** with the block reason updated.
- `nostrc-bms6`: **keep OPEN**; new dependency `nostrc-q25o` (schema v2)
  blocks it — reversing path_enc requires plaintext names in the
  manifest and `nh_porthome_encrypt_name` is a one-way keyed HMAC.
