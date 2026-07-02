# Signet Passkeys (FIDO2 / WebAuthn) Feasibility Plan

> Status: Feasibility assessment (feasibility-first; phased roadmap contingent on a "go" decision).
> Scope locked with user: signet as a **FIDO2/CTAP2 authenticator + passkey vault**; programmatic agent-facing API first, virtual CTAP device later; **software-backed keys with a pluggable-hardware seam**; **syncable multi-device credentials** (`BE`/`BS` set, backed by a fleet export/sync path).

## Goal

Determine whether signet — the headless NIP-46 "bunker" signer/vault for agent fleets — can act as a **FIDO2 authenticator** that creates, stores, and uses WebAuthn passkey credentials on behalf of agents, and if so, define what it would take. This extends signet's "replace the password manager / NIP-07 / NIP-46 for headless agents" mission to the FIDO passkey ecosystem.

## Background

### What signet is today
- NIP-46 bunker daemon (`signetd`) for autonomous agent fleets; signing exposed via **relay-based NIP-46** and a local **`net.signet.Signer` D-Bus** interface (`GetPublicKey`/`SignEvent`/`Encrypt`/`Decrypt`) with UID→`agent_id` authorization (`signet/README.md:1`, `signet/dbus/net.signet.Signer.xml:6`, `signet/src/dbus_unix.c:101`).
- Already a **generic secrets vault**: `secrets` table with types `nostr_nsec`, `ssh_key`, `api_token`, `credential`, `certificate` (`signet/include/signet/store_secrets.h:22`, `signet/src/store.c:112`). **Caveat:** the `secrets` table has a `UNIQUE` index on `agent_pubkey` (`store.c:141`) → **one secret row per agent** — it cannot hold N passkeys per agent, so passkeys need their own table (see Approach).
- **Encryption is a single global DEK**, not per-agent: one key derived with domain `"signet-agent-nsec-v1"` (`store.c:42`) protects all rows. Passkey keys will inherit the same blast radius as agent nsecs — acceptable for the software-backed tier, and the exact reason for the pluggable-hardware seam.
- README already anticipates a **`net.signet.Credentials`** D-Bus interface and a Credential Management roadmap (`signet/README.md:57`, `signet/README.md:287`).

### Key extension points (where passkey code would attach)
- **Secrets schema**: static SQL `SIGNET_SCHEMA_SQL` (`signet/src/store.c:112-211`); additive migrations run after creation (`store.c:258-265`).
- **Vault pattern to mirror** (not reuse, per the caveat above): `secret_type` enum + string map (`store_secrets.h:22`, `store_secrets.c:18-38`); `put`/`get`/`list` API (`store_secrets.h:44-82`).
- **Envelope encryption to reuse**: `signet_store_get_dek()` + libsodium `crypto_secretbox_easy` (`store.c` DEK domain `"signet-agent-nsec-v1"` at `store.c:42`; encrypt shape at `store_secrets.c:47-68`).
- **D-Bus method wiring** (XML → runtime introspection string → GDBus vtable → dispatch): example `SignEvent` at `net.signet.Signer.xml:11`, `dbus_unix.c:125-165`, `dbus_common.c:469`.
- **NIP-46 dispatch**: string-compare branches in `signet_nip46_server_handle_event()` (`nip46_server.c:533-651`).
- **Build**: add `.c` to `SIGNET_CORE_SOURCES` (`signet/CMakeLists.txt:39-66`); optional features gated by pkg-config blocks.

### What FIDO2 requires (external spec facts)
- **Mandatory crypto**: ES256 (COSE alg `-7`, ECDSA **P-256** + SHA-256) is the practical must-have; EdDSA `-8` / RS256 `-257` optional. **signet has no P-256 today** (only secp256k1/Schnorr + libsodium Ed25519/AES-GCM/HKDF) — this is the single biggest missing primitive.
- **Core CTAP2 commands**: `authenticatorMakeCredential`, `authenticatorGetAssertion`, `authenticatorGetInfo` (+ `GetNextAssertion` for multiple matches). `authenticatorClientPIN`/UV optional — but CTAP2.2 requires PIN or built-in UV if `rk=true` (discoverable creds) is advertised.
- **Data formats**: CTAP2 canonical **CBOR**; credential public keys are **COSE_Key**. `authenticatorData = rpIdHash(32) || flags(1) || signCount(4) || [attestedCredentialData]`; assertion signs `authData || clientDataHash`. Recommended C lib: **libcbor** (already a libfido2 dep). None vendored in-repo today.
- **Attestation**: `fmt:"none"` generally accepted for consumer passkeys; a 16-byte AAGUID is still required by `getInfo` (software authenticators use a fixed/zero AAGUID).
- **No off-the-shelf authenticator lib**: **libfido2 is client/platform-side only**. Authenticator-side CTAP2/CBOR/COSE/state/HID must be hand-rolled; SoloKeys `solo1` firmware is the closest C reference.
- **Virtual device path (later phase)**: to be seen by unmodified browsers on Linux → USB-HID / CTAP-HID framing via `/dev/uhid`. Platform-specific and heavier.

### Prior art
- No existing FIDO/WebAuthn/CTAP/COSE/CBOR anywhere in the repo (`docs/plans`, `docs/proposals`, `docs/designs`, `docs/audits` all clear).
- `pkcs11/` is a non-functional p11-kit stub (`pkcs11/gnostr-p11.c` returns `CKR_FUNCTION_NOT_SUPPORTED`); real HSM/hidapi/p11-kit/keystore plumbing lives in the separate `apps/gnostr-signer`, not signet — a reference for the later pluggable-hardware seam.

## Approach

**Verdict: conditional GO.** Signet is a good structural fit for a software passkey vault — it already is an encrypted, policy-gated, per-agent secret store with multiple request transports. Nothing in the architecture blocks it. The "go" is *conditional on a Phase 0 spike* that proves three things at once: the crypto/encoding stack (ES256, canonical CBOR, COSE_Key, authenticatorData) is byte-correct, the N-per-agent storage model fits, and a real relying party (`python-fido2`) actually completes a register+authenticate against a headless authenticator. Everything past Phase 0 is ordinary integration work; FIDO interop is unforgiving, so we gate on it before building the rest.

### Recommended architecture (in signet terms)
A new **`signet_fido` passkey subsystem** sits between the existing transports and the existing vault, so ceremony logic is written once:

**Portability (sync/backup) model.** Credentials are wrapped under a fleet-shared PSK and are movable via a versioned, self-describing **export container** (`credential_id`, `rp_id`/`rp_id_hash`, `user_handle`, `discoverable`, `aaguid`, PSK-wrapped private key, `alg -7`). Export/import is the primitive; a signet instance can hand a credential to a peer as a container over any existing transport. On top of that primitive, live sync is layered: the **default is orchestrator-driven** (the fleet manager calls `Export`/`Import` — zero new protocol, cross-host, matches signet's "policy lives outside, signet executes" model), with an **optional native built-in** of encrypted Nostr events over signet's existing relay transport for hands-off fleet replication. The offline container alone is the MVP and is enough to honestly justify `BE`/`BS`.

- **Crypto — use OpenSSL.** ES256/P-256 is mandatory and **libsodium has no P-256**. OpenSSL is *already* a signet dependency, so it's the zero-new-dependency choice. mbed TLS / micro-ecc only matter if a future hardware backend needs them; `libsecp256k1` is irrelevant (wrong curve).
- **Encoding — add `libcbor` behind the feature gate.** CTAP2/COSE need canonical CBOR; don't hand-roll it. Not vendored today.
- **Storage — a dedicated, self-contained `passkey_credentials` table** (the existing `secrets` table's `UNIQUE(agent_pubkey)` forbids N-per-agent, so we do *not* reuse it). Add the table to `SIGNET_SCHEMA_SQL` (`store.c:112-211`) holding the lookup fields a discoverable authenticator must keep (`credential_id`, `agent_id`, `rp_id`, `rp_id_hash`, `user_handle`, `sign_count`=0, `aaguid`, `discoverable`, timestamps) **and** a **PSK-wrapped** `payload`+`nonce` for the private key material. Reuse the `crypto_secretbox_easy` *primitive* (`store_secrets.c:47-68`) but keyed by the fleet sync key, not the local DEK, so rows are portable.
- **Key-backend seam from day one.** A small interface (`generate` / `sign` / `export_public_key`) with a software (OpenSSL) impl now; a future hardware/enclave impl stores only an opaque handle in the payload. This is the user-requested "software now, pluggable hardware later."
- **Passkeys are backup-eligible & synced (decided): set `BE`+`BS`.** signet presents *multi-device* credentials — the `BE` (Backup Eligible) and `BS` (Backup State) flag bits are set in `authenticatorData`, backed by a real sync path (see Portability). Two consequences flow from this and are now design invariants:
  - **Credential keys are wrapped under a fleet sync key (PSK), not the per-instance DEK.** The PSK is a separately-provisioned fleet secret (like `SIGNET_DB_KEY`), so an encrypted credential row is portable to any signet instance in the fleet. At rest the DB is still SQLCipher-protected; the PSK wrapping is what makes the payload *movable*. The instance-local DEK path (`signet_store_get_dek()`) remains the reference for non-synced material.
  - **`signCount` is fixed at 0.** A synced credential cannot maintain a monotonic counter across instances; WebAuthn L3 says such authenticators SHOULD report 0. This removes the atomic-increment requirement entirely (simpler, and avoids false clone-detection alarms at RPs).
- **Identity — fixed non-zero signet AAGUID + `none` attestation (decided).** This is exactly how synced passkeys actually ship: iCloud Keychain and Google Password Manager both report a *fixed vendor AAGUID* with **no attestation** — the AAGUID is a provider-identification hint, not cryptographic proof. signet does the same: assign and freeze one stable AAGUID UUID (`uuidgen` at implementation, then hardcode), default attestation `none`. Caveat: under `none`, a browser suppresses the AAGUID to the RP (W3C WebAuthn §5.1.3), so AAGUID allow-listing is not guaranteed on the virtual-device path — the programmatic path returns authData directly, so cooperating callers always see it. If an RP ever needs cryptographic "this came from signet" proof, add an optional **`packed` basic-attestation** backend (x5c from a signet attestation CA) and only then pursue a FIDO Metadata Service listing — heavyweight and out of scope for MVP.
- **Transports call one service.** Programmatic first: a new **`net.signet.Passkeys`** D-Bus interface (sibling to the token-oriented `net.signet.Credentials`, which stays untouched) plus NIP-46 methods `webauthn_get_info` / `webauthn_make_credential` / `webauthn_get_assertion`, both dispatching into `SignetFidoService`. Signet is the **authenticator only** — challenge generation and assertion *verification* stay with the RP/caller.
- **Virtual CTAP-HID (`/dev/uhid`) is Phase 4, optional, Linux-only** — it makes unmodified browsers see signet as a real authenticator but adds CTAP-HID framing, channel state, and honest UP/UV/PIN semantics. Not part of the feasibility gate.

### Top 3 risks
1. **Crypto/encoding correctness** — P-256 DER signatures, COSE_Key shape, canonical CBOR, and exact `authenticatorData` byte layout are interop-fragile. *Mitigation: Phase 0 is a fixture-driven spike; it's the go/no-go.*
2. **Headless presence is a fabrication by design, not an edge case.** Every usable WebAuthn ceremony sets **UP=1** (user present), so a headless daemon asserts presence in *100%* of ceremonies with no human involved — this is inherent to the product, and the plan owns it explicitly rather than pretending otherwise. On top of that, **UV** (`userVerification:"required"`) must **default-deny** unless an explicit `allow_headless_uv` policy opts in — and some RPs will still reject a headless authenticator outright. *Mitigation: the "presence" a signet passkey attests is really "the signet policy engine authorized this agent"; make that substitution explicit in docs, gate UV behind policy, audit every ceremony. Whether real RPs accept this is a Phase 0 question, not a Phase 3 discovery.*
3. **Virtual-device scope creep** — Phase 4 can balloon (kernel uhid, CTAP-HID, PIN/UV). *Mitigation: fully deferred and independently gated; the programmatic API delivers the agent value without it.*

## Work Items

Phases are sequential; Phase 0 gates everything.

### Phase 0 — Feasibility spike (the go/no-go gate)
The gate must probe the *novel* risks, not just the well-solved crypto. OpenSSL P-256 + libcbor are the easy part; the real unknowns are the storage cardinality and whether a real RP accepts a headless authenticator — so both are pulled into Phase 0.
1. Add a `signet_enable_passkeys` build option (default off) + `SIGNET_ENABLE_PASSKEYS` symbol; require `libcbor` only when enabled — in **both** build systems (`signet/meson.options`, `signet/meson.build`, `signet/CMakeLists.txt:39-66`).
2. Prototype the ES256 backend on OpenSSL: generate P-256, export x/y, sign `SHA256(authData‖clientDataHash)` as DER, verify — behind a key-handle interface (`include/signet/fido_crypto.h`, `src/fido_crypto_openssl.c`, `tests/test_fido_crypto.c`).
3. Prototype COSE_Key (EC2: `kty=2,alg=-7,crv=1,x,y`) and a `fmt:"none"` attestation object via libcbor; assert byte-stable output against fixtures (`include/signet/fido_cbor.h`, `src/fido_cbor.c`, `tests/test_fido_cbor.c`).
4. **Storage-fit + portability probe:** stand up the dedicated `passkey_credentials` table and prove N-credentials-per-agent write / lookup-by-`rp_id`, **plus a PSK-wrap round-trip** (wrap a key under the fleet PSK on "instance A", unwrap and use on "instance B"). Validates both the cardinality fix and the sync decision before "go."
5. **End-to-end RP ceremony — the real gate:** drive a full *register + authenticate* against `python-fido2` acting as RP **and** client (not just standalone signature verification), and observe how it treats a headless UP=1 / no-UV authenticator. Record go/no-go here; if no-go, stop.

### Phase 1 — Storage model + passkey vault
1. Add the **self-contained `passkey_credentials` table** (lookup fields + encrypted `payload`/`nonce` columns) via an additive migration, with indexes on `(agent_id, rp_id)` and `credential_id` (`store.c:112-211`, migration at `store.c:258-265`). Do **not** touch the one-row-per-agent `secrets` table or its type enum.
2. Add a passkey store module — create-in-one-transaction, find by `agent_id+rp_id` / `credential_id`, excludeCredentials check; **`sign_count` is stored as 0 (no increment — synced credential)**; wrap the key blob with the **fleet PSK** via `crypto_secretbox_easy` (`include/signet/store_passkeys.h`, `src/store_passkeys.c`, `tests/test_store_passkeys.c`).
3. Define a **versioned** payload (backend id, key blob-or-handle, COSE public key, RP/user metadata, alg `-7`); private material only ever lives in the encrypted column.
4. Register the new tests in both build files (`tests/CMakeLists.txt`, `tests/meson.build`).

### Phase 2 — Programmatic API + policy/consent + wiring
1. Add `SignetFidoService` owning store + key backend + audit + AAGUID + headless-UV config; synchronous `get_info` / `make_credential` / `get_assertion` returning structured errors (`include/signet/fido.h`, `src/fido.c`).
2. Implement `MakeCredential`: validate 32-byte `clientDataHash`, RP id, user handle, ES256 in `pubKeyCredParams`, excludeList; generate credential id + keypair; build authData with **AT + BE + BS** flags, the frozen signet AAGUID, and `signCount=0`; attestation per config (default `none`; optional `packed` basic/x5c later); persist before returning.
3. Implement `GetAssertion`: validate inputs, select by allowList or discoverable `agent_id+rp_id`, build authData with **BE + BS** flags (no AT), `signCount=0` (no counter), sign `authData‖clientDataHash`, return credentialId/authData/DER-sig/userHandle.
4. Add capabilities `passkey.get_info|make_credential|get_assertion|manage`, map D-Bus + NIP-46 method names; **do not** add them to the wildcard default policy (`capability.h/.c`, `signetd_main.c`, `policies.toml.example`).
5. Audit every ceremony via an additive `SIGNET_AUDIT_EVENT_PASSKEY_CEREMONY` — agent, method, rpIdHash, credIdHash, decision, reason, UV mode; never log keys/payload/full clientData (`audit_logger.h/.c`, `src/fido.c`).
6. Add the `net.signet.Passkeys` D-Bus interface (`GetInfo`/`MakeCredential`/`GetAssertion`), extend the dispatch context, return `NotConfigured` when disabled (`dbus/net.signet.Signer.xml`, `dbus/net.signet.conf`, `dbus_unix.c`, `dbus_tcp.c`, `dbus_common.h/.c`).
7. Add NIP-46 branches `webauthn_get_info|make_credential|get_assertion`, keeping `signet_policy_engine_eval()` before execution; JSON payloads mirror the D-Bus API (`nip46_server.h/.c:533-651`, `signetd_main.c`, `INTEGRATION.md`). NIP-5L exposure optional/same methods.
8. Add `[passkeys]` config (`enabled=false`, `backend=software-openssl`, `aaguid`=frozen signet UUID [overridable to zero for max-privacy], `attestation=none`, `allow_headless_uv=false`, **`sync_key`/`sync_key_file` for the fleet PSK**) + lifecycle init/free (`signet_config.*`, `signetd_main.c`, `signet.conf.example`, `README.md`).

### Phase 2b — Credential portability (sync/backup)
1. Define the **versioned export container** (self-describing: `credential_id`, `rp_id`/`rp_id_hash`, `user_handle`, `discoverable`, `aaguid`, PSK-wrapped key, `alg -7`, format version) in `store_passkeys.*`.
2. Add `ExportCredential` / `ImportCredential` service ops + transport methods on `net.signet.Passkeys` and the NIP-46 surface (`webauthn_export` / `webauthn_import`), policy-gated by a `passkey.export`/`passkey.import` capability; import validates the PSK and refuses cross-fleet containers.
3. **Live auto-sync — default vs optional.** Default is **orchestrator-driven**: the fleet manager calls `Export` on the source and `Import` on the target via the APIs above (no new protocol, cross-host). Optional native built-in: **encrypted Nostr events over signet's existing relay transport** — container NIP-44-encrypted to fleet-member pubkeys, PSK-wrapped inside (double-wrapped) — for fleets wanting signet to self-replicate. **D-Bus push is rejected** for cross-host sync (local bus only).

### Phase 3 — Interop + conformance
1. Ceremony unit tests: create/verify attestation, assert/verify ECDSA, **`signCount==0`**, **BE/BS bits set**, excludeCredentials rejects dup, UV-required default-fails (`tests/test_fido_service.c`).
2. **Sync round-trip test:** export a credential, import into a second store with the same PSK, assert an assertion made there verifies at the RP; assert import fails under a wrong PSK.
3. Policy/transport tests: capability mapping, D-Bus dispatch, NIP-46 policy decisions.
4. **Full-stack interop harness** (`tests/` or `tools/`): drive register+assert through the *wired* D-Bus/NIP-46 API (not Phase 0's raw prototype) with `python-fido2` as RP+client, verifying outputs with OpenSSL + CBOR; document commands + expected results in `INTEGRATION.md`. This is the Phase 3 acceptance gate.

### Phase 4 — Virtual CTAP-HID device (optional, later, Linux-only)
Deferred and independently gated (`[passkeys] virtual_ctap=false`, Linux-only). Scope when pursued: a `/dev/uhid` virtual HID module (`src/fido_ctaphid.c`) implementing CTAP-HID framing (INIT/CBOR/PING/CANCEL/ERROR/KEEPALIVE) plus a minimal CTAP2 adapter over the existing `SignetFidoService` — with honest UP/UV/PIN advertising settled *before* default-enable, validated by libfido2 / `fido2-token`. Size it as its own project after Phase 3; the programmatic API delivers the agent value without it.

## Deferred Decisions (non-blocking)

No blocking open questions remain — the design decisions are settled above. These three have a clear default and can be revisited during or after implementation without reshaping the plan:
- **Optional attested identity.** Build the `packed` basic-attestation backend (x5c) + pursue a FIDO Metadata Service listing only if a target RP demands cryptographic proof a credential came from signet. Default: don't — ship `none` + fixed AAGUID, exactly as iCloud Keychain / Google Password Manager do.
- **Live relay auto-sync build-out.** Implement the optional Nostr-relay auto-sync (Phase 2b item 3) or stay orchestrator-driven. Default: orchestrator-driven; add relay sync when a fleet wants hands-off replication.
- **Credential-ID scheme.** Random-and-stored (default — discoverable passkeys need the row anyway, and it's carried by the export container) vs key-wrapped/stateless.

## References

- FIDO CTAP 2.2 (PS 2025-07-14): https://fidoalliance.org/specs/fido-v2.2-ps-20250714/fido-client-to-authenticator-protocol-v2.2-ps-20250714.html
- W3C WebAuthn Level 3: https://www.w3.org/TR/webauthn-3/
- Yubico libfido2 (client-side reference): https://developers.yubico.com/libfido2/
- SoloKeys solo1 firmware (authenticator-side C reference): https://github.com/solokeys/solo1
- FIDO Alliance specifications index: https://fidoalliance.org/specifications/
