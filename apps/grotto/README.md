# Grotto

Grotto keeps your Nostr identities and signs, encrypts and decrypts for the
apps you allow. Private keys never leave it.

## One signer for all your Nostr apps

Grotto is the signer for **every** Nostr app that speaks one of:

- **NIP-55L** — local apps on this computer, over the session bus
  (`org.nostr.Signer`, see `docs/dbus-interface.md`). Groundhog, gnostr and
  the browser bridge can use this when configured for Grotto.
- **NIP-46 (Nostr Connect)** — remote apps over relays, with Grotto as the
  bunker (`grotto-daemon --bunker`; shared-gate signing with a distinct persistent
  transport key). See [daemon deployment](DAEMON_DEPLOYMENT.md#remote-signing-and-headless-deployment-g05)
  for custody, scoped/headless operation and the explicit network drop-in.
  Pairing/QR and live relay configuration UI are the later G06 work, not the
  old log-only “Listen for new connections” control.

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

## Background and desktop activation

Background review defaults on **after setup disclosure or an explicit Settings
choice**. Launch on login defaults off. Closing a window (Ctrl+W) saves its
geometry and destroys it; background/request holds do not retain hidden windows.
Ctrl+Q quits only the approval UI for this session: already-authorised signing
continues in the daemon. Pending requests keep their daemon deadlines, notifications
are withdrawn, and a later request needing consent can start the UI again.
Use **Pause signing** instead to stop signing; this exported action can only pause.
**Resume signing** opens a GUI confirmation, never a silent toggle. The daemon
rejects resume while its observed session is locked, even if the optional lock
policy is off; confirmation also rechecks the GUI's lock state and daemon owner.

Optional **Pause signing on screen lock** is enforced by the daemon, independently
of the UI and background permission. It observes the user's logind session's
`Lock`/`Unlock` signals and `LockedHint` on the system bus (including non-GNOME
desktops); a systemd user service outside a session selects the user's primary
graphical `Display` session, validating its UID. When the policy is enabled but
observation is unavailable, signing stays paused. Unlock requests are not proof
of an unlocked desktop, and unlocking never resumes signing automatically.
The GUI's GNOME ScreenSaver observer is only a redundant pause trigger.

Settings reports desktop background permission: unavailable portals are nonfatal
for native installations; denial removes continuous residence but does not disable
request-scoped D-Bus activation. Portal v2 status contains only generic readiness,
waiting or paused text, updated only when that generic state changes. Only an
explicit successful response with `background=false` means denial; cancellation,
portal errors and missing results mean unavailable, not denied. Native login is
consent-gated and copies the installed login template into
`$XDG_CONFIG_HOME/autostart/org.nostr.Grotto-login.desktop`, invoking
`grotto --gapplication-service` without a window. Portal-only installations request
portal autostart instead with `dbus-activatable=false`, never both. A durable
`portal-autostart-created` marker is set before requesting login and cleared only
on confirmed removal, so disabled login is reconciled even after a restart.
Native login remains disabled while an old portal entry awaits removal.
Experimental AppImage login is unsupported and visibly disabled in Settings.
Turning background off turns login launch off;
turning login launch off does not turn background off. Migration v1 reads the old
`autostart`/`start-minimized` preferences once; explicit launches and reviews are
always visible, never minimized by those old settings.

Desktop actions `grotto --new-identity` and `grotto --settings` reuse the existing
management/onboarding window. The desktop entry handles `bunker:`, `nostrconnect:`
and local `grotto:` activation, never `nostr:`. Until G06's identity-first pairing
entry point lands, pairing links open the review path with an explicit unavailable
message: they do not contact a relay or silently connect. The opportunistic
StatusNotifier uses `org.nostr.Grotto-symbolic`; it is Passive while the Shell signer
module owns `org.nostr.Shell.Signer1`, preventing duplicate indicators.

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
