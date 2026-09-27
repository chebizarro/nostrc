# GNostr

GTK 4 / libadwaita Nostr client. Build from the repository root with
`-DBUILD_APPS=ON` (see the top-level README and `CMakePresets.json`).

## Keys and signing

GNostr never holds your private key (nostrc-e5nz). Every signature and every
NIP-44/NIP-04 encryption goes through a signer:

- **GNostr Signer on this computer**: the `org.nostr.Signer` D-Bus service
  (the nip55l daemon, `nips/nip55l`). It keeps keys in the Secret Service
  keyring under `org.gnostr.Signer/identity` (see
  `gnome/seahorse/org.gnostr.secret.schema.txt`), or in the macOS Keychain.
- **A remote signer** over NIP-46 (`bunker://` / `nostrconnect://`).

Adding an account means creating or importing a key **in GNostr Signer**, or
pairing a remote signer. GNostr has no "import nsec" and no key backup or
export; those live in GNostr Signer. Removing an account in GNostr only
forgets it on this device; the key stays in the signer.

GNostr still reads identity *metadata* (npub and label, never secrets) from
the key store, see `src/util/keystore.h`.

### Which identity GNostr Signer uses

GNostr always asks GNostr Signer for the account it is signed in as: every
`SignEvent` and NIP-44 call passes that account's npub as `current_user`
(`docs/dbus-interface.md`), never `""` (the signer's default identity).
A signed event whose `pubkey` is not that account is rejected (nostrc-vuwu).

On startup a GNostr Signer session resumes only when the signer is already
running and its `GetPublicKey()` (its active identity) is the saved account.
GNostr never starts the signer by itself at startup; once it appears on the
bus, for example through **Start GNostr Signer**, the session resumes. If
the signer's active identity is a different account, GNostr stays signed
out and asks you to sign in again rather than continue as another pubkey.

### When no signer is available

If a session signs through `org.nostr.Signer` and the service is not on the
session bus, GNostr is read-only. A banner at the top of the window says so,
and the sign-in page explains which case applies:

- installed but not running: **Start GNostr Signer** D-Bus-activates it
  (`org.nostr.Signer.service`);
- not installed: install GNostr Signer, or sign in with a remote signer.

GNostr does not fall back to a local key. NIP-46 sessions do not depend on
`org.nostr.Signer` and are not affected.

## Migrating from GNostr releases that stored keys

Older GNostr releases kept nsec in the client's own keystore:
libsecret schema `org.gnostr.NostrKey` (`npub`, `application=org.gnostr.Client`)
on Linux, and Keychain service `org.gnostr.Client` on macOS.

- **Linux**: nothing to do by hand. When the signer daemon starts, it imports
  every `org.gnostr.NostrKey` item written by GNostr into
  `org.gnostr.Signer/identity` (label "gnostr import"), then deletes the
  client copy. A key the signer already holds keeps its existing label. The
  pass runs once per keyring and records a marker item
  (`org.gnostr.Signer/migration`, `name=legacy-keys-v2`). If the keyring is
  locked, or the unlock prompt is dismissed, the pass retries on the next
  start.
- **macOS**: the daemon does not import Keychain items yet. Import the key in
  GNostr Signer, then delete the old `org.gnostr.Client` items in Keychain
  Access.

Until the keys are imported, GNostr shows how many old keys are waiting
(sign-in page and banner) but never reads them. GNostr no longer deletes
keys when an account is removed, so an unmigrated key is never lost that
way.
