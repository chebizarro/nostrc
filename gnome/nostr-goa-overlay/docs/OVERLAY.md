# GOA Overlay Design

This overlay installs a Nostr GOA provider and related assets entirely under `~/.local`. The system GOA provided by the distro remains unchanged and will dynamically load the provider backend from the user prefix.

## Components

- Provider shared object installed under `~/.local/lib/goa-1.0/backends/`.
- Icons and assets under `~/.local/share/icons/hicolor/...`.
- Optional URI handler desktop entry under `~/.local/share/applications/`.
- Hardened user systemd units installed/enabled by provisioning helper.

## Runtime overlay

- The system `goa-daemon` (from the distro) discovers and loads backends from `~/.local/lib/goa-1.0/backends/`.
- After installation, restarting the user `goa-daemon` (`pkill -u "$USER" -x goa-daemon`) will cause it to reload providers.

## Reverting overlay

- Remove installed files from `~/.local/lib/goa-1.0/backends/` and related icons/desktop entries, then restart `goa-daemon`.
- Provided by `overlay/uninstall-overlay.sh`.
