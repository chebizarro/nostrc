# Signet Review Work Plan

Review source: `/Users/bizarro/Documents/Projects/fleet-planning/docs/reviews/signet-review.md`
All code is under: `signet/src/` and `signet/include/signet/`

## Bucket A: Critical Bugs & Transport Hardening
**Files**: ssh_agent.c, nip5l_transport.c, key_store.c, relay_pool.c
**Status**: [x] COMPLETE

- [ ] nostrc-sdh6 [P1 Bug]: SSH agent Ed25519 key derivation — use `crypto_sign_ed25519_seed_keypair` instead of `crypto_scalarmult_ed25519_base_noclamp`. Fix both handle_request_identities and handle_sign_request.
- [ ] nostrc-73fb [P1 Bug]: NIP-5L JSON injection — use JsonBuilder or g_strescape() for all response value interpolation in nip5l_transport.c
- [ ] nostrc-xrqm [P1 Bug]: GHashTable race in key_store_cache_count — acquire cache mutex before g_hash_table_size
- [ ] nostrc-xcag [P2 Bug]: Y2038 timestamp in relay_pool.c — replace gint with gint64 for last_event_ts, use mutex instead of g_atomic_int
- [ ] nostrc-7x6n [P2 Design]: Unbounded thread spawning in nip5l_transport.c and ssh_agent.c — add max-connections semaphore or GThreadPool

## Bucket B: Security Hardening  
**Files**: mgmt_protocol.c, capability.c, bootstrap_server.c
**Status**: [x] COMPLETE

- [ ] nostrc-ke2c [P1 Security]: Plaintext fallback in mgmt protocol — reject events where NIP-44 decryption fails instead of falling back to plaintext. Add optional config flag `allow_unencrypted_mgmt`.
- [ ] nostrc-o7rp [P1 Security]: Bunker sk_hex not mlock'd — use sodium_malloc for bunker_sk_hex in mgmt_protocol.c, replace memset with sodium_memzero in free
- [ ] nostrc-5qm0 [P1 Security]: Fail-open capability default — change capability.c to return false for unmapped methods, explicitly mark connect/ping/get_relays as always-allowed
- [ ] nostrc-h1rf [P2 Security]: Bootstrap TLS — add config option `bootstrap_require_tls`, check X-Forwarded-Proto header, log startup warning

## Bucket C: Design & Architecture
**Files**: nip46_server.c, dbus_tcp.c, dbus_unix.c, session_broker.c, NEW dbus_common.c/h
**Status**: [x] COMPLETE

- [ ] nostrc-in3i [P2 Design]: NIP-46 mutex contention — narrow critical section to session lookup only, release before signing/encryption/relay publish
- [ ] nostrc-czt1 [P2 Design]: D-Bus code duplication — extract shared Signer+Credentials dispatch to new dbus_common.c, each transport handles only auth
- [ ] nostrc-t1d6 [P2 Design]: Session broker JSON — replace strstr/strchr parsing with json_parser_load_from_data + json_object_get_string_member  
- [ ] nostrc-r7ae [P3 Minor]: GetSession token persistence — document that lease record is source of truth, or persist token hash alongside lease

## Bucket D: Code Hygiene & Documentation
**Files**: store.c, health_server.c, new signet/util.h, multiple source files
**Status**: [x] COMPLETE
**Note**: This bucket touches files also modified by Buckets A-C (nip5l_transport.c, dbus_tcp.c, dbus_unix.c, mgmt_protocol.c, session_broker.c). The changes are trivial (replace static function with #include) and should not conflict with substantive changes.

- [ ] nostrc-lutx [P3 Minor]: store.c DEK derivation comments — change "HKDF-SHA256" references to accurately describe BLAKE2b with domain separation
- [ ] nostrc-ldmi [P3 Minor]: Document dual key management threat model in store.c — add block comment explaining why both SQLCipher + envelope exist and the shared-key tradeoff
- [ ] nostrc-7kio [P3 Minor]: Extract signet_now_unix() to signet/util.h — create new header, replace in dbus_tcp.c, dbus_unix.c, session_broker.c, health_server.c, nip5l_transport.c
- [ ] nostrc-ju8r [P3 Minor]: Extract hex_to_bytes32 to signet/util.h — move from mgmt_protocol.c and nip46_server.c to shared header
- [ ] nostrc-l7h1 [P3 Minor]: health_server.c port parsing — replace atoi with strtoul + proper error checking
