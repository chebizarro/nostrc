# Groundhog

Groundhog is a privacy-first Nostr messenger for GNOME, built with
GTK 4 and libadwaita. It supports NIP-17 private messaging, NIP-29
relay groups and Marmot (MLS) encrypted groups.

## Running multiple instances

A named instance runs Groundhog with fully isolated data, so two
instances on the same machine behave as two separate devices from the
protocol's point of view—each has its own keys, encrypted store, relay
lists and MLS state.

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

Instance names are 1–32 characters, ASCII alphanumeric or underscore,
starting with a letter. Invalid names are ignored (the default instance
starts instead).

### Session bus resilience

If the session bus socket accepts connections but never answers the
D-Bus AUTH handshake (common on macOS where launchd holds the socket),
Groundhog probes the bus with a 5-second timeout before `gtk_init` or
`g_application_run`. When the probe fails, Groundhog starts in
offline mode with a banner: "Nostr Signer and notifications are
unavailable."

## Building

```sh
cmake -S . -B build -G Ninja -DBUILD_GROUNDHOG=ON
ninja -C build
```

See the top-level README for dependencies.
