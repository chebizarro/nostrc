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
