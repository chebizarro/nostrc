# NIP-55L Linux Signer

**Component version: 0.5.0** (tracked in `/VERSION_MANIFEST.md`; authoritative
source `NOSTR_NIP55L_VERSION_*` in `include/nostr/nip55l/signer_ops.h`).
0.5.0 is additive: `EnableTypedApprovalErrors` opts a bus connection in to
`Error.ApprovalTimedOut` and `Error.NoApprovalAgent` where it would otherwise
get `Error.ApprovalDenied` (see below). 0.4.0 changes the ACL semantics: grants are keyed on a bus-derived caller
principal plus the resolved npub and request kind, `GetPublicKey`,
`GetRelays`, NIP-04/NIP-44 and `DecryptZapEvent` are approval-gated, only the
installed approval UI may call `ApproveRequest`, and `*ForApp` /
`GetApprovalInfo` were added (see *Access control* below and
`docs/dbus-interface.md`). 0.3.0 was additive: new approval-gated `NIP44DeriveConversationKey` (used by
`nostr-seal`, nostrc-da9c). 0.2.0 was a breaking change for D-Bus clients:
`SignEvent` returns the complete signed event JSON instead of the bare
signature — see below.

This component provides a local signer daemon exposing a GLib/GDBus interface for Nostr signing and peer-to-peer encryption (NIP-04 and NIP-44 v2). It also ships a small CLI that talks to the DBus service.

- Service name: `org.nostr.Signer`
- Object path: `/org/nostr/signer`
- Interface: `org.nostr.Signer`

## Build

The `nips/nip55l` targets are built as part of the top-level CMake project.

```
cmake -S . -B build
cmake --build build --target nostr_nip55l_core nostr_nip55l_glib nostr-signer-daemon nostr-signer-cli -j 8
```

Optional dependencies:
- OpenSSL (required for NIP-44)
- libsecp256k1 (ECDH/NIP-44)
- GIO (GLib DBus)
- libsecret (optional: Secret Service-based key storage)

## Running (DBus)

Start a session bus if not present. On macOS:

```
brew install dbus
brew services start dbus
```

Run the daemon:

```
# Optional: allow key mutations over DBus
export NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1

# Optional: provide a secret for key resolution (hex or nsec)
export NOSTR_SIGNER_SECKEY_HEX=...   # or
export NOSTR_SIGNER_NSEC=nsec1...

build/nips/nip55l/nostr-signer-daemon
```

### D-Bus activation

`org.nostr.Signer.service` is owned by **grotto**, which activates
`grotto-daemon` (the same `nips/nip55l` GLib service plus the GUI's
approval flow). This tree does **not** install an activation file by default,
so the two packages never ship conflicting copies of the same path.

Headless installs that want `nostr-signer-daemon` auto-activated without
grotto build with:

```
cmake -S . -B build -DENABLE_NIP55L_STANDALONE_ACTIVATION=ON
```

which configures `dbus/org.nostr.Signer.service.in` (absolute `Exec=` path)
and installs it to `share/dbus-1/services/`. Do not enable it alongside a
grotto package.

## Key resolution order

The daemon resolves the signing key in this order:

1. `current_user` parameter (64-hex or `nsec1...`)
2. Env `NOSTR_SIGNER_SECKEY_HEX` (64-hex)
3. Env `NOSTR_SIGNER_NSEC` (`nsec1...`)
4. Secret Service (libsecret), attribute `account="default"` (or specified)

If none are found: returns NOT_FOUND.

## Access control (0.4.0)

- **Principal** — derived from the D-Bus connection (`GetConnectionCredentials`
  + `/proc/<pid>`): `flatpak:<id>`, `app:<id>;exe:<path>`, `snap:<name>;exe:<path>`,
  `exe:<path>`; never from an argument (`src/glib/signer_caller.[ch]`).
- **Web origins** — an `app_id` like `https://site` is the principal only when
  the caller is the installed `nostr-signer-webext-host` (path + inode); from
  anyone else it is an unverified label. Same rule as the wallet agent.
- **Grants** — `$XDG_CONFIG_HOME/gnostr/signer-grants.ini`, section = request
  kind, key `<principal>|<npub>` or `<principal>|*`, value
  `allow|deny[:<until>]`. The npub is what the identity selector resolves to,
  so `""`, a key_id and the npub hit the same grant. The old
  `signer-acl.ini` is not read.
- **NIP-5F socket** — opt-in (`NOSTR_SIGNER_ENDPOINT=unix:<path>`,
  `nip55l_nip5f.h`, `src/glib/signer_nip5f.c`): every method goes through the
  same gate (`src/glib/signer_gate.h`). The principal comes from the socket
  peer's kernel credentials (`SO_PEERCRED`/`SO_PEERPIDFD`, macOS
  `LOCAL_PEERPID`) and `/proc`, the same shape as over D-Bus, so grants are
  shared; prompts are the same `ApprovalRequested`.
- **Approval** — without a grant: `ApprovalRequested(principal, npub, kind,
  preview, id)`; identical queued calls from one connection share it;
  `ApproveRequest` only from `<bindir>/grotto` (or its Flatpak), fails
  fast when no approval UI owns `org.nostr.Grotto`, expires after 300 s.
- **Defaults** — first-party headless services (nostr-homed helpers,
  `nostr-notify-daemon`) are granted `get_public_key`/`get_relays`, and
  `nostr-homectl` `nip44_decrypt`, by executable (CMake `NIP55L_DEFAULT_GRANTS`).
- **Trust lists** (CMake cache): `NIP55L_ORIGIN_BRIDGE_PATHS`,
  `NIP55L_APPROVAL_UI_PATHS`, `NIP55L_APPROVAL_UI_FLATPAK_ID`,
  `NIP55L_APPROVAL_UI_BUS_NAME`. Test builds (`NIP55L_TEST_TRUST_ENV`, default
  `BUILD_TESTING`) honour `NOSTR_SIGNER_TEST_ORIGIN_BRIDGES` /
  `NOSTR_SIGNER_TEST_APPROVERS`.
- **macOS** — the bus reports no client PIDs, so callers are keyed as
  `claimed:<app_id>` and any same-user process may approve (unverified).

## DBus API

Interface: `org.nostr.Signer`

Every method below except `StoreKey`/`ClearKey` is approval-gated (0.4.0);
each gated method except `SignEvent` and `NIP44DeriveConversationKey` (which
already take one) has a `…ForApp` twin with a trailing `app_id`.

- `GetPublicKey() -> (s npub)`
  - Returns the `npub1...` for the current secret key.

- `SignEvent(in s eventJson, in s currentUser, in s requester) -> (s signed_event)`
  - Signs a Nostr event JSON and returns the **complete signed event JSON**
    (`id`, `pubkey`, `created_at`, `kind`, `tags`, `content`, `sig`).
    `pubkey` is always the signing key's; `created_at` 0 is filled with now.
    `currentUser` may be empty to use resolution order above.
  - Since 0.2.0. Earlier versions returned only the 128-hex signature; the
    D-Bus type (`s`) did not change, so update callers to parse JSON.

- `NIP04Encrypt(in s plaintext, in s peerPubHex, in s currentUser) -> (s cipherB64)`
- `NIP04Decrypt(in s cipherB64, in s peerPubHex, in s currentUser) -> (s plaintext)`

- `NIP44Encrypt(in s plaintext, in s peerPubHex, in s currentUser) -> (s cipherB64)`
- `NIP44Decrypt(in s cipherB64, in s peerPubHex, in s currentUser) -> (s plaintext)`

- `NIP44EncryptB64` / `NIP44DecryptB64`: binary-safe NIP-44 (plaintext side is base64).

- `NIP44DeriveConversationKey(in s peerPubKey, in s identity, in s app_id) -> (s conversationKey)`
  - Since 0.3.0. The NIP-44 v2 conversation key between `identity` and the
    64-hex x-only `peerPubKey` (`HKDF-extract(SHA256, ECDH shared-x, "nip44-v2")`,
    same as `nostr_nip44_convkey`), as 64 lowercase hex. The secret key never
    leaves the daemon.
  - **Approval-gated** like `SignEvent`, kind `nip44_conversation_key`,
    preview `derive NIP-44 conversation key with <peer>`.
  - The returned key opens every NIP-44 payload exchanged with that peer, so
    grant it for throwaway peers: `nostr-seal` only ever asks with a per-file
    ephemeral key.
  - Errors: `Error.InvalidInput` (not 64-hex / not on the curve — refused
    before any prompt), `Error.ApprovalDenied`, `Error.RateLimited`,
    `Error.NoKeyConfigured`, `Error.Internal`.

- `EnableTypedApprovalErrors() -> ()`
  - Since 0.5.0; ungated. Opts the calling bus connection in, until it
    disconnects, to typed approval errors: an approval request that expires
    unanswered fails with `Error.ApprovalTimedOut`, and a call that needs a
    prompt while no approval UI is on the bus fails with
    `Error.NoApprovalAgent`, and an approved call whose selector no longer
    resolves to the approved npub (the active account switched during the
    prompt) fails with `Error.IdentityChanged`. `Error.ApprovalDenied` then
    means a denial only.
    Without the opt-in (every pre-0.5.0 client, and the NIP-5F socket) those
    cases stay `Error.ApprovalDenied`. Classify by error name, never by message.

- `GetRelays() -> (s relaysJson)`
  - JSON array of the user's explicitly configured relays. Sources, first
    match wins: `$XDG_CONFIG_HOME/nostr/relays.conf` (a JSON array of
    `ws://`/`wss://` URL strings), then the relay list set in grotto
    (GSettings `org.nostr.Grotto` `relays`, user-written value only, not the
    schema default).
  - No network access: NIP-65 lists are never fetched to answer this.
  - `org.nostr.Signer.Error.NotFound` when nothing is configured — callers
    fall back to their own relays; `org.nostr.Signer.Error.InvalidConfig`
    when `relays.conf` is malformed.

- `StoreKey(in s key, in s identity) -> (b ok)`
  - Stores a private key (64-hex or `nsec1...`) in Secret Service under schema `org.nostr.Signer`, attribute `identity`. Optional feature; returns error if libsecret is unavailable.
  - ACL: requires env `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1` and subject to a per-sender 500ms rate limit.

- `ClearKey(in s identity) -> (b ok)`
  - Clears stored key for `identity`. Same ACL/rate limit as above.

Errors are D-Bus error names under `org.nostr.Signer.Error.*`
(`nip55l_dbus_errors.h`); see `docs/dbus-interface.md` for the table.

## CLI

The CLI wraps the DBus API.

Binary: `build/nips/nip55l/nostr-signer-cli`

```
Usage: nostr-signer-cli <cmd> [args]

Commands:
  get-pubkey
  store-key <key> [identity]
  clear-key [identity]
  sign <json> [current_user] [requester]
  nip04-encrypt <plaintext> <peer_hex> [current_user]
  nip04-decrypt <cipher_b64> <peer_hex> [current_user]
  nip44-encrypt <plaintext> <peer_hex> [current_user]
  nip44-decrypt <cipher_b64> <peer_hex> [current_user]
```

Examples:

```
# Get public key
nostr-signer-cli get-pubkey

# Store a key (requires Secret Service and env gate)
export NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1
nostr-signer-cli store-key 'nsec1...' default

# Sign an event
nostr-signer-cli sign '{"kind":1,"tags":[],"content":"hi"}'

# NIP-04 roundtrip
C=$(nostr-signer-cli nip04-encrypt 'hello' <peer_hex>)
nostr-signer-cli nip04-decrypt "$C" <peer_hex>

# NIP-44 roundtrip
C=$(nostr-signer-cli nip44-encrypt 'hello' <peer_hex>)
nostr-signer-cli nip44-decrypt "$C" <peer_hex>
```

## Security notes

- Secrets never leave the local machine. No network calls are made by the signer.
- Key mutations over DBus are opt-in via `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1` and rate-limited.
- Reading, signing, encrypting and decrypting are approval-gated per verified
  caller (see *Access control*). Against unsandboxed same-user processes this
  is a guard, not a boundary (they can edit the grants file too).
- On macOS, libsecret typically has no Secret Service provider by default; store/clear may return NOT_FOUND. Linux desktops with keyrings (e.g., GNOME Keyring) provide this service.

## Roadmap

- DBus error code mapping refinement and richer error domains
- Expand integration tests for DBus + CLI
- Structured fuzz harness for `StoreKey` / `SignEvent` / `GetRelays` / NIP-44
  base64 inputs (deferred from D1.a; tracked separately)
- Real-service D-Bus contract test on a private `GTestDBus` bus (deferred; the
  in-tree consumer coverage lives in `apps/grotto/tests/test-dbus.c`
  and `gnome/nostr-homed/tests/integration/test_mock_signer_contract.c`)
