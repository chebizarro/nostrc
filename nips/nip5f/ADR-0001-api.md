# ADR-0001: NIP-5F Socket Protocol, ACL, Errors

Status: Draft
Date: 2025-08-10

Context
- Spec source: `../../docs/proposals/5F.md` (referenced by `nips/nip5f/SPEC_SOURCE`).
- Provide local-only remote signing over Unix domain sockets with clear framing and ACL.

Decisions
- Framing: 4-byte big-endian length prefix, UTF-8 JSON body; max 1 MiB.
- Socket path: `$HOME/.local/share/nostr/signer.sock` (override `NOSTR_SIGNER_SOCK`).
- Permissions: parent dir 0700; socket 0600; remove stale socket on startup.
- Handshake: server banner `{name:"nostr-signer", supported_methods:[...]}` then client `{client:"<name>"}`.
- Methods: required `get_public_key`, `sign_event`; optional `nip44_encrypt`, `nip44_decrypt`, `list_public_keys`.
- Errors: numeric codes per spec (1,2,3,4,5,10); map module `int` return values to on-wire error.code.
- Rate limiting: per-connection token bucket (default 10 rps, burst 20). Exceed → error with message "RATE_LIMITED".
- ACL: JSON at `$HOME/.config/nostr-signer/acl.json` with allow rules by `exe` and/or `uid` + per-method list.
- Identity: on Linux, use `SO_PEERCRED` to obtain `{pid, uid, gid}`; optionally resolve `exe` via `/proc/<pid>/exe`.
- Concurrency: accept-loop; per-connection reader + worker dispatch; back-pressure via token bucket.

Consequences
- Deterministic, binary-safe transport.
- Local-only access controlled by filesystem perms + ACL + peer creds.
- Extensible method set mirrored with DBus NIP-55L to allow future unification.

Alternatives
- DBus-only (NIP-55L) — kept as separate implementation; this NIP targets minimal POSIX installs.
- JSON-RPC TCP localhost — wider attack surface; avoided.

Notes
- All JSON serialization/parsing must use libnostr JSON helpers; no ad-hoc JSON.
- All code resides under `nips/nip5f/` per guardrails.
