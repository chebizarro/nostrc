# GOA Overlay Design

This overlay ships a user-scoped `goa-daemon` and a Nostr GOA provider entirely under `~/.local` using D-Bus activation. The system remains unchanged.

## Components

- Vendor GOA (46.x ± minor) built under `vendor/gnome-online-accounts/` with a Nostr provider patch.
- User D-Bus activation file: `~/.local/share/dbus-1/services/org.gnome.OnlineAccounts.service`.
- Provider shared object installed under `~/.local/lib/goa-1.0/backends/`.
- Icons and UI assets under `~/.local/share/icons/hicolor/...`.

## Runtime overlay

- The service file points Exec to `~/%h/.local/libexec/goa-daemon` (vendor build artifact).
- When `org.gnome.OnlineAccounts` is requested, the user overlay daemon is started from `~/.local`.

## Reverting overlay

- Remove the user service file and icons, then restart `goa-daemon`.
- Provided by `overlay/uninstall-overlay.sh`.
