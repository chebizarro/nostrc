# Groundhog

Groundhog is a privacy-first Nostr messenger for GNOME, built with
GTK 4 and libadwaita. It supports NIP-17 private messaging, NIP-29
relay groups and Marmot (MLS) encrypted groups.

## Accounts and signers

An account can use the local Grotto signer (NIP-55L), or a NIP-46 remote
signer such as Amber on a phone or a bunker. To pair a remote signer, scan
Groundhog's `nostrconnect://` QR code with the signer or paste its `bunker://`
link. Grotto is needed only for Grotto-backed accounts; remote-only users do
not need to install it. Groundhog stores the remote connection credential in
Secret Service or Keychain, never in GSettings or the message database.

Remote signing and decrypting contact the chosen signer relays. Those relays
can see connection timing, your IP address unless Tor is enabled, and the
link between the client and signer transport keys. They do not receive your
message plaintext. Groundhog requires a working encrypted SQLCipher store
before a remote account can sign or decrypt; memory-only mode is unavailable
for remote accounts. Previously cached conversations remain readable while
the phone or bunker is offline. New messages and outgoing actions wait for
the signer and may need a retry when it returns.

Before downgrading to an alpha without remote-signer support, select a
Grotto-backed account or read-only mode. Older versions cannot use a
remote-only selection.

## Running multiple instances

A named instance runs Groundhog with fully isolated data, so two
instances on the same machine behave as two separate devices from the
protocol's point of view—each selects a signer identity and has its
own encrypted store, relay lists and MLS state. Both instances may select
the same remote account and use independent NIP-46 sessions. They may share
one Grotto signer and Secret Service under the same user account; this is not
a security boundary against another process running as that user.

### Command line

```sh
groundhog --instance dev2
```

### Environment variable

```sh
GROUNDHOG_INSTANCE=dev2 groundhog
```

### What an instance changes

| Aspect | Default instance | Named instance (`NAME`) |
|---|---|---|
| App id | `org.nostr.Groundhog` | `org.nostr.Groundhog.NAME` |
| XDG dirs | `$XDG_*_HOME` | `$XDG_*_HOME/.groundhog-instances/NAME` |
| GSettings | system backend (dconf) | keyfile backend |
| Encrypted store | `$XDG_DATA_HOME/groundhog/accounts/…` | under the instance's `XDG_DATA_HOME` |
| D-Bus name | `org.nostr.Groundhog` | `org.nostr.Groundhog.NAME` |
| Background autostart | Supported | Disabled: a desktop autostart entry cannot safely restart a named profile |
| Signer | Per-account Grotto or remote NIP-46 signer | The same signer choices; Grotto serves all Grotto-backed instances, while remote accounts use independent sessions. Grotto has no `--instance`. |

Instance names are 1–32 characters, ASCII alphanumeric or underscore,
starting with a letter. Invalid or missing names fail closed; Groundhog
never silently opens the default profile instead. Named instances force the
keyfile GSettings backend even when `GSETTINGS_BACKEND=dconf` is inherited.

### Session bus resilience

If the session bus socket accepts connections but never answers the
D-Bus AUTH handshake (common on macOS where launchd holds the socket),
Groundhog performs a complete D-Bus connection and AUTH exchange with a
5-second deadline before `gtk_init` or `g_application_run`. Only a stalled
connection enters non-unique fallback mode, with the banner: "Grotto
and notifications are unavailable: the session bus isn't responding".

## Building

```sh
cmake -S . -B build -G Ninja -DBUILD_GROUNDHOG=ON
ninja -C build
```

See the top-level README for dependencies.
