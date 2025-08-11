# NIP-55L Linux Signer

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
# Optional: allow secret mutations over DBus
export NOSTR_SIGNER_ALLOW_SECRET_MUTATIONS=1

# Optional: provide a secret for key resolution (hex or nsec)
export NOSTR_SIGNER_SECKEY_HEX=...   # or
export NOSTR_SIGNER_NSEC=nsec1...

build/nips/nip55l/nostr-signer-daemon
```

Service file (`dbus/com.nostr.Signer.service.in`, installed as `org.nostr.Signer.service`) installs to `share/dbus-1/services/` to enable auto-activation on Linux.

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

- `SignEvent(in s eventJson, in s currentUser, in s requester) -> (s signedJson)`
  - Signs a Nostr event JSON. `currentUser` may be empty to use resolution order above.

- `NIP04Encrypt(in s plaintext, in s peerPubHex, in s currentUser) -> (s cipherB64)`
- `NIP04Decrypt(in s cipherB64, in s peerPubHex, in s currentUser) -> (s plaintext)`

- `NIP44Encrypt(in s plaintext, in s peerPubHex, in s currentUser) -> (s cipherB64)`
- `NIP44Decrypt(in s cipherB64, in s peerPubHex, in s currentUser) -> (s plaintext)`

- `GetRelays() -> (s relaysJson)`
  - Stub; currently returns NOT_FOUND.

- `StoreSecret(in s secret, in s account) -> (b ok)`
  - Stores a secret (64-hex or `nsec1...`) in Secret Service under schema `org.nostr.Signer`, attr `account`. Optional feature; returns error if libsecret is unavailable.
  - ACL: requires env `NOSTR_SIGNER_ALLOW_SECRET_MUTATIONS=1` and subject to a per-sender 500ms rate limit.

- `ClearSecret(in s account) -> (b ok)`
  - Clears stored secret for `account`. Same ACL/rate limit as above.

Errors are returned as GLib `G_IO_ERROR_*` over DBus. Core error codes map to generic DBus failure for now.

## CLI

The CLI wraps the DBus API.

Binary: `build/nips/nip55l/nostr-signer-cli`

```
Usage: nostr-signer-cli <cmd> [args]

Commands:
  get-pubkey
  store-secret <secret> [account]
  clear-secret [account]
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
export NOSTR_SIGNER_ALLOW_SECRET_MUTATIONS=1
nostr-signer-cli store-secret 'nsec1...' default

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
- Secret mutations over DBus are opt-in via `NOSTR_SIGNER_ALLOW_SECRET_MUTATIONS=1` and rate-limited.
- On macOS, libsecret typically has no Secret Service provider by default; store/clear may return NOT_FOUND. Linux desktops with keyrings (e.g., GNOME Keyring) provide this service.

## Roadmap

- Implement `GetRelays()` integration with profile/store
- DBus error code mapping refinement and richer error domains
- Expand integration tests for DBus + CLI
