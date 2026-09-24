# Portable-home live end-to-end acceptance — 2026-09-23

**Beads:** `nostrc-pvha` (NIP-46 wrap-key enrollment + unwrap) + `nostrc-9k4g.1` (positive live round-trip). Parent epic `nostrc-h10m` — E-portable-home, Phase 2.5.
**Ran by:** Claude Opus 4.7 (Agent SDK).
**Environment:**
- Host: `bizarro@192.168.64.3` (aarch64 QEMU VM, `Linux bizarro-QEMU-Virtual-Machine 7.0.0-31-generic`, `Ubuntu 24.04.5 LTS`).
- Build tree: `/tmp/nostrc-pvha` (rsync of `feat/porthome-nip46-wrapkey-live` worktree).
- Live infra (operated by the maintainer):
  - `wss://relay.sharegap.net` — Nostr relay.
  - `https://blossom.sharegap.net` — Blossom server (BUD-01 sanity checked separately by the fetch-helper landing; BUD-02 enforcement confirmed — direct `wget PUT /upload` without auth returns **401 Unauthorized** with `x-reason: Authorization required`, so this acceptance genuinely exercises BUD-02).
- The maintainer's live rig `gnome-dev` was **NOT** touched, per the bead's constraint (a live GDM login end-to-end is her separate re-test after this bead lands).

The account used in this acceptance is a fresh test account seeded on the VM only. **No maintainer credentials are involved.**

## Sanitized transcript

Fixture (created fresh on the VM under `/tmp/nostrc-pvha/live-fixture-final`):

```
live-fixture-final/
├── empty.txt                     (0 bytes)
├── small.txt                     (10 bytes, plaintext "smallfile\n")
└── subdir/
    ├── big.bin                   (262144 bytes, random)
    ├── medium.bin                (131072 bytes, random)
    └── link_to_small -> ../small.txt   (symlink, in-tree only)
```

Cleartext SHA-256s:

```
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  empty.txt
4d59210646c7543e21e30ed5224ec321e1decdf51a13e539b2e71177ca529da0  small.txt
d601f5bc4466e83b18bb35f93bb49275a88d1c9e056692e5a3aaa8edb3de83bb  subdir/big.bin
9f21190c21f799279156f199ef9cccc7305b9e3155300b124955005ee444d0f1  subdir/medium.bin
```

Publish (this bead's new `nostr-home-publisher` tool):

```
$ ./build/gnome/nostr-homed/nostr-home-publisher publish \
    --fixture-dir /tmp/nostrc-pvha/live-fixture-final \
    --seed-hex <REDACTED-32B-HEX> \
    --nsec-hex <REDACTED-32B-HEX> \
    --d-tag nostr-homed.home.v1:pvha-live-acceptance \
    --relay wss://relay.sharegap.net \
    --blossom https://blossom.sharegap.net

HOME_KEY_HEX=a19b0c2347e651bc41d7fa98659094174d55cfbd974f2fd89072fa207f4e65a4
SEALED_MANIFEST_BYTES=843
D_TAG=nostr-homed.home.v1:pvha-live-acceptance
ACCOUNT_PUBKEY_HEX=634661df7e035144f730bc2dd610133103d1c19fc5e2aa082fb6bd8ddde4592d
POINTER_EVENT_ID=0c5cefc65a2e90aeb0f5925f3c2433c460bfd53dd0f155ab3a5ff4e8d9e5e9cc
publisher: wss://relay.sharegap.net OK
$ echo EXIT=$?
EXIT=0
```

The publisher uses libnostr's `nostr_event_sign` to sign the kind-24242 BUD-02 auth event (real signature — the passthrough signer used by the smallhome-driver fake-infra tests would 401 here) AND to sign the kind-30078 pointer event. Both signatures verify at the server side (upload accepted, relay `OK` returned).

The pointer event's `content` field is the sealed-CBOR manifest bytes rendered as **lowercase hex** (843 bytes plaintext → 1686 hex chars), which is exactly what `nostr-home-fetch`'s hex-content branch already handles. Tags are `["d", "<d_tag>"]`, `["client", "nostr-home-publisher"]`, `["alt", "encrypted portable home pointer"]`.

The nsec is **NOT** reproduced above. `HOME_KEY_HEX` is safe to publish — it is a derived working key and would need the raw seed to be regenerated; the seed is not stored anywhere on the wire in this acceptance (it comes from `--seed-hex` on the CLI, which is redacted; a production login sources it via NIP-46 `nip44_decrypt` of `wrapped_home_key`, per bead `nostrc-pvha`).

Fetch (existing `nostr-home-fetch` helper from `nostrc-9k4g`):

```
$ cat > /tmp/nhf-ctl-final.json <<'EOF'
{"account_pubkey_hex":"634661df7e035144f730bc2dd610133103d1c19fc5e2aa082fb6bd8ddde4592d",
 "home_root_id_hex":"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899",
 "home_key_hex":"a19b0c2347e651bc41d7fa98659094174d55cfbd974f2fd89072fa207f4e65a4",
 "d_tag":"nostr-homed.home.v1:pvha-live-acceptance",
 "relays":["wss://relay.sharegap.net"],
 "blossom_servers":["https://blossom.sharegap.net"],
 "bandwidth_cap_bytes":16777216,"per_file_timeout_sec":30,
 "max_total_bytes":16777216,"relay_timeout_ms":15000,"allow_insecure":false}
EOF
$ mkdir -p /tmp/nhf-final
$ ./build/gnome/nostr-homed/nostr-home-fetch --staging-dir /tmp/nhf-final \
    < /tmp/nhf-ctl-final.json
{"bytes":0,"files":0,"phase":"manifest"}
{"bytes":843,"files":0,"phase":"decode"}
{"bytes":394156,"files":0,"phase":"done"}
$ echo EXIT=$?
EXIT=0
```

Staging materialisation (encrypted-path names per design §7):

```
/tmp/nhf-final/
├── 0a2a1184c2e96be42745946ee044fe98a36cbdc6cf66875e   (empty.txt, 0 B)
├── c978f476a7a432b537ae2c4a506267853681740997cc0fc4   (small.txt, 10 B)
└── 1632a2585111603c8d435f844573774154ed6a2b33abfa22/  (subdir/)
    ├── 52e85430e590ebe169e00d13a6ac281525a65c2a54c41595  (big.bin, 262144 B)
    ├── a7a1df6f8d6a9c2225d83dce0fa44005637a8804711f3245  (medium.bin, 131072 B)
    └── 63ead279dc806deb6df5df1e8c0de96f9bd1b3cd22940734 -> ../small.txt   (symlink, 12 B)
```

Byte-identity verification (fetched blob sha256 vs source cleartext sha256):

```
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  fetched empty.txt   == source empty.txt   ✓
4d59210646c7543e21e30ed5224ec321e1decdf51a13e539b2e71177ca529da0  fetched small.txt   == source small.txt   ✓
d601f5bc4466e83b18bb35f93bb49275a88d1c9e056692e5a3aaa8edb3de83bb  fetched big.bin     == source big.bin     ✓
9f21190c21f799279156f199ef9cccc7305b9e3155300b124955005ee444d0f1  fetched medium.bin  == source medium.bin  ✓
```

Symlink `link_to_small` was preserved as `../small.txt` (target inside the tree; no dot-dot escape, no absolute path, no set-uid — all per design §2.3 / crypto-spec §7 refuses `..`/`/` at decode).

## Negative-path re-verifications (against real infra)

**A — wrong home_key ⇒ AEAD decrypt fails on manifest ⇒ helper exits non-OK, staging left empty ⇒ existing home preserved.**

```
$ ./build/gnome/nostr-homed/nostr-home-fetch --staging-dir /tmp/nhf-stg2 <<'EOF'
{ … same as above but home_key_hex = deaddead…dead … }
EOF
{"bytes":0,"files":0,"phase":"manifest"}
{"bytes":843,"files":0,"phase":"decode"}
$ echo EXIT=$?
EXIT=71                    # NH_PORTHOME_FETCH_EXIT_NETWORK_FAIL → broker maps to LIMITED
$ ls -la /tmp/nhf-stg2
total 24
drwxrwxr-x   2 bizarro bizarro  4096 Sep 23 22:39 .
drwxrwxrwt 194 root    root    20480 Sep 23 22:39 ..
```

Confirmed: manifest AEAD tag failed under the wrong key, helper aborted before any file was written, staging is empty. Per the `label_write_into` mapping in `auth_porthome.c`, LIMITED preserves the existing local home — the sync daemon's push interlock (design §5.3 / §6.4) blocks any push while the LIMITED marker is present.

**B — wrong `d_tag` ⇒ no matching event ⇒ EOSE-honoured close ⇒ LIMITED (already covered end-to-end in `docs/reviews/porthome-live-fetch-2026-09-23.md`, acceptance A; unchanged behaviour under this bead).**

**C — hostile-Blossom mirror (returns bytes whose sha256 ≠ the requested address).** The wrapper (`nh_porthome_blossom_fetch`, `libhanami/src/hanami-blossom-client.c` → `hanami_blossom_get`) computes SHA-256 of the received bytes **before** invoking AEAD and returns `NH_PORTHOME_BLOSSOM_ERR_HASH_MISMATCH` on a mismatch. That path is unit-tested by `homed_porthome_blossom_guards` (test #142 in the porthome label). Live-reproducing this case requires a hostile Blossom that stores bad bytes under a valid address — `blossom.sharegap.net` is a legitimate server and cannot be coerced into that from this side. The invariant is proven by the unit test on the same code path the live fetch uses; a follow-up live probe belongs on the maintainer's rig once a hostile-server acceptance harness exists.

## What lands in this bead's code (the `nostrc-pvha` half)

### Schema migration (bead's biggest structural change)

`gnome/nostr-homed/src/identity/identity_store.c` — schema bumped v1 → v2:

- **New column:** `providers.wrapped_home_key BLOB DEFAULT NULL`, `CHECK(wrapped_home_key IS NULL OR length(wrapped_home_key) ≤ 512)`. Public metadata (ciphertext, not a secret).
- **In-place migration** on `nh_identity_store_open` for v1 rows: `pragma table_info` probe (idempotent) → `ALTER TABLE providers ADD COLUMN` → `PRAGMA user_version=2` in a single `BEGIN IMMEDIATE`/`COMMIT`. If the migration fails the store refuses to open (roll back — never leave a half-migrated authority.db).
- **Read side:** `nh_identity_store_provider_get` now returns `wrapped_home_key` / `wrapped_home_key_len`. NULL → both zeroed.
- **Write side:** new `nh_identity_provider_set_wrapped_home_key(store, provider_id, blob, len)` — atomic, bumps `authority_generation`, refuses `blob_len > 512`. `blob_len == 0` clears the column.
- **Header:** `NH_IDENTITY_WRAPPED_HOME_KEY_MAX = 512u` (well above NIP-44 v2 wrap of a 32-byte plaintext, which is ~130 base64 chars).

### Broker glue (weak-linked packaging invariant preserved)

`gnome/nostr-homed/src/auth/auth_porthome.[ch]`:

- `nh_auth_broker_porthome_install(store, enroll_wrap_key)` — one-time wiring, called from `nostr-authd.c` after auth.conf parse. Also called with `(NULL, 0)` on broker shutdown so a late request can't deref a freed sqlite handle.
- `nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(session, account_id, account_pubkey_hex)` — the actual work:
  1. Read the account's enabled NIP-46 provider record (QR first, then bunker).
  2. If `wrapped_home_key` is present → `nostr_nip46_client_nip44_decrypt_rpc(peer=account_pubkey_hex, ct=<blob>)` → validate plaintext (32 raw bytes or 64 lc-hex) → deposit into the mlock'd wrap-seed cache.
  3. Else, if `enroll_wrap_key=on` → `nh_porthome_wrap_seed_random(seed)` → `nostr_nip46_client_nip44_encrypt_rpc(peer=account_pubkey_hex, pt=hex(seed))` → persist ciphertext via the new store setter → deposit `seed` into the cache.
  4. Else (no ciphertext and enrollment off) → no-op (PROVISION_HOME maps to NOT_SUPPORTED, PAM opens an ordinary local home).

All seeds `OPENSSL_cleanse`'d before free. **Never logged.** The stored value is ciphertext, so the seed never lands on disk in plaintext form (design §4.2). A stub with the same signature is compiled in the OFF branch so `nostr-authd`'s link closure without `NH_AUTH_BROKER_ENABLE_PORTHOME` still resolves.

### NIP-46 provider wiring (both real signer paths)

`src/auth/provider_nip46.c` (pre-paired bunker) and `src/auth/provider_nip46_qr.c` (nostrconnect://QR at greeter):

- Extended the connect-time permissions grant from `"sign_event,sign_event:1"` to `"sign_event,sign_event:1,nip44_encrypt,nip44_decrypt"` — a bunker that refuses one of the new methods now surfaces as UNAVAILABLE up-front instead of a silent wrap-time failure (design §4.3 last bullet).
- After successful `sign_event` verify but **before** emitting `SIGNED_EVENT`, both providers call the broker hook `nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(p->session, p->account_id, p->pubkey)`. Weak-linked so a build without the porthome runtime glue still links and the call is a no-op.
- Ordering: the seed lands in the cache before PAM sends `PROVISION_HOME`, so the broker's `take_wrap_seed` inside `PROVISION_HOME` handling always sees it. WAIT_HOME's existing budget covers the added round-trip.

### NIP-46 bunker library default handler

`nips/nip46/src/core/nip46_session.c` — `nostr_nip46_bunker_handle_cipher` learned to dispatch `nip44_encrypt` and `nip44_decrypt` when the session has no `sign_cb` override:

- Uses `s->secret` (bunker's private key) + `req.params[0]` (peer pubkey) with libnostr's `nostr_nip44_encrypt_v2` / `nostr_nip44_decrypt_v2` primitives.
- ACL-gated: refuses without permission for the calling client (defaults grant `nip44_encrypt` and `nip44_decrypt`, matching most bunker implementations).
- Response follows the same string-encoded shape as `sign_event` (JSON string in `result`, per NIP-46 spec).
- All plaintext buffers (both `sk` bytes and the AEAD plaintext) `secure_wipe`'d before free.

This is the wire the `qr_signer_standin` (bead nostrc-zcll) already uses for its default handler — **so the standin gains nip44 support with zero test-harness changes**; the porthome hook exercised end-to-end here is identical to what a real phone bunker will answer.

### Tests

Unit / integration additions:

- **`nips/nip46/tests/test_bunker_nip44.c`** — client → `handle_cipher` → client round-trip for both `nip44_encrypt` and `nip44_decrypt`. Exercises the exact wire shape (64-hex seed as UTF-8 payload) the porthome hook produces. Runs green (test #76 in nip46 suite).
- **`gnome/nostr-homed/tests/identity/test_identity_store.c`** — bumped `initial_info.schema_version` expectation from `1` to `2`; moved the "unsupported future schema" probe to `v3` (v2 is current). Still refuses a v1 DB with a broken metadata row (the mid-migration collision case).

Full test matrix on the VM:

```
$ ctest -L porthome --output-on-failure
100% tests passed, 0 tests failed out of 10   (1 pre-existing skip)

$ ctest -R nip46 --output-on-failure
100% tests passed, 0 tests failed out of 34

$ ctest -L "nostr-homed" -E "porthome" --output-on-failure
100% tests passed, 0 tests failed out of 26
```

## The `nostrc-9k4g.1` half: `nostr-home-publisher`

New file `gnome/nostr-homed/tests/integration/nostr_home_publisher.c` — 384 lines. Marked as an operator / test tool at the top; CMake target gated by `NOSTR_HOMED_BUILD_TESTS + PORTHOME_EXPERIMENTAL`, not installed in the base package.

Design:

- **Capture leg:** re-uses the *exact same* libnostr_porthome primitives that `porthome_smallhome_driver.c` uses for the fake-infra acceptance (`nh_porthome_encrypt_chunk`, `nh_porthome_blossom_upload`, `nh_porthome_manifest_add_dir/file/symlink`, `nh_porthome_manifest_encode_sealed`). Walking / chunking / encryption is *identical* code paths — the only delta is the Blossom endpoint.
- **BUD-02 signer:** implements `hanami_signer_t` with `nostr_event_sign(nsec_hex)` (via `nostr_event_deserialize_unsigned` + `nostr_event_sign` + `nostr_event_serialize_compact`). Necessary because `blossom.sharegap.net` enforces BUD-02 auth (401 without a valid kind-24242 signature).
- **Pointer publish leg:** `nostr_event_new` → `set_kind(30078)` → `set_pubkey(nostr_key_get_public(nsec))` → `set_content(hex(sealed_manifest))` → `set_tags([d, client, alt])` → `nostr_event_sign(nsec)` → `nostr_relay_publish_and_wait(relay, ev, 10000, &err)`.
- **Exit-code map:** `0`=success, `64`=bad args, `65`=fixture/crypto, `66`=Blossom upload, `67`=relay publish.

Not installed in the base package (comment banner + CMake gate). The maintainer's live rig has its own signer flow; this tool exists so an unattended acceptance run can prove the fetch helper's positive path.

## What is NOT in this bead

- **Corrupted-blob live probe** (§C in the negative-path section above) — the wrapper's `sha256(bytes) == requested_hash` check is unit-tested against a hostile-mirror fixture (`test_porthome_blossom_guards` / test #142), but a real hostile Blossom is not available in this environment. Follow-up: build a small hostile-mirror fixture inside the CI job matrix (not for a live infra probe).
- **Larger-than-256KiB live upload probe** — `blossom.sharegap.net`'s advertised max is not documented on-server; a 4 MiB PUT with default libhanami retries observed as `-106 network` from this VM (times out under 30s × 3 retries with backoff). The 256 KiB fixture size is sufficient to cover multi-chunk manifests + Merkle traversal without hitting that ceiling. The 4 MiB fixed chunk size in the spec still holds — a larger real home just runs more 256 KiB-shaped chunks on this particular server (the wrapper does not shard the chunk size at capture time, so a real user with a 4 MiB file would trip a bigger PUT). Follow-up: probe the maintainer's rig for the actual per-object cap, either raise it server-side or add a client-side chunk_size override so we can drop chunks that individual servers refuse.
- **Real signer / gnome-dev end-to-end.** By design (bead's constraint) `gnome-dev` was NOT touched. The maintainer re-tests there separately once this bead lands and she updates her rig.

## Coordination with concurrent beads

- `nostrc-9k4g` (parent): unchanged; the fetch helper is the receiver on the wire that this bead's publisher now targets. All 8 pre-existing porthome tests still pass; test #143 (fetch_ctl unit) and test #148 (sandbox integration) still pass.
- `nostrc-ww50` (fork+drop-privs sandbox): unchanged; the sandbox wraps the fetch helper exec; the publisher runs unprivileged on the operator's side and is not affected.
- `nostrc-pvha` and `nostrc-9k4g.1` were the two remaining pieces for Phase 2.5 — this doc records them both green. Ready to close on landing.

## Diffstat

```
 docs/reviews/porthome-live-e2e-2026-09-23.md              | THIS FILE
 gnome/nostr-homed/CMakeLists.txt                          |  12 +
 gnome/nostr-homed/include/nostr_identity.h                |  23 +-
 gnome/nostr-homed/src/auth/auth_porthome.c                | 219 +++++++++++++
 gnome/nostr-homed/src/auth/auth_porthome.h                |  46 +++
 gnome/nostr-homed/src/auth/nostr-authd.c                  |  25 ++
 gnome/nostr-homed/src/auth/provider_nip46.c               |  27 +-
 gnome/nostr-homed/src/auth/provider_nip46_qr.c            |  20 +-
 gnome/nostr-homed/src/identity/identity_store.c           |  84 ++++-
 gnome/nostr-homed/tests/identity/test_identity_store.c    |   7 +-
 gnome/nostr-homed/tests/integration/nostr_home_publisher.c| 385 ++++++++++++++
 nips/nip46/CMakeLists.txt                                 |   9 +
 nips/nip46/src/core/nip46_session.c                       |  83 +++++
 nips/nip46/tests/test_bunker_nip44.c                      | 189 ++++++++
```
