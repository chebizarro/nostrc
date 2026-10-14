# Grotto

Grotto keeps your Nostr identities and signs, encrypts and decrypts for the
apps you allow. Client apps never receive the private key; explicit identity
creation/import/backup in Grotto may transiently handle key material.

## One signer for all your Nostr apps

Grotto is the signer for **every** Nostr app that speaks one of:

- **NIP-55L** — local apps on this computer, over the session bus
  (`org.nostr.Signer`, see `docs/dbus-interface.md`). Groundhog, gnostr and
  the browser bridge can use this when configured for Grotto.
- **NIP-46 (Nostr Connect)** — remote apps over relays, with Grotto as the
  bunker (`grotto-daemon --bunker`; shared-gate signing with a distinct persistent
  transport key). See [daemon deployment](DAEMON_DEPLOYMENT.md#remote-signing-and-headless-deployment-g05)
  for custody, scoped/headless operation and the explicit network drop-in.
  Pair from **Connect** using paste, camera/file QR, or an identity-bound
  single-use pairing link. [Pairing and live relay configuration](docs/pairing.md)
  require explicit remote-signing enablement.

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
and local `grotto:` activation, never `nostr:`. Pairing previews and the identity
chooser precede supplied-relay contact. Another signer's `bunker://` link does not
delegate custody: Grotto explains that unsupported use without contacting it. The opportunistic
StatusNotifier uses `org.nostr.Grotto-symbolic`; it is Passive while the Shell signer
module owns `org.nostr.Shell.Signer1`, preventing duplicate indicators.

## Management pages

- **Home**: management-only identity selection, public-key copy/QR, pending reviews,
  Signed today / Clients / Pending and connected clients. Signed today counts
  successful daemon `sign_event` operations since local midnight, not publications;
  clearing activity resets it. It reflects retained activity, not a lifetime ledger.
- **Activity**: daemon metadata with event/client/outcome filters and explicit older
  pages; no historical event JSON or message content is kept.
- **Connect**: paste, camera/file QR, link entry and single-use pairing offers.
  Local apps need no pairing. Approved remote connections waiting for relay OK
  remain visibly incomplete, not paired.
- **Settings**: background/login, appearance/accessibility, management lock,
  screen-lock pause, required-confirmation event types, remote runtime/configuration,
  1–30 day activity retention, and About. Required-confirmation defaults and Medium's
  public-publishing set come from G03 policy data; final membership awaits owner sign-off.

A client opens its label, verified principal, Full/Medium/Low trust, effective
read-only policy rows, explicit editable permissions with absolute expiry, recent
activity and Revoke. Lowering trust preserves explicit grants. Clearing remembered
permissions is explicitly identity-scoped, as is `SetPermissions`; other identities
are preserved. Editors use an owner/epoch/revision-bracketed permission snapshot,
submit once with `expected_revision`, and surface conflicts instead of overwriting.
Identity avatars open G09's pinned identity/backup page. “Default for older apps”
is a separate daemon action, never changed by management selection.

Blueprint is authoritative. Use **blueprint-compiler 0.22.2** (official GNOME
release), GTK ≥4.14, libadwaita ≥1.5 and GLib ≥2.80. Meson uses prebuilt nostrc libraries; select another CMake build with
`meson setup <build> apps/grotto -Dcmake_build_dir=/absolute/path/to/cmake-build`.
On macOS, add `-Ddependency_prefixes="$(brew --prefix)"` for installed nsync/flatcc libraries without pkg-config metadata.
Both builds enforce the API
ceiling; production source/resource inventories exclude the retired pages and
unproven hardware/multisig/delegation/social features. `signer/blueprint-drift`
checks committed `.ui` and the generated kind display catalogue. The native Linux
`signer/ui-layout` renders real templates and saves light/dark 360px screenshots
to `<build>/screenshots/`; `signer/home-model` drives the real model over a private
bus. These are local source/layout proofs, not the G12 installed GNOME/Orca matrix.

## Features

- **Secure Identity Management**: Securely store and manage Nostr identities
- **Permission Control**: Scoped control over which applications may sign, encrypt or decrypt
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

- GTK 4.14+
- libadwaita 1.5+
- GLib 2.80+
- blueprint-compiler 0.22.2
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

## Remote application pairing

The window menu's **Connect an application…** opens paste, native camera/image scan, explicit identity selection, shared pairing review, and daemon-issued single-use QR offers. Relay settings apply without a daemon restart. See [pairing, headless configuration, and the systemd operator check](docs/pairing.md). Linux GUI builds require libqrencode, zbar, and GStreamer (core/video/appsink plus the native capture/base/good runtime plugins).

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
