# Building GNostr Signer

This document provides comprehensive build instructions for GNostr Signer.

## Build Methods

- [Native Build](#native-build) - Build directly on your system
- [Flatpak Build](#flatpak-build) - Build as a sandboxed Flatpak application

---

## Native Build

### Dependencies

Install the following dependencies on your system:

**Fedora/RHEL:**
```bash
sudo dnf install gtk4-devel libadwaita-devel glib2-devel json-glib-devel \
    libsecret-devel libsecp256k1-devel openssl-devel libwebsockets-devel \
    cmake ninja-build gcc pkg-config
```

**Debian/Ubuntu:**
```bash
sudo apt install libgtk-4-dev libadwaita-1-dev libglib2.0-dev libjson-glib-dev \
    libsecret-1-dev libsecp256k1-dev libssl-dev libwebsockets-dev \
    cmake ninja-build gcc pkg-config
```

**Arch Linux:**
```bash
sudo pacman -S gtk4 libadwaita glib2 json-glib libsecret libsecp256k1 \
    openssl libwebsockets cmake ninja gcc pkgconf
```

**macOS (Homebrew):**
```bash
brew install gtk4 libadwaita glib json-glib libsecret libsecp256k1 \
    openssl@3 libwebsockets cmake ninja pkg-config
```

Additionally, you need nsync (Google's synchronization library):
```bash
# Build from source
git clone https://github.com/google/nsync.git
cd nsync
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

### Building

From the repository root:

```bash
# Configure
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --target gnostr-signer gnostr-signer-daemon

# Install (optional)
sudo cmake --install build
```

### Running

```bash
# Run the GUI application
./build/apps/gnostr-signer/gnostr-signer

# Run the daemon (for D-Bus and socket services)
./build/apps/gnostr-signer/gnostr-signer-daemon
```

---

## Flatpak Build

Flatpak provides a sandboxed, portable build that works on any Linux distribution.

### Prerequisites

Install Flatpak and flatpak-builder:

```bash
# Fedora
sudo dnf install flatpak flatpak-builder

# Debian/Ubuntu
sudo apt install flatpak flatpak-builder

# Arch Linux
sudo pacman -S flatpak flatpak-builder
```

Add the Flathub repository and install the GNOME SDK:

```bash
flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install flathub org.gnome.Platform//46 org.gnome.Sdk//46
```

### Building the Flatpak

Navigate to the flatpak directory and build:

```bash
cd apps/gnostr-signer/flatpak

# Build and install for current user
flatpak-builder --user --install --force-clean build-dir org.gnostr.Signer.yaml

# Or build without installing
flatpak-builder --force-clean build-dir org.gnostr.Signer.yaml
```

### Running the Flatpak

```bash
flatpak run org.gnostr.Signer
```

### Building a Distributable Bundle

To create a single-file bundle for distribution:

```bash
# Build the app
flatpak-builder --repo=repo --force-clean build-dir org.gnostr.Signer.yaml

# Create a bundle file
flatpak build-bundle repo gnostr-signer.flatpak org.gnostr.Signer
```

Install the bundle on another system:
```bash
flatpak install gnostr-signer.flatpak
```

### Development Build

For faster iteration during development:

```bash
# Build with debug symbols
flatpak-builder --user --install --force-clean \
    --install-deps-from=flathub \
    --ccache \
    build-dir org.gnostr.Signer.yaml

# Run with environment variables
flatpak run --env=G_MESSAGES_DEBUG=all org.gnostr.Signer
```

### Updating the Manifest

When updating dependencies, use `flatpak-builder --show-manifest` to verify the manifest syntax:

```bash
python3 -c "import yaml; yaml.safe_load(open('org.gnostr.Signer.yaml'))"
```

---

## Flatpak Permissions

The Flatpak is configured with minimal permissions for security:

| Permission | Purpose |
|------------|---------|
| `--share=ipc` | D-Bus IPC access |
| `--socket=fallback-x11` | X11 display (fallback) |
| `--socket=wayland` | Wayland display |
| `--share=network` | Network for relay connections |
| `--talk-name=org.freedesktop.secrets` | GNOME Keyring access |
| `--own-name=org.nostr.Signer` | Own D-Bus service name |
| `--talk-name=org.freedesktop.Notifications` | Desktop notifications |
| `--talk-name=org.freedesktop.portal.Desktop` | File picker portals |

---

## Troubleshooting

### Build Fails with Missing Headers

Ensure all development packages are installed. On some distributions, you may need `-devel` or `-dev` suffixed packages.

### Flatpak Build Fails at secp256k1

The secp256k1 build requires autotools:
```bash
flatpak install flathub org.freedesktop.Sdk.Extension.autotools//23.08
```

### GSettings Schema Not Found

Compile schemas after installation:
```bash
glib-compile-schemas /app/share/glib-2.0/schemas/
# Or system-wide:
sudo glib-compile-schemas /usr/local/share/glib-2.0/schemas/
```

### D-Bus Service Not Activating

Ensure the service file is in the correct location:
```bash
# Flatpak
~/.local/share/flatpak/exports/share/dbus-1/services/

# System
/usr/share/dbus-1/services/
```

---

## Publishing to Flathub

To submit to Flathub:

1. Fork https://github.com/flathub/flathub
2. Add the manifest as `org.gnostr.Signer.yaml`
3. Include any required patches or supplementary files
4. Submit a pull request

See https://docs.flathub.org/docs/for-app-authors/submission for detailed instructions.
