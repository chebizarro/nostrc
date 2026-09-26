# NIP-55L Linux Signer

**Component version: 0.2.0** (tracked in `/VERSION_MANIFEST.md`; authoritative
source `NOSTR_NIP55L_VERSION_*` in `include/nostr/nip55l/signer_ops.h`).
0.2.0 is a breaking change for D-Bus clients: `SignEvent` now returns the
complete signed event JSON instead of the bare signature — see below.

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

`org.nostr.Signer.service` is owned by **gnostr-signer**, which activates
`gnostr-signer-daemon` (the same `nips/nip55l` GLib service plus the GUI's
approval flow). This tree does **not** install an activation file by default,
so the two packages never ship conflicting copies of the same path.

Headless installs that want `nostr-signer-daemon` auto-activated without
gnostr-signer build with:

```
cmake -S . -B build -DENABLE_NIP55L_STANDALONE_ACTIVATION=ON
```

which configures `dbus/org.nostr.Signer.service.in` (absolute `Exec=` path)
and installs it to `share/dbus-1/services/`. Do not enable it alongside a
gnostr-signer package.

## Key resolution order

The daemon resolves the signing key in this order:

1. `current_user` parameter (64-hex or `nsec1...`)
2. Env `NOSTR_SIGNER_SECKEY_HEX` (64-hex)
3. Env `NOSTR_SIGNER_NSEC` (`nsec1...`)
4. Secret Service (libsecret), attribute `account="default"` (or specified)

If none are found: returns NOT_FOUND.

## DBus API

Interface: `org.nostr.Signer`

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

- `GetRelays() -> (s relaysJson)`
  - JSON array of the user's explicitly configured relays. Sources, first
    match wins: `$XDG_CONFIG_HOME/nostr/relays.conf` (a JSON array of
    `ws://`/`wss://` URL strings), then the relay list set in gnostr-signer
    (GSettings `org.gnostr.Signer` `relays`, user-written value only, not the
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
- On macOS, libsecret typically has no Secret Service provider by default; store/clear may return NOT_FOUND. Linux desktops with keyrings (e.g., GNOME Keyring) provide this service.

## Roadmap

- DBus error code mapping refinement and richer error domains
- Expand integration tests for DBus + CLI
- Structured fuzz harness for `StoreKey` / `SignEvent` / `GetRelays` / NIP-44
  base64 inputs (deferred from D1.a; tracked separately)
- Real-service D-Bus contract test on a private `GTestDBus` bus (deferred; the
  in-tree consumer coverage lives in `apps/gnostr-signer/tests/test-dbus.c`
  and `gnome/nostr-homed/tests/integration/test_mock_signer_contract.c`)
