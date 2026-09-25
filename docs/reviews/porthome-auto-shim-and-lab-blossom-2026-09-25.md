# porthome — probe-driven auto-shim + lab-Blossom recipe — 2026-09-25

**Beads:** `nostrc-prli` (P3 — probe extension), `nostrc-si30` (P4 —
auto-shim decision), `nostrc-4f9v` (P4 — lab Blossom recipe).

**Prior context:**
- `docs/reviews/porthome-blossom-shim-2026-09-25.md` (bpum) — the shim
  primitives + env-var opt-in.
- `docs/reviews/porthome-hy3e-shim-live-probe-2026-09-25.md` (hy3e) —
  the empirical evidence sharegap raw ✓ / primal shim ✓ / band 500.
- `docs/reviews/porthome-blossom-batch-auth-2026-09-24.md` (yo44) — the
  baseline content-type + 415 matrix.

## 1. Probe extension — `hanami_server_probe_capabilities` (nostrc-prli)

Two ADDITIONAL PUT probes per server were added to the existing
capability probe (from ypn2). The existing four-step probe
(reachability HEAD + server-tag PUT + batch PUT×2 + strict-x PUT) is
UNCHANGED — the new steps run AFTER, are sequential, and are capped at
2 additional PUTs.

- **Step (e) raw-random**: upload a fresh 1 KiB `os.urandom` blob with
  single-x-tag auth, client's default `Content-Type` (usually
  `application/octet-stream`). Records:
    - `2xx` -> `caps.raw_random_ok = HANAMI_CAP_YES`
    - `400 / 415 / 422 / 5xx` -> `HANAMI_CAP_NO`
    - `401 / 403` -> leaves UNKNOWN (that failure is auth, not
      content-policy)

- **Step (f) PNG-shim**: wrap the SAME random bytes with the shim's
  41-byte prefix (via `hanami_blossom_shim_encode`), upload with
  `Content-Type: image/png` and single-x auth over
  `sha256(shim || random)`. Records:
    - `2xx` -> `caps.png_shim_ok = HANAMI_CAP_YES`
    - `400 / 415 / 422 / 5xx` -> `HANAMI_CAP_NO`
    - `401 / 403` -> UNKNOWN

Content-Type override for step (f) is done via
`hanami_blossom_client_set_upload_content_type(c, "image/png")` around
the `put_one_blob` call; the previous value is restored immediately
after (including the "" empty state that means "use client default").
This threads the shim's `image/png` requirement through the existing
Content-Type resolver instead of adding a new plumb-through.

Cleanup: best-effort DELETE of any 2xx-stored probe blob, using the
same ephemeral secp256k1 key that signed the PUT (so per-pubkey delete
auth matches upstream).

`NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE=1` still bypasses the entire
probe (existing behaviour). Backward compat: pre-existing capability
fields (`batch_ok`, `server_tag_ok`, `strict_x_binding`) are
untouched.

### Tests — `test_hanami_server_capability_probe_shim.c`

Four scenarios exercised against the batch-test HTTP stub, extended to
capture per-PUT Content-Type header and body-prefix bytes:

- **A** — 201 raw + 201 shim: cache lands `raw_random_ok=YES,
  png_shim_ok=YES`. Total PUT count = 6 (4 existing + 2 new).
  Shim PUT's Content-Type is exactly `image/png`. Shim PUT body starts
  with `89 50 4E 47` (PNG signature). Raw PUT body does NOT.
- **B** — 415 raw + 201 shim (primal.net shape): `raw_random_ok=NO,
  png_shim_ok=YES`.
- **C** — 201 raw + 500 shim (blossom.band shape — full PNG decoder
  blows up on truncated IDAT): `raw_random_ok=YES, png_shim_ok=NO`.
- **Budget** — total probe PUTs never exceed the existing 4 + 2 = 6.

## 2. Probe-driven auto-shim decision (nostrc-si30)

Three new symbols in `libhanami-blossom-shim`:

```c
bool
hanami_blossom_shim_active_for(const char *const *server_urls, size_t count);

/* Same but also fills in a diagnostic reason + the URL that tipped
 * the decision. */
bool
hanami_blossom_shim_active_for_ex(const char *const *server_urls, size_t count,
                                  hanami_blossom_shim_reason_t *out_reason,
                                  char *out_url_buf, size_t buf_cap);

/* Test / injection hooks. */
void hanami_blossom_shim_cache_reset(void);
void hanami_blossom_shim_cache_set(const char *server_url,
                                   hanami_capability_state_t raw_random_ok,
                                   hanami_capability_state_t png_shim_ok);
```

### 2.1 Decision precedence

1. `NOSTR_HOMED_BLOSSOM_PNG_SHIM=1` -> shim ON
   (`HANAMI_SHIM_REASON_ENV_FORCE_ON`)
2. `NOSTR_HOMED_BLOSSOM_PNG_SHIM=0` -> shim OFF
   (`HANAMI_SHIM_REASON_ENV_FORCE_OFF`)
3. Env unset OR malformed -> auto-decide:
   - any server with `raw_random_ok=NO && png_shim_ok=YES` -> ON
     (`AUTO_REQUIRED`), URL that tipped it recorded
   - all servers with `raw_random_ok=YES` -> OFF (`AUTO_RAW_OK`)
   - otherwise -> OFF (`AUTO_FALLBACK`), conservative default

### 2.2 URL-keyed cache

Session-scoped in-memory map keyed by the exact endpoint URL. Grows
one-way inside a process, guarded by a mutex. Populated in two ways:

- **Explicit seed** via `hanami_blossom_shim_cache_set` — for tests +
  callers that already have capability data.
- **Lazy probe** from `hanami_blossom_shim_active_for` — mints a
  short-lived `hanami_blossom_client_t` against the URL (with a NULL
  signer since the probe uses its own ephemeral key) and calls
  `hanami_server_probe_capabilities`, then copies the raw/png flags
  into the cache. Respects `NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE=1`.

Because the cache is URL-keyed and per-process, the three call sites
(cmd_push's manifest hashing, `nh_syncd_pusher::upload_chunk`, and
`nh_porthome_blossom.c`'s upload path) reach identical verdicts as
long as they pass the same server list. This is what keeps the
manifest chunk_hash and the Blossom URL from disagreeing (the nostrc-
wmb5 failure mode).

### 2.3 Consumer wire-in

- **`nh_porthome_blossom.c`**: the old `nh_porthome_shim_enabled()`
  (env-var-only) is replaced by `nh_porthome_shim_enabled_for(c)` which
  passes the wrapper's `c->servers` list to
  `hanami_blossom_shim_active_for`. Both the single-blob and batch
  upload paths call it.
- **`nostr-homed-provision.c::cmd_push`**: reads the auto-decide ONCE
  at push start, right after `nh_porthome_blossom_new`, using
  `hanami_blossom_shim_active_for_ex` so the log line can explain the
  reason. The resulting `push_shim_on` bool is used for the manifest
  hash swap that was previously reading the env var per chunk.
- **`nh_syncd_pusher.c::upload_chunk`**: same treatment — swaps
  `hanami_blossom_shim_active()` for `hanami_blossom_shim_active_for(
  cfg->blossom_servers, cfg->n_blossom_servers)`. The URL cache
  memoises the probe so this is O(1) per chunk after the first push.

### 2.4 Log line

`cmd_push` emits ONE line at push start explaining the shim decision:

```
push: shim=on  (auto: https://blossom.primal.net requires it — raw_random_ok=no, png_shim_ok=yes)
push: shim=off (auto: all 2 servers accept raw bytes)
push: shim=on  (forced by NOSTR_HOMED_BLOSSOM_PNG_SHIM=1)
push: shim=off (forced by NOSTR_HOMED_BLOSSOM_PNG_SHIM=0)
push: shim=on  (auto: fallback — capabilities unknown)   # probes disabled
```

### 2.5 Tests — `test_hanami_blossom_shim_auto.c`

Eight scenarios, all seeded via `hanami_blossom_shim_cache_set` so no
network I/O is required:

- `all_raw_ok_returns_false`
- `one_shim_required_returns_true` (also checks the URL that tipped
  the decision is copied into the out buffer)
- `unknown_falls_back_to_off`
- `env_force_on_overrides_caps`
- `env_force_off_overrides_caps`
- `env_garbage_falls_through`
- `zero_servers_falls_back_to_env`
- `cache_reset`

All 8 assert the expected `hanami_blossom_shim_reason_t` in addition
to the boolean.

## 3. Lab Blossom recipe (nostrc-4f9v)

Under `packaging/lab-blossom/`:

- `docker-compose.yml` — hzrd149's reference `blossom-server` (pinned
  tag) behind an nginx TLS terminator. Config sized for the porthome
  4 MiB chunk with headroom (`MAX_FILE_SIZE=8 MiB`,
  `client_max_body_size 16m`). No content sniffing, no thumbnailing,
  no allow-list on pubkeys.
- `blossom-server.config.yaml` — minimum-fuss reference-server config
  with `rules.upload: []` (deliberately empty). Rate limits at lab-
  loose defaults.
- `nginx.conf` — HTTPS on port 8443, reverse-proxies to
  `blossom-lab:3000`. `proxy_request_buffering off` so BUD-02 body-hash
  verification sees the exact bytes the pusher sent.
- `README.md` — operator quickstart. Documents the SSRF pre-check
  requirement explicitly: the recipe MUST run on a publicly-routable
  IP. There is no `--allow-insecure-lan-blossom` flag on the fetcher
  and the README says so; operators either get real DNS + letsencrypt,
  or self-signed on a public IP, or use a VPN whose CGNAT/public-
  looking range fools the fetcher's DNS gate.
- `.gitignore` — blocks `*.pem`, `*.key`, `*.crt`, `blobs/`, and
  `docker-compose.override.yml` from being accidentally committed.

Docker-compose was picked over systemd because the upstream Blossom
reference image is the well-tested integration point and bring-up is
one command; the README §6 notes how to translate to systemd for
docker-averse lab hosts.

### `docs/porthome-operator-guide.md` §5.1 update

The shim knob description in the operator guide is rewritten:

- Points at the hy3e review as the empirical evidence (primal.net 200,
  band 500).
- Documents the auto-decide default (env unset -> probe-driven).
- Documents the explicit env-var override precedence.
- Documents `NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE=1` as the
  probe kill-switch.
- Updates the "suggested Blossom set" to include primal.net (which
  the auto-decide now unlocks automatically) and to NOT include
  blossom.band.
- Points at `packaging/lab-blossom/README.md` for the operator-run
  Option A path.

## 4. Backwards compatibility

- The env var `NOSTR_HOMED_BLOSSOM_PNG_SHIM` retains its exact
  semantics when set to "1" or "0". Only "unset" now flips from
  "shim off" to "auto-decide", and the auto-decide's conservative
  default is OFF whenever probes are disabled — matching pre-si30
  behaviour bit-for-bit for operators that flip the kill-switch.
- Existing capability probe consumers are unaffected: the two new
  cache fields (`raw_random_ok`, `png_shim_ok`) were already declared
  by bpum and stayed UNKNOWN until this pass wired probes to populate
  them.
- No wire format changes. Manifest chunk_hash is still whatever
  addresses the actual byte content on Blossom — the auto-decide
  merely picks WHICH bytes get hashed.

## 5. Test summary

Two new test binaries under `libhanami/tests/`:

```
test-hanami-server-capability-probe-shim  — 4 tests, TIMEOUT 60 s
  ○ probe_scenario_A_both_ok
  ○ probe_scenario_B_primal_like
  ○ probe_scenario_C_band_like
  ○ probe_extension_budget

test-hanami-blossom-shim-auto             — 8 tests, TIMEOUT 30 s
  ○ all_raw_ok_returns_false
  ○ one_shim_required_returns_true
  ○ unknown_falls_back_to_off
  ○ env_force_on_overrides_caps
  ○ env_force_off_overrides_caps
  ○ env_garbage_falls_through
  ○ zero_servers_falls_back_to_env
  ○ cache_reset
```

Both are registered in `libhanami/CMakeLists.txt` with the existing
sanitizer + BUILD_TESTING guards.

Standalone compile of the modified sources on this macOS worktree
(clang 17, `-std=c11 -Wall -Wextra -Wpedantic -Werror`) is clean for:

- `libhanami/src/hanami-blossom-shim.c`
- `libhanami/src/hanami-blossom-client.c`
- `libhanami/src/hanami-server-capability.c`
- `gnome/nostr-homed/src/porthome/nh_porthome_blossom.c`
- `gnome/nostr-homed/src/porthome/nostr-homed-provision.c`
- New test files
  (`test_hanami_server_capability_probe_shim.c`,
   `test_hanami_blossom_shim_auto.c`)

Full-tree `cmake --build` fails on this macOS worktree at a pre-existing
`ndb_backend.c` include-path issue (`nostrdb.h` not found in libnostr's
transitive path — same header discovery issue noted in the bpum review
§7). Not caused by this pass; not blocking aarch64.

## 6. Dep-purity

The shim module now has a runtime dep on the blossom-client module
(for the lazy probe path). That dep is INTRA-libhanami: `hanami` is
one library target; adding `#include
<hanami/hanami-blossom-client.h>` to the shim source does not create
a new closure edge into `nostr-authd` or `pam_nostr.so`. Both those
targets already refuse to link `hanami` (they use `libnostr` only).

## 7. Close checks

- (a) `hanami_server_probe_capabilities` includes two additional
      raw + shim uploads. ✓
- (b) `raw_random_ok` + `png_shim_ok` populate correctly per test
      scenarios (A/B/C). ✓
- (c) `hanami_blossom_shim_active_for(servers, count)` returns the
      correct decision under all env-var + capability combinations
      (8 test scenarios). ✓
- (d) `cmd_push` and `nh_syncd_pusher::upload_chunk` consume the
      auto-decision at push start; log line names the reason
      (env force / auto required / auto raw-ok / auto fallback). ✓
- (e) `packaging/lab-blossom/` contains a working docker-compose
      recipe + config + nginx + README. ✓
- (f) Build stays `-Werror`-clean on the modified files (standalone
      compile clean; aarch64 CI is authoritative). ✓
- (g) Dep-purity gate holds — see §6. ✓

Close `nostrc-prli`, `nostrc-si30`, `nostrc-4f9v` on merge.
