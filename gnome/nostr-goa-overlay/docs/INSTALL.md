# Install (User-Scoped Overlay)

This overlay installs entirely under `~/.local` and D-Bus-activates a user `goa-daemon`. Do not use `sudo`.

## Quick start

```
gnome/nostr-goa-overlay/overlay/install-overlay.sh
```

## Configuration (matches your local build system)

Create `~/.config/nostr-goa-overlay/build.conf` to control the install without long CLI flags. All settings are optional.

Example template:

```
# Where to install
NOSTR_OVERLAY_PREFIX="$HOME/.local"

# Use an existing local GOA checkout and skip cloning
# e.g., NOSTR_OVERLAY_GOA_SRC="$HOME/src/gnome-online-accounts"
NOSTR_OVERLAY_GOA_SRC=""

# Pin an exact GOA tag, or use a ref (branch/SHA), or a tag pattern
# Only set one of these (tag has priority over pattern if both are set)
NOSTR_OVERLAY_GOA_TAG=""            # e.g., GNOME_46_2
NOSTR_OVERLAY_GOA_REF=""            # e.g., origin/gnome-46 or a commit SHA
NOSTR_OVERLAY_GOA_REF_PATTERN=""    # e.g., GNOME_46*

# Extra Meson switches when building vendor/overlay
NOSTR_OVERLAY_MESON_ARGS='-Dbuildtype=release'

# If your deps live in a custom prefix, seed pkg-config and loader paths
NOSTR_OVERLAY_PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig"
NOSTR_OVERLAY_LD_LIBRARY_PATH="$HOME/.local/lib"
```

CLI flags override config file values:

```
install-overlay.sh \
  --prefix=$HOME/.local \
  --goa-src=$HOME/src/gnome-online-accounts \
  --goa-tag=GNOME_46_2 \
  --goa-ref=origin/gnome-46 \
  --goa-ref-pattern='GNOME_46*' \
  --meson-args='-Dbuildtype=release' \
  --pkg-config-path=$HOME/.local/lib/pkgconfig \
  --ld-library-path=$HOME/.local/lib
```

## Host-aware GOA selection

If you do not set a tag/ref/pattern, the installer derives a sensible default from `/etc/os-release`:

- Ubuntu 24.04 → `GNOME_46*`
- Fedora 40 → `GNOME_46*`
- Arch (rolling) → latest tag

You can override this behavior via config or CLI at any time.

# Install / Uninstall (User-Scoped)

This overlay installs GNOME Online Accounts (GOA) with a "Nostr" provider entirely under `~/.local`. The system remains untouched.

## Install

Prereqs: meson, ninja, pkg-config, glib2, gio-2.0, gtk4 (optional), libsoup-3.0

1) Build overlay

```
meson setup _build --prefix=$HOME/.local
meson compile -C _build
```

2) Install overlay

```
./overlay/install-overlay.sh
```

This will:

- Build vendor GOA with the Nostr provider patch
- Install artifacts to `~/.local` (goa-daemon, provider .so, icons, locale)
- Generate `~/.local/share/dbus-1/services/org.gnome.OnlineAccounts.service`
- Restart the per-user goa-daemon so the overlay is active

3) Verify

```
G_MESSAGES_DEBUG=all gdbus introspect --session \
  --dest org.gnome.OnlineAccounts \
  --object-path /org/gnome/OnlineAccounts || true
```

Open Settings → Online Accounts. You should see a "Nostr" entry.

## Uninstall

```
./overlay/uninstall-overlay.sh
```

This removes the user-scoped service and provider assets and restarts goa-daemon, letting the system GOA take over.
