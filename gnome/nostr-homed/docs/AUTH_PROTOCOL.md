# Nostr authentication protocol v1

This is the frozen protocol contract for the opt-in authentication broker. It
does not make the unfinished broker or PAM module safe to install.

## Framing and admission

`auth.sock`, `user.sock`, and broker/worker socketpairs are Unix
`SOCK_SEQPACKET` connections. A JSON packet is one UTF-8 object of at most
65,536 bytes with exactly `version`, `operation`, `request_id`,
`transaction_id`, and object-valued `payload`. Version is `1`; IDs are empty
where an operation has no transaction, otherwise 64 lowercase hex characters.
Duplicate keys, trailing data, embedded NUL, unknown versions/operations,
wrong types, truncation, control truncation, and all ancillary data are errors.
Unlock input is a separate packet of 12..1,024 bytes and signed events are at
most 16 KiB. Neither is logged.

Admission is default-deny. `auth.sock` requires kernel UID 0 and admits login,
account/session, and explicit administration operations. `user.sock` admits
only an own-account `BeginSmbProof`, deriving the account from kernel UID.
Follow-up selection/unlock/wait/cancel operations require the same endpoint,
connection, PID/UID, transaction, purpose, and current account generation.
Worker endpoints admit no client operations. `SO_PEERCRED`, pidfd/process-start
identity, and the allowed PAM service must be checked before parsing payload
policy. A transferred descriptor never transfers a transaction.

## Challenge and provider contract

The private, never-published proof is a kind-1 event with ordered tags
`["a","org.nostr.auth/1"]`, `["purpose",PURPOSE]`, and
`["challenge",NONCE]`. Compact Jansson content contains the fields frozen in
the shipping plan: protocol, purpose, nonce, transaction/authority/boot/account
identities, canonical username/UID/pubkey/key generation, service, issuance and
expiry, context, and resource. Challenge lifetime is 120 seconds; the whole
transaction is 180 seconds and a connection-bound receipt is 300 seconds.

The provider ABI is internal and declared in `src/auth/auth_provider.h`.
Providers prepare, begin the exact immutable challenge, optionally receive a
local unlock packet, cancel, and destroy. They may emit a complete signed event
but never an authentication-success result. The broker strictly parses it,
calls `nostr_event_validate`, compares pubkey, kind, timestamp, ordered tags,
content bytes and canonical ID, rechecks the A-owned active account and both
generations, then performs the sole one-winner transition to verified.

Local flow is challenge-before-input: select provider, construct challenge,
prompt, decrypt/sign/wipe. Local vault v1 uses OpenSSL scrypt N=262144/r=8/p=1,
a 32-byte random salt, AES-256-GCM with a 12-byte nonce and 16-byte tag, and a
32-byte scalar. Length-prefixed AAD binds version, provider UUID, account UUID,
enrolled pubkey, and key generation. Unsupported envelopes fail closed; wrong
passphrase, tamper, and binding mismatch share the public unlock failure.

## PAM conversation and results

When both providers exist, use `PAM_PROMPT_ECHO_ON` with exactly `Choose Nostr
login method: local or remote`. Trim ASCII surrounding whitespace only and
accept exactly `local` or `remote`; three invalid selections within the original
transaction deadline map to `PAM_MAXTRIES`. A local provider uses
`PAM_PROMPT_ECHO_OFF`; remote uses `PAM_TEXT_INFO`. There is no default, silent
fallback, QR/browser launch, session bus, or custom greeter.

The passphrase remains module-owned and never enters `PAM_AUTHTOK`, argv,
environment, clipboard, logs, or persistent files. PAM response buffers,
module/broker packets, derived keys, and decrypted keys are wiped on every
project-owned exit. Copies internal to GDM/gnome-shell are outside this wipe
guarantee. Cancellation marks terminal before cleanup and acknowledges to PAM
within one second; workers are killed/reaped within three seconds.

Result mapping: verified proof `PAM_SUCCESS`; outside `n_` namespace
`PAM_IGNORE`; absent reserved account `PAM_USER_UNKNOWN`; denial/bad proof/replay
or cancellation `PAM_AUTH_ERR`; conversation failure `PAM_CONV_ERR`; throttling
`PAM_MAXTRIES`; provider/network/storage unavailable `PAM_AUTHINFO_UNAVAIL`;
disabled/not-ready `PAM_PERM_DENIED`; receipt/home/session failure
`PAM_SESSION_ERR`; protocol/programming failure `PAM_SYSTEM_ERR`.

## Transaction admission core

`auth_transaction.c` is an internal single-owner event-loop state machine. It
binds endpoint, UID/GID/PID/process-start identity and connection nonce to the
purpose, account snapshot and service. The `auth_transaction_identity.c` adapter
performs actual private-authority lookups and generation/status rechecks before
verification, after verification, and before receipt consumption. A provider
completion is never sufficient: only the broker strict verifier may supply the
proof result. These entry points must not be exposed directly to client JSON.

The owner serializes proof completions and cancellation; the core is deliberately
not thread-safe. Once cancelled or completed, subsequent proof results cannot
win. Receipt tokens are connection-bound, one-use and expire at their monotonic
deadline; regressing timestamps fail closed. Fixed-size receipt fields are
compared with bounded operations, including malformed nonterminated input.
The broker must derive peer identity and time itself and enforce the configured
PAM service allowlist before invoking the core. The core alone does not perform
socket admission, launch providers, enforce throttling, or authenticate a login.

`test_auth_transaction` exercises peer binding, expiry boundaries, replay,
cancellation, receipt tampering and generation changes. The additional
`test_auth_transaction_identity` uses the real authority store and verifies that
disabling an account after proof completion denies its outstanding receipt.
Its provider/enrollment attestations are store fixtures, not live login proof.

## Still gated

The pinned systemd inherited-descriptor launch, pidfd behavior, persistent
throttling, daemon transaction state, PAM wiring, AppArmor/systemd units, and
real GDM rendering/cancellation remain required Linux integration work. No
foundation test or mock satisfies those gates.
