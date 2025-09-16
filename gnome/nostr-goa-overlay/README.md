# Nostr GOA Overlay

This subproject provides a user-scoped GNOME Online Accounts (GOA) overlay that ships a first-class "Nostr" provider and a per-user goa-daemon overlay. It installs entirely under `~/.local` and does not modify the base OS.

Features

- Nostr provider for GOA (Calendar, Contacts, Files; Mail optional/off by default)
- User-scoped `goa-daemon` via D-Bus activation in `~/.local/share/dbus-1/services/`
- Provisioning helper to create EDS CalDAV/CardDAV sources pointing to a local DAV bridge
- Starts/stops user services: `nostr-router`, `nostr-dav`, `nostrfs`, `nostr-notify`
- Registers a `nostr:` URI handler under `~/.local`

Build quickstart

- CMake (wrapping Meson):
  - `cmake -S . -B build -DENABLE_NOSTR_GOA_OVERLAY=ON`
  - `cmake --build build -j`
- Meson (inside overlay dir):
  - `meson setup _build --prefix=$HOME/.local`
  - `meson compile -C _build`

Install overlay to user scope

- `gnome/nostr-goa-overlay/overlay/install-overlay.sh`
  - Builds vendor GOA with the Nostr provider and installs into `~/.local`
  - Generates `~/.local/share/dbus-1/services/org.gnome.OnlineAccounts.service`
  - Restarts per-user `goa-daemon`

Uninstall

- `gnome/nostr-goa-overlay/overlay/uninstall-overlay.sh`

Docs

- `docs/OVERLAY.md` – overlay design and how to revert
- `docs/PROVIDER.md` – provider behavior, flows, services
- `docs/INSTALL.md` – installing/uninstalling the overlay
- `docs/SECURITY.md` – threat model & sandboxing
- `docs/TESTING.md` – headless test rig and CI hints

Notes

- The signer DBus name defaults to `org.nostr.Signer`. If the repo provides a different name/path, the provider and helper adapt accordingly.
- DAV bridge default: `http://127.0.0.1:7680` with `/cal/<user>` and `/card/<user>`.
