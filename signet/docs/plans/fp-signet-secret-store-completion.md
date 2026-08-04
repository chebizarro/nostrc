# Plan: fp-signet-secret-store-completion-20260803 (P0)

Canonical fleet task: `fp-signet-secret-store-completion-20260803` (nostrig ledger, epic fp-50).
Codebase: `/Users/bizarro/Documents/Projects/nostrc/signet` (C, meson + cmake). Key files: `src/store_secrets.c`, `include/signet/store_secrets.h`, `src/signetctl_main.c`, `src/mgmt_protocol.c`, `src/store_leases.c`, `src/store_audit.c`, `src/capability.c`, `src/policy_engine.c`, `src/nip5l_transport.c`, `src/ssh_agent.c`, `src/dbus_common.c`/`dbus_tcp.c`/`dbus_unix.c`, `src/nip46_server.c`, `dbus/net.signet.Signer.xml`.

Context: Signet has an encrypted generic secret store (SQLCipher + per-agent secretbox; types nostr_nsec, ssh_key, api_token, credential, certificate; put/get/delete/list/rotate; secret_versions; leases; D-Bus GetToken/GetSession/ListCredentials; capabilities; hash-chained audit). The operator/control-plane lifecycle is incomplete. Do NOT reopen already-fixed findings: SSH Ed25519 derivation, cache locking, gint64 relay timestamps, fail-closed unknown capabilities, json-glib session parsing, sodium bunker-key storage, encrypted-management fail-closed.

Hard security invariants (all items):
- Secret payloads NEVER via argv, logs, Nostr plaintext, audit/evidence, or REST. Input only via stdin or protected file.
- Mutations only via encrypted, signed, replay-protected ContextVM management channel. No REST management API.
- Retrieval remains agent-owned, capability/lease gated. Listing/inspection is metadata-only.

## Work items

- [x] **Item 1 — Lifecycle core** (DONE — commit c72cd040) (store_secrets.c/h, signetctl_main.c, mgmt_protocol.c)
  - Provisioner-authorized generic credential/API-token create + import (payload via stdin or protected file only), over encrypted ContextVM.
  - Metadata-only list/inspect (active/revoked/expired/version metadata, no payloads); deterministic IDs; ownership/policy fields.
  - Rotation: `signetctl rotate-credential` currently prints failure but exits 0 — return nonzero on failure. Make rotation atomic, preserve provenance/history; fix `INSERT OR REPLACE` silently overwriting/resetting version history.
  - Revoke/delete; `expires_at` on creation, enforced at retrieval/session issuance.
  - Done when: create/import/list/rotate/revoke/expiry work end-to-end via CLI + ContextVM with correct exit codes and no payload disclosure; builds clean; unit tests for exit codes, overwrite refusal, rotation history, expiry.

- [x] **Item 2 — Authorization, audit, Git credential integration** (DONE — see progress log 2026-08-04) (capability.c, policy_engine.c, store_leases.c, store_audit.c, audit_logger.c; new git-credential helper)
  - End-to-end authz: provisioner-only mutations, per-agent ownership, explicit capabilities, deny/revoke precedence, lease burn on use.
  - Hash-chained audit entries for mutation/access/denial — never containing secret values; redaction verified.
  - File-backed Git credential helper: GitHub PAT stored as api_token, bound least-privilege to an agent, retrieved without argv or materialized config, rotates safely.
  - Done when: unauthorized/wrong-agent/expired/revoked paths fail closed with audit entries; git helper completes an authenticated read-only canary without ever outputting the token outside the credential protocol stream.

- [x] **Item 3 — Concurrency & robustness** (DONE — commit c72cd040; load test fixed via SQLITE_OPEN_FULLMUTEX + retrieval serialization, nostrc-247o closed) (nip5l_transport.c, ssh_agent.c, dbus_common/tcp/unix, nip46_server.c, signet_config.c)
  - Bound NIP-5L and SSH-agent thread-per-client models: connection limits/pools, denial metrics, clean shutdown; race tests.
  - Replace remaining `atoi` port parsing with checked range validation.
  - Consolidate duplicated D-Bus TCP/Unix dispatch into shared helpers so authorization logic cannot drift between transports.
  - Revalidate NIP-46 mutex scope: relay network I/O must not serialize all signing; add a load test for concurrent signing + credential retrieval.
  - Done when: connection-flood test is bounded and metered, ports reject out-of-range, one shared dispatch path, load test passes without serialization.

- [ ] **Item 4 — Threat model, backup/restore, comprehensive tests** (docs/, tests/)
  - Document SQLCipher + envelope-encryption threat model; both currently derive from the same master secret — justify or separate key material. Verify mlock/zeroization; encrypted backup/restore covering current and archived secrets.
  - Test suite: create/read/list w/o disclosure; wrong-agent/capability; plaintext-mutation rejection; replay; duplicate/overwrite refusal; rotation/archive; expiry; revoke/lease burn; restart durability; backup/restore; wrong DB key/tamper; audit integrity; CLI exit codes; redaction; concurrency; git-helper canary.
  - Done when: threat-model doc exists, backup/restore proven, full suite green.

## Progress log
- 2026-08-03: plan created; task claimed in beads.
- 2026-08-03: Item 3 implemented (8-conn bounds + denial metrics on NIP-5L/SSH-agent, clean shutdown, 0..65535 port validation, flood/shutdown/metric/port tests; D-Bus already shared-dispatch; NIP-46 mutex scope confirmed OK). Remaining: concurrent signing+retrieval load test crashes in signet_store_get_secret (SQLite thread-safety) — needs Item 1 store fix (nostrc-247o). signing_crypto test failing under Item 1's in-flight changes.
- 2026-08-03: Items 1+3 committed as c72cd040 (nostrc repo, unpushed). Full suite 20/20; concurrency load test passed 10 stress runs. nostrc-247o, nostrc-n33l, nostrc-s0e4 closed.
- 2026-08-04: Item 2 implemented (nostrc-a1sg). New unified credential-access API (credential_access.h/.c) — single enforcement path for every credential payload retrieval: deny-list precedence (pubkey from custody, unknown agent fails closed) → explicit capability (NULL registry fails closed) → rate limit → metadata-only owner + revoked>expired check BEFORE decrypt → type deny rules (new signet_policy_type_allowed) → atomic one-use lease burn (new signet_store_consume_lease) → decrypt (owner re-verified) → tracking lease clamped to credential expiry → mandatory hash-chain audit on EVERY outcome (unauditable success is rolled back and denied). store_audit chain appends made atomic (db mutex + BEGIN IMMEDIATE with autocommit guard). D-Bus GetToken routed through the API (uniform NotFound on denial; deny list plumbed through dbus_unix/tcp/signetd); GetSession audited. session_broker owner-check hole fixed by routing through the API (signature gained deny list). ContextVM management now writes hash-chain audits for all mutations and unauthorized senders. New installable `signet-git-credential` helper (protocol engine in core + D-Bus GetToken binary; deterministic id from agent+host; token only ever on the credential protocol stream; store/erase no-ops; non-https refused). Tests: test_credential_access.c (authz matrix, precedence, lease burn, rate limit, redaction, concurrent chain appends), test_git_credential.c (canary: PAT exactly once on protocol stream, wrong-agent/revoked quiet fail-closed, rotation-safe, audit chain intact + PAT-free). Suite 22/22 green. Remaining: Item 4.
