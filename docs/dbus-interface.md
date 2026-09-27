# GNostr Signer D-Bus Interface

This document describes the D-Bus interface exposed by the `gnostr-signer-daemon` for secure Nostr key management and cryptographic operations.

## Overview

The GNostr Signer provides a secure D-Bus service for:

- **Key Management**: Secure storage and retrieval of Nostr private keys
- **Event Signing**: Cryptographic signing of Nostr events (NIP-01)
- **Encryption/Decryption**: NIP-04 (legacy) and NIP-44 (modern) message encryption
- **Zap Processing**: Decryption of zap receipt events (NIP-57)
- **User Approval**: Interactive approval flow for sensitive operations

The daemon isolates private keys from client applications, ensuring keys never leave the secure process boundary.

## Bus Name and Object Path

| Property | Value |
|----------|-------|
| **Bus Name** | `org.nostr.Signer` |
| **Object Path** | `/org/nostr/signer` |
| **Interface** | `org.nostr.Signer` |
| **Bus Type** | Session bus (default) or System bus (`--system` flag) |

### D-Bus Service Activation

The daemon supports D-Bus activation. When a client calls a method on `org.nostr.Signer`, D-Bus will automatically start the daemon if it is not running.

Service file location: `/usr/share/dbus-1/services/org.nostr.Signer.service`

Exactly one package owns that file: **gnostr-signer** installs it, activating
`gnostr-signer-daemon` (via `SystemdService=gnostr-signer-daemon.service`).
The standalone `nips/nip55l` daemon (`nostr-signer-daemon`) installs its own
copy only when built with `-DENABLE_NIP55L_STANDALONE_ACTIVATION=ON` (default
OFF), for headless installs that ship no gnostr-signer. Never install both.

## Interface: org.nostr.Signer

### Access control (nip55l 0.4.0)

Every method that uses or reveals key material or configuration is
approval-gated: `GetPublicKey`, `GetRelays`, `SignEvent`, `NIP04Encrypt`,
`NIP04Decrypt`, `NIP44Encrypt`, `NIP44Decrypt`, `NIP44EncryptB64`,
`NIP44DecryptB64`, `NIP44DeriveConversationKey`, `DecryptZapEvent` and their
`*ForApp` variants (nostrc-y02q, nostrc-1e31). `StoreKey`/`ClearKey` keep
their own gate (`NOSTR_SIGNER_ALLOW_KEY_MUTATIONS`).

**Who is calling (principal).** Derived by the daemon from the bus
connection, never from an argument (nostrc-phk4):
`GetConnectionCredentials` (uid, pid, pidfd when the bus offers it) and
`/proc/<pid>`:

| Principal | Source | Verified by |
|-----------|--------|-------------|
| `flatpak:<app-id>` | `/proc/<pid>/root/.flatpak-info` | the sandbox (attested) |
| `app:<app-id>;exe:<path>` | systemd app scope/service in `/proc/<pid>/cgroup` + `/proc/<pid>/exe` | kernel (unsandboxed; the scope name is choosable) |
| `snap:<name>;exe:<path>` | snap cgroup + executable | as above |
| `exe:<path>` | `/proc/<pid>/exe` | kernel |
| `https://<site>` | the `app_id` argument, **only** from the installed browser bridge | bridge executable (path + inode) |
| (none) | other uid, `/proc` unreadable, caller gone | — |

A scoped caller is keyed on scope *and* executable: every program started
from a terminal emulator shares the terminal's app scope, and every GJS or
Python app shares its interpreter binary. An unidentified caller can still
be prompted; nothing is remembered for it.

The `app_id` argument is a sub-principal only when the caller's executable
is `<libexecdir>/nostr-signer-webext-host` (path and device/inode, EXE or
app-scope caller, never Flatpak/Snap) *and* the value is a browser-serialized
secure origin (`https://host[:port]`, `http://localhost…`); the call then
runs as that origin. From anyone else `app_id` is an unverified label that
the approval UI shows as "calls itself …" — claiming `https://snort.social`
does not inherit that site's grants. This is the wallet agent's rule
(`gnome/nostr-wallet-agent/src/nwa-caller.h`).

**Which identity.** The identity selector a caller passes is never key
material (nostrc-a4w5): `""` is the active identity; an npub or a bare
64-hex (read as an x-only *public* key) must name a known identity exactly
(the active one, or a stored key with that npub), else
`Error.NoKeyConfigured`; an `nsec` is refused with `Error.InvalidInput`;
anything else is a `key_id`/label as before. Before 0.4.0 any 64-hex was
used as the private key, so a pubkey passed by mistake signed with a key
derived from the pubkey bytes. A grant is keyed on the npub the selector
resolves to — the key the operation will use — never on the selector
string, so `""`, a `key_id`, the npub and the hex pubkey hit the same grant
(nostrc-eie5).
If the active identity changes while a request is pending, approving it
fails instead of using the new key.

**Grants.** `$XDG_CONFIG_HOME/gnostr/signer-grants.ini`, one section per
request kind, key `<principal>|<npub>` (or `<principal>|*` for any
identity), value `allow`, `deny`, `allow:<until-unix-ts>` or
`deny:<until-unix-ts>`. Kinds: `event`, `nip44_conversation_key`,
`get_public_key`, `get_relays`, `nip04_encrypt`, `nip04_decrypt`,
`nip44_encrypt`, `nip44_decrypt` (also the B64 variants), `zap_decrypt`.
Remembering one kind never covers another. The pre-0.4.0
`~/.config/gnostr/signer-acl.ini` (keyed on the claimed `app_id`) is ignored.
Built-in defaults cover first-party services that run without a UI
(`get_public_key`/`get_relays` for the nostr-homed helpers and
`nostr-notify-daemon`, `nip44_decrypt` for `nostr-homectl`), matched on the
installed executable's path and inode; CMake `NIP55L_DEFAULT_GRANTS`. An
explicit entry in the grants file wins.

**Approval.** Without a grant the call is parked and `ApprovalRequested`
is emitted with `app_id` = principal and `identity` = resolved npub.
Identical calls from the same connection join the pending request, so a
burst of DM decrypts raises one prompt. Only the installed approval UI —
`<bindir>/gnostr-signer` (path + inode) or the `org.gnostr.Signer` Flatpak —
may call `ApproveRequest`/`GetApprovalInfo`; others get
`Error.PermissionDenied`. When no process owns `org.gnostr.Signer` (no
approval UI running) a call that needs a prompt fails at once with
`Error.ApprovalDenied`. Unanswered requests expire after 300 s, and are
dropped when the caller disconnects.

**Limits of the model.** An unsandboxed process of the same user can also
edit the grants file or ptrace other processes, so for those callers the
grants are a guard against mistakes and confused deputies, not a boundary.
The boundary is against sandboxed apps and against a process claiming
another app's or a website's identity over the bus. Accepted under that
model: an unsandboxed same-user process can run a trusted binary itself —
drive the browser bridge's stdin to speak for any origin, or inject code
into a trusted executable (`LD_PRELOAD`) — and inherit its grants; only
Flatpak-attested principals resist that. Without a pidfd from the bus
(dbus-daemon < 1.15) a process that passes its bus socket to another and
exits could, after PID reuse, be read as a different process. The approval
UI protects against other processes, not against itself: gnostr-signer can
answer its own requests. Approver checks identify the caller afresh on every
call; after an upgrade replaces `gnostr-signer` the running instance is no
longer trusted until restarted (logged). One application (principal, or
connection when unidentified) may park at most 8 requests. On macOS the bus does not report
client PIDs: callers are keyed as `claimed:<app_id>` (the pre-0.4.0 model,
shown as unverified) and any same-user process may answer approvals.

**NIP-5F socket.** The optional Unix socket (`NOSTR_SIGNER_ENDPOINT=unix:<path>`,
off by default) goes through the same path (nostrc-q23h). Its caller's
principal is built from the socket peer the kernel reports at connect time
(`SO_PEERCRED`, plus `SO_PEERPIDFD` on Linux >= 6.5; `getpeereid` +
`LOCAL_PEERPID` on macOS) and `/proc/<pid>`, so it has the same shape as that
process's D-Bus principal and the same grants apply to both transports. A
socket request without a grant raises the same `ApprovalRequested` on the bus
and waits for `ApproveRequest`; a denial is NIP-5F error 5. Only peers of the
daemon's uid are accepted; a claimed `app_id` in the request is ignored; a
client that hangs up while waiting drops its request. The TCP lane
(`tcp:127.0.0.1:<port>`, build option `ENABLE_TCP_IPC`) has no peer
credentials: its callers are unidentified, prompted every time and never
remembered. Without a pidfd the same PID-reuse caveat as above applies.

**Daemon sandboxing.** Reading callers' `/proc` entries needs the daemon
outside any user namespace, so its systemd user unit uses only
namespace-free hardening (see *Systemd Hardening*).

### Methods

#### GetPublicKey

Returns the public key (npub) for the currently active identity.

```xml
<method name="GetPublicKey">
  <arg name="npub" type="s" direction="out"/>
</method>
```

**Parameters**: None

**Returns**:
- `npub` (string): Bech32-encoded public key (npub1...)

**Approval** (0.4.0): kind `get_public_key`. `GetPublicKeyForApp(s app_id)`
is the same call with an `app_id`.

**Errors**:
- `org.nostr.Signer.Error.NoKeyConfigured`: No key configured
- `org.nostr.Signer.Error.ApprovalDenied`: denied, timed out, or no approval UI running
- `org.nostr.Signer.Error.Internal`: backend failure

---

#### SignEvent

Signs a Nostr event and returns the complete signed event JSON. May trigger user approval dialog.

> **Contract change (nip55l 0.2.0).** Before 0.2.0 this method returned only
> the 128-hex Schnorr signature. It now returns the whole signed event, so
> callers get `id`, `pubkey` and `sig` together from one consistent signing
> operation. The D-Bus signature is unchanged (`s`), so an old caller will not
> fail to marshal — it must be updated to parse the result as JSON.

```xml
<method name="SignEvent">
  <arg name="event_json" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="app_id" type="s" direction="in"/>
  <arg name="signed_event" type="s" direction="out"/>
</method>
```

**Parameters**:
- `event_json` (string): JSON-serialized Nostr event (without signature)
- `current_user` (string): Identity selector (npub, key_id, or empty for default)
- `app_id` (string): Web origin when called by the browser bridge; otherwise
  an unverified label (see *Access control*)

**Returns**:
- `signed_event` (string): the complete signed event JSON —
  `{"id":…,"pubkey":…,"created_at":…,"kind":…,"tags":[…],"content":…,"sig":…}`.
  `pubkey` is always the signing key's (a caller-supplied value is replaced),
  so callers should verify `id`/`sig` against the returned event, not against
  the template they sent.

**Errors**:
- `org.nostr.Signer.Error.InvalidInput`: Malformed event JSON
- `org.nostr.Signer.Error.ApprovalDenied`: User denied the signing request
- `org.nostr.Signer.Error.RateLimited`: Too many requests in short period
- `org.nostr.Signer.Error.NoKeyConfigured`: No key for the requested identity
- `org.nostr.Signer.Error.Internal`: Signing operation failed

**Notes**:
- Kind `event`. Without a grant, emits `ApprovalRequested` and waits for `ApproveRequest`
- The `created_at` field is auto-populated if set to 0

---

#### NIP44Encrypt

Encrypts a message using NIP-44 v2 (modern, recommended).

```xml
<method name="NIP44Encrypt">
  <arg name="plaintext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="ciphertext" type="s" direction="out"/>
</method>
```

**Parameters**:
- `plaintext` (string): UTF-8 message to encrypt
- `peer_pubkey` (string): Recipient's public key (64-char hex)
- `current_user` (string): Identity selector (npub, key_id, or empty for default)

**Returns**:
- `ciphertext` (string): Base64-encoded NIP-44 v2 ciphertext

**Approval** (0.4.0): kind `nip44_encrypt` (shared with `NIP44EncryptB64`).
`NIP44EncryptForApp` / `NIP44EncryptB64ForApp` take a trailing `app_id`.

**Errors**:
- `org.nostr.Signer.Error.InvalidInput`: Invalid public key format (checked before prompting)
- `org.nostr.Signer.Error.ApprovalDenied`: denied, timed out, or no approval UI running
- `org.nostr.Signer.Error.Internal`: Encryption failed

---

#### NIP44Decrypt

Decrypts a message using NIP-44 v2.

```xml
<method name="NIP44Decrypt">
  <arg name="ciphertext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="plaintext" type="s" direction="out"/>
</method>
```

**Parameters**:
- `ciphertext` (string): Base64-encoded NIP-44 v2 ciphertext
- `peer_pubkey` (string): Sender's public key (64-char hex)
- `current_user` (string): Identity selector

**Returns**:
- `plaintext` (string): Decrypted UTF-8 message

**Approval** (0.4.0, nostrc-y02q): kind `nip44_decrypt` (shared with
`NIP44DecryptB64`). Before 0.4.0 any session-bus client could decrypt the
user's messages without a prompt. `NIP44DecryptForApp` /
`NIP44DecryptB64ForApp` take a trailing `app_id`.

**Errors**:
- `org.nostr.Signer.Error.InvalidInput`: Invalid public key (checked before prompting)
- `org.nostr.Signer.Error.ApprovalDenied`: denied, timed out, or no approval UI running
- `org.nostr.Signer.Error.Internal`: Decryption failed (wrong key or corrupted)

---

#### NIP44DeriveConversationKey

*Since nip55l 0.3.0.* Returns the NIP-44 v2 conversation key between the
selected identity and a peer, so a client can open NIP-44 payloads addressed
to that peer without ever seeing the nsec. Used by `nostr-seal` (nostrc-da9c)
with a per-file ephemeral peer.

```xml
<method name="NIP44DeriveConversationKey">
  <arg name="peerPubKey" type="s" direction="in"/>
  <arg name="identity" type="s" direction="in"/>
  <arg name="app_id" type="s" direction="in"/>
  <arg name="conversationKey" type="s" direction="out"/>
</method>
```

**Parameters**:
- `peerPubKey` (string): Peer's x-only public key (64-char hex, any case)
- `identity` (string): Identity selector (empty = active identity)
- `app_id` (string): Web origin from the browser bridge, else an unverified label

**Returns**:
- `conversationKey` (string): 64 lowercase hex, `HKDF-extract(SHA-256, IKM = ECDH shared x, salt = "nip44-v2")`

**Approval**: same flow as `SignEvent`, kind `nip44_conversation_key`; with
no grant the call is held, `ApprovalRequested` is emitted with preview
`derive NIP-44 conversation key with <peer hex>`, and `ApproveRequest`
completes it. Grant it only for throwaway peers — the key opens every NIP-44
payload exchanged with that peer.

**Errors**:
- `org.nostr.Signer.Error.InvalidInput`: peer is not 64-hex or not on secp256k1 (checked before prompting)
- `org.nostr.Signer.Error.ApprovalDenied`: denied by policy or by the user
- `org.nostr.Signer.Error.RateLimited`: more than one prompt per 100 ms from a sender
- `org.nostr.Signer.Error.NoKeyConfigured`: no key for this identity

---

#### NIP04Encrypt

Encrypts a message using NIP-04 (legacy, for compatibility).

```xml
<method name="NIP04Encrypt">
  <arg name="plaintext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="ciphertext" type="s" direction="out"/>
</method>
```

**Parameters**: Same as NIP44Encrypt

**Returns**:
- `ciphertext` (string): Base64-encoded NIP-04 ciphertext with IV

**Approval** (0.4.0): kind `nip04_encrypt`; `NIP04EncryptForApp` takes a trailing `app_id`.

**Notes**: NIP-04 is deprecated. Prefer NIP-44 for new implementations.

---

#### NIP04Decrypt

Decrypts a message using NIP-04 (legacy).

```xml
<method name="NIP04Decrypt">
  <arg name="ciphertext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="plaintext" type="s" direction="out"/>
</method>
```

**Parameters**: Same as NIP44Decrypt

**Approval** (0.4.0, nostrc-y02q): kind `nip04_decrypt`; `NIP04DecryptForApp`
takes a trailing `app_id`.

---

#### DecryptZapEvent

Decrypts the content of a zap receipt event (NIP-57).

```xml
<method name="DecryptZapEvent">
  <arg name="event_json" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="decrypted_event" type="s" direction="out"/>
</method>
```

**Parameters**:
- `event_json` (string): JSON-serialized zap event
- `current_user` (string): Identity selector

**Returns**:
- `decrypted_event` (string): Event JSON with decrypted content field

**Notes**:
- Extracts peer pubkey from the first `p` tag
- Tries NIP-44 first, falls back to NIP-04
- Approval (0.4.0): kind `zap_decrypt`; `DecryptZapEventForApp` takes a trailing `app_id`

---

#### GetRelays

Returns the user's explicitly configured relay URLs. The signer never fetches
NIP-65 relay lists from the network to answer this.

```xml
<method name="GetRelays">
  <arg name="relays_json" type="s" direction="out"/>
</method>
```

Sources, first match wins:
1. `$XDG_CONFIG_HOME/nostr/relays.conf` (default `~/.config/nostr/relays.conf`):
   a JSON array of `ws://` / `wss://` URL strings, e.g. `["wss://relay.example"]`.
2. The relay list set in gnostr-signer (GSettings `org.gnostr.Signer` `relays`),
   only when the user has written it — the schema default does not count.

**Returns**:
- `relays_json` (string): JSON array of normalised relay URLs (lowercase
  scheme/host, no bare trailing `/`, duplicates removed), e.g. `["wss://nos.lol"]`

**Approval** (0.4.0): kind `get_relays` (identity-independent grant
`<principal>|*`); `GetRelaysForApp(s app_id)` takes an `app_id`.

**Errors**:
- `org.nostr.Signer.Error.NotFound`: nothing configured. This is an expected
  state: callers fall back to their own configured relays, never treat it as fatal.
- `org.nostr.Signer.Error.InvalidConfig`: `relays.conf` exists but is malformed.

---

#### StoreKey

Stores a private key in the secure backend (libsecret/Keychain).

```xml
<method name="StoreKey">
  <arg name="key" type="s" direction="in"/>
  <arg name="identity" type="s" direction="in"/>
  <arg name="ok" type="b" direction="out"/>
  <arg name="npub" type="s" direction="out"/>
</method>
```

**Parameters**:
- `key` (string): Private key (64-char hex or nsec1... bech32)
- `identity` (string): Optional identity label/selector

**Returns**:
- `ok` (boolean): True if stored successfully
- `npub` (string): Derived public key (npub1...)

**Errors**:
- `org.nostr.Signer.Error.PermissionDenied`: Key mutations disabled
- `org.nostr.Signer.Error.RateLimited`: Rate limit exceeded
- `org.nostr.Signer.InvalidKey`: Invalid private key format
- `org.nostr.Signer.SecretServiceUnavailable`: Backend unavailable

**Security**: Requires `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1` environment variable.

---

#### ClearKey

Removes a private key from the secure backend.

```xml
<method name="ClearKey">
  <arg name="identity" type="s" direction="in"/>
  <arg name="ok" type="b" direction="out"/>
</method>
```

**Parameters**:
- `identity` (string): Identity selector (npub or key_id)

**Returns**:
- `ok` (boolean): True if removed successfully

**Security**: Requires `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1` environment variable.

---

#### ApproveRequest

Responds to a pending approval request (from UI).

```xml
<method name="ApproveRequest">
  <arg name="request_id" type="s" direction="in"/>
  <arg name="decision" type="b" direction="in"/>
  <arg name="remember" type="b" direction="in"/>
  <arg name="ttl_seconds" type="t" direction="in"/>
  <arg name="ok" type="b" direction="out"/>
</method>
```

**Parameters**:
- `request_id` (string): ID from `ApprovalRequested` signal
- `decision` (boolean): True to approve, false to deny
- `remember` (boolean): Remember the decision for (principal, identity, kind)
  in the grants file; ignored for an unidentified caller
- `ttl_seconds` (uint64): How long to remember (0 = forever)

**Returns**:
- `ok` (boolean): True if the request was found and every queued call succeeded

**Errors**:
- `org.nostr.Signer.Error.PermissionDenied`: the caller is not the installed
  approval UI (0.4.0)

---

#### GetApprovalInfo

*Since nip55l 0.4.0.* Details of a pending request for the approval UI
(approval-UI callers only, like `ApproveRequest`).

```xml
<method name="GetApprovalInfo">
  <arg name="request_id" type="s" direction="in"/>
  <arg name="info" type="a{sv}" direction="out"/>
</method>
```

Keys: `request_id`, `kind`, `principal`, `principal_kind` (`flatpak`,
`snap`, `systemd-scope`, `executable`, `website`, `claimed`,
`unidentified`), `app_id`, `exe`, `via` (web origins: the bridge's
principal), `claimed_app_id` (the caller's unverified `app_id` argument),
`identity`, `preview` (all `s`); `attested`, `verified`, `rememberable`
(`b`); `calls` (`u`, calls that joined the request).
Unknown request: `Error.NotFound`.

---

### Signals

#### ApprovalRequested

Emitted when a signing operation or a conversation-key export requires user approval.

```xml
<signal name="ApprovalRequested">
  <arg type="s" name="app_id"/>
  <arg type="s" name="identity"/>
  <arg type="s" name="kind"/>
  <arg type="s" name="preview"/>
  <arg type="s" name="request_id"/>
</signal>
```

**Arguments**:
- `app_id`: The verified principal (0.4.0; empty when unidentified) — not the
  caller's `app_id` argument (use `GetApprovalInfo` for that)
- `identity`: The npub the request will use (0.4.0: resolved, never empty
  when a key exists)
- `kind`: `event`, `nip44_conversation_key`, `get_public_key`, `get_relays`,
  `nip04_encrypt`, `nip04_decrypt`, `nip44_encrypt`, `nip44_decrypt`,
  `zap_decrypt`
- `preview`: Human-readable preview of the content (truncated)
- `request_id`: Unique ID to use with `ApproveRequest`

---

#### ApprovalCompleted

Emitted when an approval request has been resolved.

```xml
<signal name="ApprovalCompleted">
  <arg type="s" name="request_id"/>
  <arg type="b" name="decision"/>
</signal>
```

**Arguments**:
- `request_id`: The request that was completed
- `decision`: True if approved, false if denied

---

## Error Codes

### D-Bus Error Names

| Error Name | Description |
|------------|-------------|
| `org.nostr.Signer.Error.PermissionDenied` | Operation not allowed by policy |
| `org.nostr.Signer.Error.RateLimited` | Too many requests (500ms cooldown) |
| `org.nostr.Signer.Error.ApprovalDenied` | User denied the operation |
| `org.nostr.Signer.Error.InvalidInput` | Malformed input data |
| `org.nostr.Signer.Error.Internal` | Internal error or backend failure |
| `org.nostr.Signer.Error.NoKeyConfigured` | No key configured for the identity |
| `org.nostr.Signer.Error.NotFound` | `GetRelays`: no relays configured (fall back) |
| `org.nostr.Signer.Error.InvalidConfig` | `GetRelays`: `relays.conf` is malformed |
| `org.nostr.Signer.InvalidKey` | Invalid private key format |
| `org.nostr.Signer.InvalidArgument` | Invalid method argument |
| `org.nostr.Signer.NotFound` | Key or identity not found |
| `org.nostr.Signer.SecretServiceUnavailable` | Secret storage backend unavailable |
| `org.nostr.Signer.Failure` | Generic operation failure |

### Internal Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0 | `NOSTR_SIGNER_OK` | Success |
| 1 | `NOSTR_SIGNER_ERROR_INVALID_JSON` | Malformed JSON input |
| 2 | `NOSTR_SIGNER_ERROR_INVALID_KEY` | Invalid key format |
| 3 | `NOSTR_SIGNER_ERROR_UNAUTHORIZED` | Unauthorized operation |
| 4 | `NOSTR_SIGNER_ERROR_PERMISSION_DENIED` | Permission denied |
| 5 | `NOSTR_SIGNER_ERROR_CRYPTO_FAILED` | Cryptographic operation failed |
| 6 | `NOSTR_SIGNER_ERROR_NOT_FOUND` | Resource not found |
| 7 | `NOSTR_SIGNER_ERROR_BACKEND` | Backend service error |
| 8 | `NOSTR_SIGNER_ERROR_RATE_LIMITED` | Rate limit exceeded |
| 9 | `NOSTR_SIGNER_ERROR_INVALID_ARG` | Invalid argument |

---

## Rate Limiting

The signer implements rate limiting to prevent abuse:

| Operation | Cooldown |
|-----------|----------|
| New approval prompt (any gated method) | 100ms per D-Bus sender; calls joining a pending request are not limited |
| Pending requests | 64 at a time, 256 calls per request, 300 s each |
| `StoreKey` / `ClearKey` | 500ms per D-Bus sender |

Requests exceeding the rate limit receive `org.nostr.Signer.Error.RateLimited`.

---

## Security Considerations

### Verifying the Signer is Authentic

1. **Check the bus name owner**: Use `GetNameOwner` to verify `org.nostr.Signer` is owned by the expected process.

2. **Verify process credentials**: On Linux, use `org.freedesktop.DBus.GetConnectionUnixProcessID` to get the PID, then verify `/proc/<pid>/exe` points to the expected binary.

3. **Flatpak/Snap isolation**: When running in a sandbox, the signer should be on the host system with appropriate portal access.

### Permission Model

- **Key mutations disabled by default**: `StoreKey` and `ClearKey` require `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1`
- **User approval required**: every gated method (see *Access control*) without a grant triggers interactive approval
- **Grants**: decisions are remembered per (verified principal, resolved npub, kind), optionally with a TTL, in `~/.config/gnostr/signer-grants.ini`
- **Approval UI only**: `ApproveRequest` is refused for any caller other than the installed gnostr-signer

### Key Storage Security

| Platform | Backend | Security |
|----------|---------|----------|
| Linux | libsecret (GNOME Keyring / KDE Wallet) | Encrypted, user session locked |
| macOS | Keychain | Hardware-backed, biometric unlock |
| Fallback | Environment variables | Not recommended for production |

### Systemd Hardening

The daemon's systemd *user* unit uses only hardening that works in a user
unit and keeps the daemon out of a user namespace (caller identification
reads other processes' `/proc` entries):

- `NoNewPrivileges=yes`, `MemoryDenyWriteExecute=yes`, `LockPersonality=yes`
- `RestrictNamespaces=yes`, `RestrictRealtime=yes`, `RestrictSUIDSGID=yes`
- `ProtectProc=invisible`, `ProcSubset=pid`
- `SystemCallFilter` allow/deny lists, `RestrictAddressFamilies`, `IPAddressDeny=any` (localhost allowed)
- `LimitCORE=0` (no core dumps with secrets), `TasksMax=32`

Not used: `PrivateDevices=`, `ProtectKernelModules=`, `ProtectKernelLogs=`,
`ProtectClock=`, `CapabilityBoundingSet=` (fail a user unit with
218/CAPABILITIES; nostrc-8sya), `LimitNPROC=` (counts the whole uid), and
`PrivateTmp=`, `ProtectSystem=`, `ProtectHome=`, `ProtectKernelTunables=`,
`ProtectControlGroups=`, `ProtectHostname=`, `ReadWritePaths=` (each puts
the unit in a user namespace that hides callers' `/proc`).

---

## Example Code

### Python (using pydbus)

```python
#!/usr/bin/env python3
"""Example: Sign a Nostr event using GNostr Signer D-Bus interface."""

from pydbus import SessionBus
import json
import time

# Connect to the session bus
bus = SessionBus()

# Get the signer proxy
signer = bus.get("org.nostr.Signer", "/org/nostr/signer")

# Get the current public key
try:
    npub = signer.GetPublicKey()
    print(f"Public key: {npub}")
except Exception as e:
    print(f"Error getting public key: {e}")
    exit(1)

# Create an unsigned event
event = {
    "kind": 1,
    "content": "Hello from GNostr Signer!",
    "tags": [],
    "created_at": int(time.time()),
    "pubkey": ""  # Will be filled by signer
}

# Sign the event
try:
    signed = json.loads(signer.SignEvent(
        json.dumps(event),  # event_json
        "",                  # current_user (empty = default identity)
        "my-app"            # app_id
    ))
    print(f"Signed event {signed['id']} sig={signed['sig']}")
except Exception as e:
    print(f"Signing failed: {e}")

# NIP-44 encryption example
recipient_pubkey = "abc123..."  # 64-char hex pubkey
try:
    ciphertext = signer.NIP44Encrypt(
        "Secret message",
        recipient_pubkey,
        ""  # current_user
    )
    print(f"Encrypted: {ciphertext}")

    # Decrypt
    plaintext = signer.NIP44Decrypt(ciphertext, recipient_pubkey, "")
    print(f"Decrypted: {plaintext}")
except Exception as e:
    print(f"Encryption error: {e}")
```

### Python (Listening for Approval Signals)

```python
#!/usr/bin/env python3
"""Example: Listen for approval requests from GNostr Signer."""

from pydbus import SessionBus
from gi.repository import GLib

bus = SessionBus()
signer = bus.get("org.nostr.Signer", "/org/nostr/signer")

def on_approval_requested(app_id, identity, kind, preview, request_id):
    print(f"Approval requested:")
    print(f"  App: {app_id}")
    print(f"  Identity: {identity}")
    print(f"  Kind: {kind}")
    print(f"  Preview: {preview}")
    print(f"  Request ID: {request_id}")

    # Auto-approve for demonstration (in real app, show UI)
    user_approves = True
    remember = False
    ttl = 0

    ok = signer.ApproveRequest(request_id, user_approves, remember, ttl)
    print(f"Approval sent: {ok}")

def on_approval_completed(request_id, decision):
    print(f"Approval completed: {request_id} -> {'approved' if decision else 'denied'}")

# Subscribe to signals
signer.ApprovalRequested.connect(on_approval_requested)
signer.ApprovalCompleted.connect(on_approval_completed)

print("Listening for approval requests... (Ctrl+C to exit)")
loop = GLib.MainLoop()
loop.run()
```

### JavaScript (using dbus-native / Node.js)

```javascript
#!/usr/bin/env node
/**
 * Example: Sign a Nostr event using GNostr Signer D-Bus interface.
 */

const dbus = require('dbus-native');

const bus = dbus.sessionBus();

const service = bus.getService('org.nostr.Signer');

service.getInterface(
  '/org/nostr/signer',
  'org.nostr.Signer',
  (err, signer) => {
    if (err) {
      console.error('Failed to get interface:', err);
      process.exit(1);
    }

    // Get public key
    signer.GetPublicKey((err, npub) => {
      if (err) {
        console.error('GetPublicKey error:', err);
        return;
      }
      console.log('Public key:', npub);
    });

    // Sign an event
    const event = JSON.stringify({
      kind: 1,
      content: 'Hello from Node.js!',
      tags: [],
      created_at: Math.floor(Date.now() / 1000),
      pubkey: ''
    });

    signer.SignEvent(event, '', 'node-app', (err, signedEvent) => {
      if (err) {
        console.error('SignEvent error:', err);
        return;
      }
      const signed = JSON.parse(signedEvent);
      console.log('Signed event:', signed.id, signed.sig);
    });

    // NIP-44 encryption
    const recipientPubkey = 'abc123...'; // 64-char hex
    signer.NIP44Encrypt('Secret message', recipientPubkey, '', (err, ciphertext) => {
      if (err) {
        console.error('Encrypt error:', err);
        return;
      }
      console.log('Encrypted:', ciphertext);
    });
  }
);
```

### C (using GDBus)

```c
/**
 * Example: Sign a Nostr event using GNostr Signer D-Bus interface.
 * Compile: gcc -o sign_event sign_event.c $(pkg-config --cflags --libs gio-2.0)
 */

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>

#define SIGNER_BUS_NAME "org.nostr.Signer"
#define SIGNER_OBJECT_PATH "/org/nostr/signer"
#define SIGNER_INTERFACE "org.nostr.Signer"

int main(int argc, char *argv[]) {
    GError *error = NULL;
    GDBusConnection *conn;
    GVariant *result;

    // Connect to session bus
    conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error) {
        g_printerr("Failed to connect to session bus: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    // Get public key
    result = g_dbus_connection_call_sync(
        conn,
        SIGNER_BUS_NAME,
        SIGNER_OBJECT_PATH,
        SIGNER_INTERFACE,
        "GetPublicKey",
        NULL,                          // no parameters
        G_VARIANT_TYPE("(s)"),         // return type
        G_DBUS_CALL_FLAGS_NONE,
        -1,                            // default timeout
        NULL,
        &error
    );

    if (error) {
        g_printerr("GetPublicKey failed: %s\n", error->message);
        g_error_free(error);
        error = NULL;
    } else {
        const gchar *npub;
        g_variant_get(result, "(&s)", &npub);
        g_print("Public key: %s\n", npub);
        g_variant_unref(result);
    }

    // Sign an event
    const gchar *event_json =
        "{\"kind\":1,\"content\":\"Hello from C!\",\"tags\":[],\"created_at\":0,\"pubkey\":\"\"}";

    result = g_dbus_connection_call_sync(
        conn,
        SIGNER_BUS_NAME,
        SIGNER_OBJECT_PATH,
        SIGNER_INTERFACE,
        "SignEvent",
        g_variant_new("(sss)", event_json, "", "c-example"),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        30000,  // 30 second timeout for user approval
        NULL,
        &error
    );

    if (error) {
        g_printerr("SignEvent failed: %s\n", error->message);
        g_error_free(error);
    } else {
        const gchar *signed_event;
        g_variant_get(result, "(&s)", &signed_event);
        g_print("Signed event: %s\n", signed_event);
        g_variant_unref(result);
    }

    g_object_unref(conn);
    return 0;
}
```

### C (Listening for Signals)

```c
/**
 * Example: Listen for approval signals from GNostr Signer.
 * Compile: gcc -o listen_signals listen_signals.c $(pkg-config --cflags --libs gio-2.0)
 */

#include <gio/gio.h>
#include <stdio.h>

static void on_approval_requested(GDBusConnection *conn,
                                   const gchar *sender,
                                   const gchar *object_path,
                                   const gchar *interface_name,
                                   const gchar *signal_name,
                                   GVariant *parameters,
                                   gpointer user_data) {
    const gchar *app_id, *identity, *kind, *preview, *request_id;
    g_variant_get(parameters, "(&s&s&s&s&s)",
                  &app_id, &identity, &kind, &preview, &request_id);

    g_print("Approval requested:\n");
    g_print("  App: %s\n", app_id);
    g_print("  Identity: %s\n", identity);
    g_print("  Kind: %s\n", kind);
    g_print("  Preview: %s\n", preview);
    g_print("  Request ID: %s\n", request_id);

    // In a real app, show UI and call ApproveRequest
}

static void on_approval_completed(GDBusConnection *conn,
                                   const gchar *sender,
                                   const gchar *object_path,
                                   const gchar *interface_name,
                                   const gchar *signal_name,
                                   GVariant *parameters,
                                   gpointer user_data) {
    const gchar *request_id;
    gboolean decision;
    g_variant_get(parameters, "(&sb)", &request_id, &decision);
    g_print("Approval completed: %s -> %s\n",
            request_id, decision ? "approved" : "denied");
}

int main(int argc, char *argv[]) {
    GMainLoop *loop;
    GDBusConnection *conn;
    GError *error = NULL;

    conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error) {
        g_printerr("Failed to connect: %s\n", error->message);
        return 1;
    }

    // Subscribe to ApprovalRequested
    g_dbus_connection_signal_subscribe(
        conn,
        "org.nostr.Signer",
        "org.nostr.Signer",
        "ApprovalRequested",
        "/org/nostr/signer",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_approval_requested,
        NULL,
        NULL
    );

    // Subscribe to ApprovalCompleted
    g_dbus_connection_signal_subscribe(
        conn,
        "org.nostr.Signer",
        "org.nostr.Signer",
        "ApprovalCompleted",
        "/org/nostr/signer",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_approval_completed,
        NULL,
        NULL
    );

    g_print("Listening for approval signals... (Ctrl+C to exit)\n");
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_object_unref(conn);
    return 0;
}
```

### Shell (using gdbus)

```bash
#!/bin/bash
# Example: Interact with GNostr Signer using gdbus CLI

# Get public key
gdbus call --session \
  --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.GetPublicKey

# Sign an event
EVENT='{"kind":1,"content":"Hello!","tags":[],"created_at":0,"pubkey":""}'
gdbus call --session \
  --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.SignEvent \
  "$EVENT" "" "bash-script"

# Get relays
gdbus call --session \
  --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.GetRelays

# Monitor signals
gdbus monitor --session --dest org.nostr.Signer
```

---

## D-Bus Introspection XML

The full interface definition is available at:
`/usr/share/dbus-1/interfaces/org.nostr.Signer.xml`

```xml
<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
  "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
  <interface name="org.nostr.Signer">
    <method name="GetPublicKey">
      <arg name="npub" type="s" direction="out"/>
    </method>

    <method name="SignEvent">
      <arg name="event_json" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="app_id" type="s" direction="in"/>
      <arg name="signed_event" type="s" direction="out"/>
    </method>

    <method name="NIP44Encrypt">
      <arg name="plaintext" type="s" direction="in"/>
      <arg name="peer_pubkey" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="ciphertext" type="s" direction="out"/>
    </method>

    <method name="NIP44Decrypt">
      <arg name="ciphertext" type="s" direction="in"/>
      <arg name="peer_pubkey" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="plaintext" type="s" direction="out"/>
    </method>

    <method name="NIP04Encrypt">
      <arg name="plaintext" type="s" direction="in"/>
      <arg name="peer_pubkey" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="ciphertext" type="s" direction="out"/>
    </method>

    <method name="NIP04Decrypt">
      <arg name="ciphertext" type="s" direction="in"/>
      <arg name="peer_pubkey" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="plaintext" type="s" direction="out"/>
    </method>

    <method name="DecryptZapEvent">
      <arg name="event_json" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="decrypted_event" type="s" direction="out"/>
    </method>

    <method name="GetRelays">
      <arg name="relays_json" type="s" direction="out"/>
    </method>

    <method name="StoreKey">
      <arg name="key" type="s" direction="in"/>
      <arg name="identity" type="s" direction="in"/>
      <arg name="ok" type="b" direction="out"/>
      <arg name="npub" type="s" direction="out"/>
    </method>

    <method name="ClearKey">
      <arg name="identity" type="s" direction="in"/>
      <arg name="ok" type="b" direction="out"/>
    </method>

    <method name="ApproveRequest">
      <arg name="request_id" type="s" direction="in"/>
      <arg name="decision" type="b" direction="in"/>
      <arg name="remember" type="b" direction="in"/>
      <arg name="ttl_seconds" type="t" direction="in"/>
      <arg name="ok" type="b" direction="out"/>
    </method>

    <signal name="ApprovalRequested">
      <arg type="s" name="app_id"/>
      <arg type="s" name="identity"/>
      <arg type="s" name="kind"/>
      <arg type="s" name="preview"/>
      <arg type="s" name="request_id"/>
    </signal>

    <signal name="ApprovalCompleted">
      <arg type="s" name="request_id"/>
      <arg type="b" name="decision"/>
    </signal>
  </interface>
</node>
```

---

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `NOSTR_SIGNER_ENDPOINT` | Opt-in NIP-5F endpoint (`unix:/path`, `tcp:host:port`), gated like D-Bus | Unset: D-Bus only |
| `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS` | Enable `StoreKey`/`ClearKey` (`1` to enable) | Disabled |
| `NOSTR_SIGNER_MAX_CONNECTIONS` | Max concurrent TCP connections | 100 |
| `NOSTR_DEBUG` | Enable debug logging | Disabled |
| `NOSTR_SIGNER_SECKEY_HEX` | Fallback private key (64-char hex) | None |
| `NOSTR_SIGNER_NSEC` | Fallback private key (nsec1...) | None |

---

## Grants file

Remembered decisions live in `$XDG_CONFIG_HOME/gnostr/signer-grants.ini`
(see **Grants** above), shared by the D-Bus methods and the NIP-5F socket:

```ini
[event]
exe:/usr/bin/gnostr|npub1abc123...=allow
[nip44_decrypt]
flatpak:org.example.Chat|npub1abc123...=deny:1706745600
```

Format: `<principal>|<npub or *>=allow|deny[:<until-unix-ts>]`. The pre-0.4.0
`signer-acl.ini` (keyed on a claimed `app_id`) is not read by anything.

---

## Signer and Login Broker Boundary

The session signer (`org.nostr.Signer`) and the pre-login broker
(`nostr-authd` + `pam_nostr`) **do not talk to each other**, in either
direction, and must not be bridged.

- The signer is **app-facing**: post-login, session bus, user-owned secrets,
  admission by "same session user + session-bus policy". It has no
  pidfd/transaction machinery and must not gain the broker's.
- `nostr-authd` is **pre-login and authoritative**: a system daemon with
  `auth.sock` (0600) / `user.sock` (0666) and SO_PEERCRED admission
  (`gnome/nostr-homed/docs/AUTH_PROTOCOL.md`). Its local-key provider signs
  challenges in its own sandboxed worker with its own vault; it cannot depend
  on a user-session service, which does not exist at the greeter.
- The one cross-consumer is porthome's wrap-key path, which calls
  `NIP44Encrypt`/`NIP44Decrypt` on the session signer. That is a *porthome*
  consumer of the signer, not an authd edge, and it is unaffected by the
  0.2.0 `SignEvent` change.

The dependency-purity gate (`scripts/check-authd-dep-purity.sh`) enforces that
`nostr-authd` and `pam_nostr.so` link no signer client library.

---

## Related Documentation

- [GNostr Signer README](/apps/gnostr-signer/README.md)
- [Architecture Overview](/apps/gnostr-signer/ARCHITECTURE.md)
- [Daemon Deployment Guide](/apps/gnostr-signer/DAEMON_DEPLOYMENT.md)
- [NIP-04 Migration Guide](/docs/NIP04_MIGRATION.md)
- [NIP-44 Specification](/docs/NIP44.md)
- [Systemd Hardening](/docs/systemd-hardening.md)
