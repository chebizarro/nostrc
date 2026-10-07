# Groundhog

Groundhog is a privacy-first Nostr messenger for GNOME, built with
GTK 4 and libadwaita. It supports NIP-17 private messaging, NIP-29
relay groups and Marmot (MLS) encrypted groups.

## Running multiple instances

A named instance runs Groundhog with fully isolated data, so two
instances on the same machine behave as two separate devices from the
protocol's point of view—each selects a distinct signer identity and has its
own encrypted store, relay lists and MLS state. They may share one signer
and Secret Service under the same user account; this is not a security
boundary against another process running as that user.

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
| Signer | Grotto (`org.nostr.Signer`) | the same Grotto: one signer serves every instance and every NIP-55L/NIP-46 app; each instance signs with the identity it selected. Grotto has no `--instance`. |

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
