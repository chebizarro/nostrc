# Grotto

Grotto keeps your Nostr identities and signs, encrypts and decrypts for the
apps you allow. Private keys never leave it.

## One signer for all your Nostr apps

Grotto is the signer for **every** Nostr app that speaks one of:

- **NIP-55L** — local apps on this computer, over the session bus
  (`org.nostr.Signer`, see `docs/dbus-interface.md`). Groundhog, gnostr and
  the browser bridge can use this when configured for Grotto.
- **NIP-46 (Nostr Connect)** — remote apps over relays, with Grotto as the
  bunker (Settings › Listen for new connections, or `grotto-daemon --bunker`).

One Grotto serves all of them at once. Each app names the identity it uses
and Grotto asks you, per app, what it may do. That includes several
Groundhog instances (`groundhog --instance NAME`) with Grotto-backed accounts:
they share the one Grotto, each signing with its selected identity. Grotto
itself has no `--instance` option: the daemon owns `org.nostr.Signer`, the name
every app speaks, and there is one per session (owner decision, W31 R4).

Groundhog can instead pair a NIP-46 remote signer such as Amber or a bunker,
so Grotto is not required for remote-signer-only accounts. Such accounts
require a working encrypted Groundhog store. When that phone or bunker is
offline, Groundhog can read cached messages, but new decryptions and signing
wait for the signer. The signer relays learn connection timing, IP address
unless Tor is used, and the transport-key link. Before downgrading to an alpha
without remote-signer support, select a Grotto-backed account or read-only mode.

## Features

- **Secure Identity Management**: Securely store and manage Nostr identities
- **Permission Control**: Granular control over which applications can access your keys
- **DBus Integration**: Provides a D-Bus interface for system integration
- **Cross-Platform**: Works on Linux, macOS, and other platforms
- **User Approval Flow**: Interactive approval dialogs for signing requests
- **Backup & Recovery**: Secure backup and recovery options for identities

## Architecture

The application consists of two main components:

1. **GTK-based GUI Application**: Provides the user interface for managing identities and permissions
2. **Background Daemon**: Handles secure key storage and cryptographic operations

## Building from Source

### Dependencies

- GTK 4.0+
- libadwaita
- GLib 2.68+
- CMake 3.16+
- pkg-config
- Compiler with C11 support

### Build Instructions

```bash
# Clone the repository
# Navigate to the project directory
mkdir build && cd build
cmake ..
make
```

### Installation

```bash
sudo make install
```

### macOS Application Bundle

On macOS, you can create a native application bundle and DMG installer:

```bash
# Enable macOS bundle support
cmake .. -DBUILD_MACOS_BUNDLE=ON

# Build the application
make -j$(sysctl -n hw.ncpu)

# Create .app bundle (for testing)
make macos-bundle

# Create DMG installer (for distribution)
make macos-dmg
```

For signed and notarized releases:

```bash
# Set your Developer ID
export DEVELOPER_ID="Developer ID Application: Your Name (TEAM_ID)"

# Create signed DMG
make macos-dmg-signed

# Create signed and notarized DMG (requires Apple credentials)
export APPLE_ID="your-apple-id@example.com"
export APPLE_PASSWORD="app-specific-password"
export APPLE_TEAM_ID="YOUR_TEAM_ID"
make macos-dmg-notarized
```

See [packaging/macos/README.md](packaging/macos/README.md) for detailed macOS packaging documentation.

## Usage

### Running the Application

```bash
grotto
```

### Environment Variables

- `NOSTR_SIGNER_ENDPOINT`: Custom IPC endpoint (e.g., `unix:/run/user/1000/gnostr/signer.sock`)
- `NOSTR_DEBUG`: Enable debug output when set

## Security Considerations

- Private keys are stored encrypted using the system keyring (Linux: libsecret, macOS: Keychain)
- All signing operations require explicit user approval
- Communication between components is secured using D-Bus authentication (Linux) or Unix sockets (macOS)
- macOS builds use hardened runtime with appropriate entitlements for Keychain access

## Documentation

- **[README.md](README.md)** - This file, overview and basic usage
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - System architecture and design
- **[DEVELOPMENT.md](DEVELOPMENT.md)** - Developer guide and coding standards
- **[DAEMON_QUICKSTART.md](DAEMON_QUICKSTART.md)** - Quick start guide for the daemon
- **[DAEMON_DEPLOYMENT.md](DAEMON_DEPLOYMENT.md)** - Comprehensive deployment guide
- **[DAEMON_IMPROVEMENTS.md](DAEMON_IMPROVEMENTS.md)** - Summary of production improvements
- **[packaging/macos/README.md](packaging/macos/README.md)** - macOS packaging and distribution guide

## Daemon

The grotto-daemon is production-ready with:

- ✅ **D-Bus Interface** - System integration for desktop applications
- ✅ **Unix Domain Sockets** - High-performance local IPC
- ✅ **TCP Support** (optional) - Network-based access with authentication
- ✅ **Comprehensive Security** - Multiple layers of protection
- ✅ **Systemd Integration** - Automatic start and monitoring
- ✅ **Graceful Shutdown** - Proper cleanup and resource management
- ✅ **Statistics & Monitoring** - Track connections, requests, and errors
- ✅ **Production Hardening** - Ready for deployment

See [DAEMON_QUICKSTART.md](DAEMON_QUICKSTART.md) for quick start instructions or [DAEMON_DEPLOYMENT.md](DAEMON_DEPLOYMENT.md) for comprehensive deployment information.

## Contributing

Contributions are welcome! Please read our [DEVELOPMENT.md](DEVELOPMENT.md) for details on our code of conduct and the process for submitting pull requests.

## License

This project is licensed under the MIT License - see the [LICENSE](../../LICENSE) file for details.
