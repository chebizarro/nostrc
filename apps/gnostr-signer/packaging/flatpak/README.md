# Flatpak Packaging for GNostr Signer

This directory contains the Flatpak manifest and supporting files for building and distributing GNostr Signer as a Flatpak application.

## Overview

GNostr Signer is packaged as `org.gnostr.Signer` and uses the GNOME Platform runtime for GTK4 and libadwaita support.

## Prerequisites

### Install Flatpak and flatpak-builder

On Fedora/RHEL:
```bash
sudo dnf install flatpak flatpak-builder
```

On Debian/Ubuntu:
```bash
sudo apt install flatpak flatpak-builder
```

On Arch Linux:
```bash
sudo pacman -S flatpak flatpak-builder
```

### Install GNOME Runtime and SDK

```bash
flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install flathub org.gnome.Platform//47 org.gnome.Sdk//47
```

## Building

### Quick Build and Install (Development)

From the `packaging/flatpak/` directory:

```bash
# Build and install for the current user
flatpak-builder --user --install --force-clean build-dir org.gnostr.Signer.yml
```

### Build Only (No Install)

```bash
flatpak-builder --force-clean build-dir org.gnostr.Signer.yml
```

### Build with Verbose Output

```bash
flatpak-builder --verbose --force-clean build-dir org.gnostr.Signer.yml
```

### Build a Repository for Distribution

```bash
# Build and export to a local repo
flatpak-builder --repo=repo --force-clean build-dir org.gnostr.Signer.yml

# Create a single-file bundle
flatpak build-bundle repo org.gnostr.Signer.flatpak org.gnostr.Signer
```

## Running

After installation, run the application:

```bash
flatpak run org.gnostr.Signer
```

Or launch from your desktop environment's application menu.

## Sandbox Permissions

The Flatpak is configured with the following sandbox permissions:

| Permission | Purpose |
|------------|---------|
| `--share=ipc` | D-Bus IPC for desktop integration |
| `--socket=wayland` | Wayland display access |
| `--socket=fallback-x11` | X11 fallback for non-Wayland systems |
| `--talk-name=org.freedesktop.secrets` | GNOME Keyring/Secret Service for secure key storage |
| `--own-name=org.nostr.Signer` | Own the D-Bus name for NIP-55L interface |
| `--share=network` | Network access for Nostr relay connections |
| `--talk-name=org.freedesktop.Notifications` | Desktop notifications for signing requests |
| `--talk-name=org.freedesktop.portal.Desktop` | Portal access for system integration |
| `--talk-name=org.freedesktop.portal.FileChooser` | File chooser portal for backup/restore |
| `--talk-name=org.freedesktop.portal.Background` | Background portal for daemon auto-start |

## Dependencies Built from Source

The manifest builds these dependencies that are not included in the GNOME runtime:

| Library | Version | Purpose |
|---------|---------|---------|
| libsecp256k1 | 0.5.1 | Elliptic curve crypto for Nostr (Schnorr signatures) |
| libsodium | 1.0.20 | Modern cryptography (NIP-44, secure memory) |
| nsync | 1.29.2 | Google's C synchronization library |
| jansson | 2.14 | JSON parsing library |
| flatcc | 0.6.1 | FlatBuffers compiler and runtime |
| libwebsockets | 4.3.3 | WebSocket library for relay connections |

## Files

| File | Description |
|------|-------------|
| `org.gnostr.Signer.yml` | Main Flatpak manifest (YAML format) |
| `org.nostr.Signer.service` | D-Bus service file for the daemon |

## Troubleshooting

### Build Fails to Find Dependencies

Ensure the GNOME SDK is installed:
```bash
flatpak list | grep org.gnome.Sdk
```

If not listed, install it:
```bash
flatpak install flathub org.gnome.Sdk//47
```

### Application Won't Start

Check the logs:
```bash
flatpak run --verbose org.gnostr.Signer
```

### D-Bus Service Issues

Verify the service is properly installed:
```bash
flatpak run --command=ls org.gnostr.Signer /app/share/dbus-1/services/
```

### Keyring Access Issues

Ensure GNOME Keyring is running:
```bash
systemctl --user status gnome-keyring-daemon
```

## Development

### Testing Changes

For rapid iteration during development:

```bash
# Build incrementally (reuses cached builds)
flatpak-builder --user --install build-dir org.gnostr.Signer.yml

# Run with debugging
flatpak run --env=G_MESSAGES_DEBUG=all org.gnostr.Signer
```

### Updating Dependencies

To update a dependency version:

1. Update the `tag` field in the manifest
2. Optionally update the `commit` field for reproducible builds
3. Rebuild with `--force-clean` to fetch the new version

### Validating the Manifest

Use `flatpak-builder` to validate the manifest syntax:

```bash
flatpak-builder --show-manifest org.gnostr.Signer.yml
```

## Publishing to Flathub

To publish on Flathub:

1. Fork the [Flathub repository](https://github.com/flathub/flathub)
2. Create a new repository for `org.gnostr.Signer`
3. Submit the manifest with proper source URLs (replace `type: dir` with actual git repository)
4. Follow Flathub's submission guidelines

## License

The Flatpak packaging is provided under the same MIT license as GNostr Signer.
