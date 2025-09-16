# Nostr GOA Overlay

This subproject provides a first-class "Nostr" provider for GNOME Online Accounts (GOA), installed entirely under `~/.local`. The system GOA provided by your distro remains unchanged and dynamically discovers the provider backend from the user prefix.

Features

- Nostr provider for GOA (Calendar, Contacts, Files; Mail optional/off by default)
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

Install overlay to user scope (system GOA)

- `gnome/nostr-goa-overlay/overlay/install-overlay.sh`
  - Builds and installs the provider/backend and assets into `~/.local`
  - Restarts the user `goa-daemon` to reload providers

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
