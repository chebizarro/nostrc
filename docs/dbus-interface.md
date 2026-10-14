# GNostr Signer D-Bus Interface

This document describes the D-Bus interfaces exposed by `grotto-daemon` for secure Nostr key management and cryptographic operations.

## Overview

The GNostr Signer provides a secure D-Bus service for:

- **Key Management**: Secure storage and retrieval of Nostr private keys
- **Event Signing**: Cryptographic signing of Nostr events (NIP-01)
- **Encryption/Decryption**: NIP-04 (legacy) and NIP-44 (modern) message encryption
- **Zap Processing**: Decryption of zap receipt events (NIP-57)
- **User Approval**: Interactive approval flow for sensitive operations

Client signing/encryption APIs never return private keys. The trusted Grotto management UI may explicitly request an encrypted backup or a separately deliberate raw-key reveal; management export is never reachable through client permissions.

## Bus Name and Object Path

| Property | Value |
|----------|-------|
| **Bus Name** | `org.nostr.Signer` |
| **Object Path** | `/org/nostr/signer` |
| **Interface** | `org.nostr.Signer` |
| **Bus Type** | Session bus (default) or System bus (`--system` flag) |

## Grotto management: `org.nostr.Grotto.Control1` (G04)

Canonical XML: [`apps/grotto/data/org.nostr.Grotto.Control1.xml`](../apps/grotto/data/org.nostr.Grotto.Control1.xml).
It is exported on the **existing daemon connection**, bus **`org.nostr.Signer`**,
object **`/org/nostr/signer/grotto`**. The GUI's `org.nostr.Grotto` name is not the
service destination and owning that name grants no authority. Legacy
`org.nostr.Signer` method signatures and activation remain unchanged. Its additive
`IdentitiesChanged(revision:t)` signal is a coarse inventory invalidation (no keys,
npubs or labels); consumers re-read `ListIdentities`, including after owner change.
The revision increases within one daemon lifetime, not across restarts. A completed
store emits it even if the initiating GUI closes before receiving its receipt.
Management selection and creating/importing/rotating a key never change the legacy
default: empty-selector clients honor the explicitly configured default before
cache or environment fallbacks.

### Authentication and compatibility

Every sensitive read and mutation uses a **fresh** `signer_caller_is_approver()`
check (verified installed path + device/inode, or attested approval-UI app).
Grotto consumes the nip55l implementation headers with PRIVATE build includes;
they are installed under `libexec/grotto/private/nip55l`, not advertised as a
public authorization API. The UID-only-bus limitation below also applies here.
Two owner-confirmed exceptions (2026-10-12): **any same-session, same-UID caller**
may read the coarse, identity-free `GetStatus`, or call **`SetPaused(true)`**.
`SetPaused(false)` still requires the approver. This lets the untrusted Shell
indicator inspect status and reduce capability, never resume or approve.

`GetStatus` returns `control_version:u = 1`, `control_epoch:s`,
`capabilities:a{sb}`, `paused:b`, `pending_count:u`, `bunker_enabled:b`,
`bunker_state:s`, `key_state:s`, `key_lock_enforced:b`, and
`activity_log_healthy:b`, `client_store_healthy:b`, `permission_store_healthy:b`.
It includes no identity, client, request contents, reason or activity records.
`key_state="unknown"` and `key_lock_enforced=false` do **not** claim a locked keyring.
Mnemonic export, canonical mnemonic storage, identity self-test and rotation are
available with the G09 implementation. Mnemonic availability is per identity:
keys imported without Grotto-created seed words return `NotFound`. Raw-nsec-export
is separate from NIP-49 export, which is advertised only when its backend is built.
Pairing/configuration/key-lock capabilities remain owned by their implementation
steps; always use the returned flags rather than inferring them from a version.

A GUI must feature-detect Control1/version/capabilities: absence or an older
version means a read-only **Update Grotto** state, not legacy mutation fallback.
The daemon keeps the preserved legacy client/approval contract for older UIs.
G04 establishes the daemon side; GUI version-skew presentation belongs to G07/G10.

### Methods

Management errors use `org.nostr.Grotto.Control1.Error.<name>`; self-test terminal
errors use the shared gate's `org.nostr.Signer.Error.*` names. Invalid signatures
are rejected by GDBus using the canonical XML before dispatch.

| Method | Input → output | Current behavior |
|---|---|---|
| `GetStatus` | `() → a{sv}` | Coarse version, capabilities, runtime and store health |
| `ListRequests` | `() → aa{sv}` | Full pending snapshots from the shared approval service |
| `ListClients` | `() → aa{sv}` | Durable consent/permission clients, trust tier, pairing/relays, permission count and needs-review flag |
| `ListPermissions` | `(principal:s) → aa{sv}` | G03's explicit/needs-review rows, unmodified; live tier policy is in the client record, never expanded into fake persisted grants |
| `SetPermissions` | `(principal:s, identity:s, permissions:aa{sv}, expected_revision:t) → ()` | G03 validates/persists a complete replacement for this principal/identity pair; stale revision → `Conflict` |
| `RevokeClient` | `(principal:s) → ()` | Cancel pending/queued work, remove tier/pairing admission, clear G03 permissions and disconnect bunker bookkeeping; retry is idempotent |
| `SetClientLabel` | `(principal:s, label:s) → ()` | Edit an existing client, never register a forged caller |
| `SetClientTrust` | `(principal:s, tier:s) → ()` | `low`, `medium`, `full`; requires prior explicit consent; persist before live G03 policy invalidation |
| `SetPaused` | `(paused:b) → ()` | Session-only generation-fenced pause; asymmetric authority as above |
| `SetDefaultIdentityForLegacy` | `(identity:s) → ()` | Resolve a stored signable identity; update daemon GSettings default, or clear with empty input; explicit consumers are unchanged |
| `ExportIdentity` | `(identity:s, format:s, passphrase:s) → s` | NIP-49 `ncryptsec` (empty format defaults to it; nonempty UTF-8 passphrase ≤1024 bytes, `log_n=16`, NFKC normalization) or custody-held `mnemonic` (no export passphrase); explicit nonempty identity required |
| `ExportRawIdentity` | `(identity:s) → s` | Separate authenticated raw-nsec reveal, never accepted through `ExportIdentity` |
| `StoreKeyWithMnemonic` | `(key:s, label:s, mnemonic:s) → s` | Validate the BIP-39 phrase and empty-passphrase NIP-06 key binding, store both in daemon custody using the canonical npub, return that npub; a label never replaces another identity |
| `ImportIdentity` | `(format:s, input:s, passphrase:s, label:s) → s` | Validate and import `key` (nsec/hex), `ncryptsec` (NIP-49 decryption password), or `mnemonic` (optional BIP-39 passphrase); return the exact canonical stored npub |
| `SelfTestIdentity` | `(identity:s) → s` | One approver-only, non-rememberable MANAGEMENT request: sign and verify an unpublished event, then independent-peer NIP-44 challenge round-trip; `passed` only after verification; no relay I/O, client record or grant |
| `CancelIdentitySelfTest` | `(identity:s) → ()` | Cancel only this approver principal's self-test for this exact identity; never remove a stored key |
| `RotateIdentity` | `(identity:s) → s` | Create and store a mnemonic-backed successor, save an old-key-signed migration announcement, return the successor npub. No publication, redirection, permission copy or old-key removal; pause-generation fenced |
| `GetIdentityMigration` | `(identity:s) → s` | Read the saved public signed migration event for an exact old npub; user/application chooses whether to publish it |
| `ConfigureBunker` | `(config:a{sv}) → a{sv}` | Authenticated typed unavailable response; G06 implements durable desired config + live apply |
| `CreatePairing` | `(identity:s, label:s) → a{sv}` | Authenticated typed unavailable response; G05/G06 implement account-bound, single-use pairing |
| `AcceptConnectURI` | `(uri:s, identity:s) → s` | Authenticated typed unavailable response; G06 validates/reviews/acks, no relay contact now |
| `CancelPairing` | `(pairing_id:s) → ()` | Authenticated typed unavailable response; G06 invalidates token + request |
| `ResetBunkerEndpoint` | `() → ()` | Authenticated typed unavailable response; G05/G06 rotate transport custody and revoke pairing |
| `ListActivity` | `(before_sequence:t, limit:u) → aa{sv}` | Newest first; 0 cursor = latest, otherwise strictly before cursor; limit 0 = empty, >100 → `InvalidInput` |
| `ClearActivity` | `() → ()` | UI confirms first; atomic clear, preserving sequence high-water mark |

Bunker unavailable responses consult the actual `BunkerService`: **`NotReady`**
when absent/stopped/starting/error, **`NotSupported`** when its current state is
RUNNING but the management feature is not implemented. They never return a fake
pairing URI, claim durable config or call the legacy nonfunctional connect path.
Each dispatcher branch names its G05/G06 owner. The prospective `bunker.ini` v1
(`enabled`, `relays`, `allowed-methods`) is G06-owned: G04 does not manufacture or
accept it through an unavailable ConfigureBunker method. Runtime state still
comes from the existing service; it does not claim protocol-proven LISTENING.

`ExportIdentity(format="mnemonic")` reads only `org.nostr.Grotto.Mnemonic`, a
separate Secret Service/Keychain custody role keyed by npub, and verifies its
empty-passphrase derivation against the selected identity. It is never included
in `ListIdentities`. nsec/hex/NIP-49 imports have no invented seed words: use the
encrypted backup instead. `format="nsec"` is deliberately rejected: the UI must use the separate
raw method after its warning. Exports use the nip55l custody resolver, not the
GUI's secret store, run off the daemon main loop (one at a time), wipe owned
passphrases/key buffers/results, re-attest the approver before secret delivery,
and suppress delivery during shutdown. They create no signing grant or secret
history entry. Private material necessarily transiently exists in the trusted
UI and D-Bus message buffers; this is not a same-user malware boundary.
`IdentitySelfTestTesting(identity:s)` is unicast to the invoking approver after
one-time consent, before backend execution. Denial, expiry, cancellation, missing
key/backend and verification failure never mean “ready”. Closing the assistant
cancels its self-test/local continuation but preserves a completed daemon key
store. Local `accounts.ini` setup metadata records `verification-pending`,
`backup-pending` or `ready`; management selection is in-memory only. Rotation
announcements are private-mode, atomic public JSON under
`$XDG_CONFIG_HOME/grotto/migrations/<old-npub>` and contain no secrets. A persistence
failure after a successor store is reported explicitly; re-read identities before
retrying. The old identity remains until explicit confirmed removal.
On UID-only buses (the macOS compatibility path, see the known limitation
below) any unattested same-UID process passes the approver check, so it can call
`ExportRawIdentity`/`ExportIdentity` and receive the key; delivery re-attestation
does not change that. Approver isolation of exports holds on PID-attested Linux.

### Revisions, lost updates and signals

`RequestsChanged(t)`, `ClientsChanged(t)`, `StatusChanged(t)` carry monotonic
**snapshot revisions within one daemon epoch**. The corresponding `GetStatus`
fields are `requests_revision`, `clients_revision`, `status_revision`; permission
replacement uses the distinct opaque `permissions_revision`. The latter combines
a random process epoch with G03's admission counter, so remembered legacy writes,
scoped edits and trust changes invalidate stale replacements and a pre-restart
revision cannot normally ABA into the new process. Revisions are not credentials.
On name-owner/epoch changes, discard all old snapshots/cursors for mutations.
For array snapshots (including empty ones), bracket reads with GetStatus and
reject/re-read if their revision changed, in addition to subscribing **before**
listing. Never poll on a timer. `SetPermissions` returns `Conflict` without
replacing authority after any revision mismatch; re-read and reapply the edit.
The guarantee assumes the running daemon is the sole writer of
`signer-permissions-v2.ini` (it is the only owner of `org.nostr.Signer`; remembered
`ApproveRequest` writes and Control1 replacements both run on its owner thread).
The revision check is not a cross-process file transaction: another program
editing that file directly can lose an update without a `Conflict`. Such writers
are unsupported; older GUIs write only the untrusted legacy `signer-grants.ini`.

`ActivityChanged(t)` carries the **activity record sequence**, not a snapshot
revision. The persistent high-water mark is `activity_sequence`. Pagination is
strictly `before_sequence`; on a change, fetch the newest page and stop merging
when the prior sequence is reached. Clear/retention removal can signal the same
high-water mark; re-read the page rather than treating this as a new record.

### Daemon storage and failure semantics

`$XDG_CONFIG_HOME/grotto/clients.ini`, schema **v1**, is daemon-owned: SHA-256
encoded record sections, canonical principal, most recent transport, label,
untrusted `claimed-name`, selected `identity`, `tier` (Low default), `paired`,
`relays`, `first-consented-at`, `last-used-at`. Browser origins are separate
principals, not the bridge's identity. Local records arise only from explicit
consent or successfully created permissions, never arbitrary failed requests.
Pairing admission never grants signing. New directories are 0700, atomic durable
files 0600; profile guards refuse the real profile under tests. Unsupported
versions/malformed admission records refuse startup, not silently reset trust.

The **existing `event_history.c` manager** has a daemon metadata mode stored in
`$XDG_CONFIG_HOME/grotto/activity.json`, envelope v1 (`version`, `sequence`,
`records`), distinct from the non-authoritative legacy GUI `event_history.json`.
No competing logger or legacy-content import is introduced. Terminal shared-gate
notifications produce exactly one record per request (joined continuations share
that request): `sequence:t`, `request_id:s`, `timestamp:t`, `transport:s`,
`principal:s`, `identity:s`, `operation:s`, optional `event_kind:u`, `outcome:s`,
sanitized `error_code:s`. Outcomes: `success`, `denied`, `expired`,
`expired-caller` (last caller continuation disappeared), `cancelled`, `failed`.
No previews, raw JSON, reasons, URIs, decrypted messages, ciphertext or crypto
results are retained. Retention is 30 days / 10,000 records, newest-first.
Unsupported/corrupt activity schemas (including a stored `error_code` that is not
a bare error name) are read-only/unhealthy and never overwritten; records made
meanwhile stay in memory under the same 30-day / 10,000 bound.

Logging failure never changes a crypto result; health is exposed in status.
Activity clear and client edits fail rather than reporting an unpersisted change.
G03 permission-persistence failure leaves authority unchanged. Permission and
client metadata are separate atomic files: if permissions persist but the
subsequent client metadata save fails, the method reports `PersistenceFailed`,
revision/health reflect the partial result, and the UI **must re-read**, not
assume permissions were rolled back. Revoke likewise cancels work immediately
and may remove admission before reporting a later permission-storage failure.
For an emergency capability reduction independent of persistence, use Pause.

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
their own trusted-approver gate (the daemon's explicit
`NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1` override is only for tests/scripts).

**Who is calling (principal).** On PID-attested buses, derived by the daemon
from the bus connection, never from an argument (nostrc-phk4):
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

**Known limitation — UID-only buses.** On buses that report the caller's UID
but no PID (including the supported macOS compatibility path), claimed names
remain the `claimed:<app_id>` principal, and unattested same-UID callers pass
the approver check. Another same-UID process can therefore claim a remembered
principal or answer approvals; the verified caller-isolation guarantee holds
only on PID-attested Linux, not this inherited compatibility path.
`CallWithContext` preserves the same limitation; its context is not a credential.

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

**Permissions (0.7.0).** `$XDG_CONFIG_HOME/gnostr/signer-permissions-v2.ini`
stores principal + pinned identity + operation + optional validated event kind.
Event approvals and denials are exact-kind by default; crypto operations remain
separate (`nip44_conversation_key`, `nip04_encrypt/decrypt`,
`nip44_encrypt/decrypt`, `zap_decrypt`). Raw/B64 variants share the same operation
scope. Expiry is absolute Unix seconds, 0 = forever, never extended by use.
Explicit deny wins, including legacy operation-wide/wildcard denials;
required-confirmation kinds always prompt despite remembered allows or trust tiers.
New identity-wildcard allows are supported only for `get_relays`; valid inherited
non-event wildcard allows are preserved during the one-time legacy import.
The single nip55l evaluator applies live per-client Low/Medium/Full policy (unknown
clients Low) through Grotto's daemon-owned `clients.ini` trust lookup. Control1
trust/admission mutations invalidate the same evaluator immediately; lowering a
tier preserves explicit grants and denies.
Conversation-key export requires an explicit allow, not a tier/default shortcut;
pairing cannot inherit a remembered allow.
The compatibility `signer-grants.ini` is re-sanitised on every load, but imported
only before v2 exists. Valid non-event rows retain their original expiry (including
forever); broad event denies remain operation-wide. Broad event allows become
non-executable **Needs review**, pruned after 90 days, and are removed before initial
evaluation is enabled. After that v2 alone is authoritative: later legacy writes
cannot add, widen, deny or resurrect permissions, and create no new review rows.
A corrupt legacy file is ignored/sanitised; a bad v2 record is quarantined only for
its recoverable scope, with sanitised logging, not a global evaluation failure.
Existing shared directory modes are unchanged; atomic permission files use 0600.
No bunker global auto-approval or remote allowlist becomes signing authority.
Initial migration/storage failures fail closed. Queued automatic requests recheck
changed revisions/expiry and proceed if still allowed; explicit approvals survive
unrelated changes. Covering deny/revoke invalidates queued work. See [55L](proposals/55L.md#scoped-permission-storage-and-migration-070).
The pre-0.4.0
`~/.config/gnostr/signer-acl.ini` (keyed on the claimed `app_id`) is ignored.
Built-in defaults cover first-party services that run without a UI
(`get_public_key`/`get_relays` for the nostr-homed helpers and
`nostr-notify-daemon`, `nip44_decrypt` for `nostr-homectl`), matched on the
installed executable's path and inode; CMake `NIP55L_DEFAULT_GRANTS`. An
explicit deny in the permission store wins; these defaults go through the same evaluator.

**Approval.** Without a grant the call is parked and `ApprovalRequested`
is emitted with `app_id` = principal and `identity` = resolved npub.
Only identical principal + pinned identity + operation + canonical argument
digest calls join a pending request; different payloads/peers do not share
consent. A disconnect removes its continuations, not another connection's
joined work. Request IDs are opaque CSPRNG values. Only the installed
approval UI — `<bindir>/grotto` (path + inode) or attested `org.nostr.Grotto`
Flatpak — may answer/read approvals; others get `Error.PermissionDenied`.
If `org.nostr.Grotto` has no owner, the daemon asynchronously asks the bus
to start it; failed activation returns `Error.NoApprovalAgent` to opted-in
clients (`ApprovalDenied` otherwise). Unanswered requests expire after
300 s or an earlier authenticated transport deadline. The shared service
serializes backend work off its owner context, rechecks pinned identity and
denies before execution, and suppresses in-flight results on pause/revoke.
The session pause/generation hook is internal until G04 adds Control1.

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

**Security**: Trusted approval UI only (fresh path/inode or attested Flatpak
identity check; unattested callers refused). Other callers require the
explicit daemon tests/scripts override `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1`.

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

**Security**: Trusted approval UI only (fresh path/inode or attested Flatpak
identity check; unattested callers refused). Other callers require the
explicit daemon tests/scripts override `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1`.

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
- `remember` (boolean): Remember the decision for (principal, pinned identity,
  operation, validated event kind) in the v2 permission store; ignored for an
  unidentified caller. Since 0.7.0, `event` no longer grants every event kind.
  Required-confirmation kinds still require fresh approval on subsequent calls;
  remembered denial wins. Persistence failure returns failure, never confirmed
  remembered access.
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
Added in 0.7.0 (all existing fields and signatures preserved): `transport`,
`operation`, `reason`, `reason_source` (`caller` or `signer`), `flow_id`,
`state` (`s`); `event_kind` (`u`, only when present; absence is not kind 0);
`event_summary` (`a{sv}`, safe validated type/preview/tag count/peer metadata);
`expires_at` (`t`, Unix seconds); `remaining_seconds` (`u`, authoritative
monotonic snapshot). No private bodies, ciphertext, keys or pairing tokens
are exposed in the summary. Unknown fields may be ignored by legacy UIs.
Unknown request: `Error.NotFound`.

`event_summary` keys (additive in unreleased 0.7.0):

| Key | Type | Meaning / redaction |
| --- | --- | --- |
| `type` | `s` | `event`, or the validated operation name |
| `preview` | `s` | Bounded public note/article/comment preview; otherwise generic event or encrypted-message text. Crypto operations contain only operation/peer metadata. |
| `content` | `s` | Public note/article/comment plain text only; valid UTF-8, at most 280 Unicode characters, ASCII controls replaced with spaces |
| `peer` | `s` | Validated public peer fingerprint for encrypt/decrypt/conversation-key operations, never a private message body |
| `tag_count` | `u` | Total tag count for signing |
| `tag_counts` | `a{su}` | Sorted counts keyed by tag name, not values: ASCII alphanumeric/underscore/hyphen names of 1–32 bytes, at most 64 distinct names; other names omitted |
| `auth_relay` | `s` | Exact sole AUTH relay URI, printable ASCII and at most 2048 bytes; valid `ws`/`wss` host, no credentials, query or fragment. Missing for unsafe/ambiguous relay tags. Challenges are never included. |
| `delete_target_count` | `u` | Number of `e`/`a` deletion target tags; no target payloads |
| `target_event_id` | `s` | Last `e` tag for repost/generic repost/reaction, only when a validated 64-hex public event ID; lowercase |
| `target_kind` | `u` | Last `k` tag for those operations, only when a validated decimal kind in 0–65535 |
| `reaction_value` | `s` | Public reaction text, valid UTF-8 and bounded to 280 characters (including empty, the protocol's default reaction) |
| `recipient_peer` | `s` | Last `p` tag of private/encrypted-message signing, only when a validated 64-hex public recipient fingerprint; lowercase. May be absent for sealed events. |

No embedded repost JSON, unknown/custom-event content, arbitrary tag values,
private bodies, ciphertext, secrets or pairing tokens are copied into the summary.
Missing fields must not be invented from raw JSON by an approval UI; essential
AUTH/deletion/target metadata must be present before approving those operations.
These are presentation fields, not permission grants or fetched profile data.

#### CallWithContext

*Additive in 0.7.0.* `(method:s, arguments:v, context:a{sv}) → reply:v`.
`arguments` wraps the exact existing input **tuple**; `reply` wraps the
existing output tuple, currently `(s)`. Whitelist: `SignEvent`,
`NIP44DeriveConversationKey`, `NIP04Encrypt`,
`NIP04Decrypt`, `NIP44Encrypt`, `NIP44Decrypt`. All use the same validated
shared-service submission as their direct handlers; no self-D-Bus calls.
`GetPublicKey`/`GetRelays` remain direct-only because D-Bus cannot encode
their empty input tuple inside `v`; no special sentinel is accepted.
`*ForApp`, `*B64`, `DecryptZapEvent` and `EnableTypedApprovalErrors` remain
direct-only, as do management methods. Unknown names, recursion, wrong
signatures and invalid context return `Error.InvalidInput`.

Context keys: `reason:s` (≤160 Unicode characters and ≤512 UTF-8 bytes),
`purpose:s`, `flow_id:s` (≤64 bytes each). Duplicate/unknown keys or wrong
types are rejected. Context never grants authority or overrides a principal
or identity. The calling connection's typed-error opt-in applies to wrapped
calls. Consumers feature-detect and fall back to direct methods if absent.

The GTK-free `SignerApprovalService` is shared by D-Bus and NIP-5F and
provides NIP-46-ready `NIP04*`, `CONNECT`/`PING` (explicit no-identity mode),
cancel handles and authenticated `nip46:<lowercase client hex>` principals.
Remote callers are identified/rememberable, never approvers; G05 owns their
transport admission and runtime wiring. Retained headless defaults and the
full executable/operation list are documented in [55L](proposals/55L.md#built-in-first-party-headless-grants).

#### ListGrants / RevokeGrant

*Additive (nostrc-yjky), tightened in 0.7.0.* The operation-wide remembered
rows in the v2 permission store, for older approval UIs (approval-UI
callers only, like `ApproveRequest`; others get `Error.PermissionDenied`).
The UI never reads the file itself.

```xml
<method name="ListGrants">
  <arg name="grants" type="a(sssbt)" direction="out"/>
</method>
<method name="RevokeGrant">
  <arg name="kind" type="s" direction="in"/>
  <arg name="principal" type="s" direction="in"/>
  <arg name="identity" type="s" direction="in"/>
  <arg name="removed" type="b" direction="out"/>
</method>
```

Each grant is `(kind, principal, identity, allow, expires_at)`: `identity`
is an npub or `*`; `expires_at` is unix seconds, `0` = never (expired
entries are listed so they can be cleaned up). Built-in defaults for
first-party services and tier-derived permissions are not listed. Scoped event
rows and Needs review markers are not listed; new management uses ListPermissions
(G04). `RevokeGrant("event", principal, identity)` removes **all** event scopes
for the pair, including operation-wide denies and Needs review. Other operations
remove their operation-wide row. Successful removal cancels that principal's
pending/queued work. Removing an explicit row does not remove a live tier's
permission; otherwise the next matching call prompts again. Empty
arguments: `Error.InvalidInput`.

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

- **Key mutations are for the signer's own UI** (nip55l 0.7.0): `StoreKey`, `ClearKey` and `CreateProfile` are allowed to the trusted approval UI (identified as for `ApproveRequest`); any other caller needs the daemon started with `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1`
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

The interface definition has one copy in the tree,
[`nips/nip55l/dbus/org.nostr.Signer.xml`](../nips/nip55l/dbus/org.nostr.Signer.xml):
the daemon's skeleton, nostr-gobject's proxy (`signer_proxy`) and the tests
are all generated from or checked against it (nostrc-56id). It is installed
as `$datadir/dbus-1/interfaces/org.nostr.Signer.xml`; a running signer also
answers `org.freedesktop.DBus.Introspectable.Introspect`.

---

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `NOSTR_SIGNER_ENDPOINT` | Opt-in NIP-5F endpoint (`unix:/path`, `tcp:host:port`), gated like D-Bus | Unset: D-Bus only |
| `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS` | Let any caller use `StoreKey`/`ClearKey`/`CreateProfile` (`1`); without it, only the trusted approval UI may | Unset |
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
