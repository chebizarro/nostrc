# Grotto (Preview 0.1.0)

Native Linux is the primary distribution (GTK ≥4.14, libadwaita ≥1.5, GLib ≥2.80).
The single `grotto` package contains GUI and daemon and recommends the optional
GNOME Shell extension. RPM/Arch/Nix recipes require installed-platform validation;
Flatpak and AppImage signer bundles are **experimental, not release-supported**.
See [packaging](packaging/README.md), the [manual acceptance matrix](../../docs/testing/grotto-manual-acceptance.md)
and [release readiness](../../docs/reviews/grotto-release-readiness-2026-10-14.md).
Preview removal requires a separate owner decision after all promotion gates.

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

Background review defaults on **after setup disclosure or an explicit Preferences
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

Preferences reports desktop background permission: unavailable portals are nonfatal
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
Experimental AppImage login is unsupported and visibly disabled in Preferences.
Turning background off turns login launch off;
turning login launch off does not turn background off. Migration v1 reads the old
`autostart`/`start-minimized` preferences once; explicit launches and reviews are
always visible, never minimized by those old settings.

Open **Preferences** from the primary menu with **Ctrl+,**. The `--preferences`
switch opens the same page; `--settings` remains a hidden compatibility alias for
the unreleased 0.1.0 wave.

Desktop actions `grotto --new-identity` and `grotto --preferences` reuse the existing
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
- **Preferences**: background/login, appearance/accessibility, management lock,
  screen-lock pause, ask-by-default event types, remote runtime/configuration,
  1–30 day activity retention, and About. The owner-approved ask-by-default list
  never overrides a saved grant; Medium's public-publishing set is unchanged.

A client opens its label, verified principal, Full/Medium/Low trust, effective
read-only policy rows, explicit editable permissions with absolute expiry, recent
activity and Revoke. Lowering trust preserves explicit grants. Clearing remembered
permissions is explicitly identity-scoped, as is `SetPermissions`; other identities
are preserved. Editors use an owner/epoch/revision-bracketed permission snapshot,
submit once with `expected_revision`, and surface conflicts instead of overwriting.
Identity avatars open G09's pinned identity/backup page. “Default for older apps”
is a separate daemon action, never changed by management selection.

### Permission choices

**Allow once** is the default. **Always allow this app to <operation/kind> as <identity>**
saves that exact scope until revoked; **24 hours** is the temporary alternative.
There is no always-confirm class, TTL dropdown or Forever confirmation. **Deny once**
always denies just this request. **Block this operation for this app** saves a scoped
deny; **Block app…** requires confirmation and denies the whole app until unblocked
on its Permissions page. Initial remote connection offers **Pair application**, not
a remembered connect grant. Paired reconnect uses separate pairing admission.

**Trust this app for routine operations** is one approver-only Control1 decision
that saves the versioned Full preset and completes this request. Routine access
includes reads, publishing and messaging; ask-by-default kinds (AUTH, DM/seal/wrap,
profile/contacts/relay-list, deletion and zap requests) need their first scoped grant.
Unknown/custom kinds need an explicit grant. Old Full clients show **Previous access
policy** and keep their old ceiling until one explicit upgrade. Lowering access
never removes saved permissions. Exact npub grants do not move to a new default identity.

AUTH permissions name one canonical relay URL, not a challenge. Every requested
AUTH event must have one relay, one nonempty challenge, empty content and a timestamp
within ten minutes. This validates the app's claim, not its transport provenance.
Named claims on macOS are unverifiable; the shared empty `claimed:` principal cannot
receive reusable consent. No conversation-key export method exists; use NIP-44 decrypt.

Dormant old grants migrate before evaluation to inactive **policy-review** rows,
not the legacy 90-day review state. Their provenance and original absolute expiry,
including zero indefinitely, are retained. Review offers activation, allow once or
discard. AUTH activation names the current relay. The Permissions page shows the
daemon's active/inactive/expired state and reason, and owns duration editing.
Flow encryption/seal choices are fully ephemeral, with the same bounded first-arrival
window; neither writes disk rows. Grant/preset changes re-evaluate waiting requests.

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
- **Native Linux first**: macOS build/test support is not installed-desktop release acceptance
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
- CMake 3.22+
- pkg-config
- Compiler with C11 support

### Build Instructions

```bash
# From the monorepo root, with submodules initialized:
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_GNOSTR_APP=OFF -DBUILD_TESTING=OFF -DBUILD_SIGNER_TESTS=OFF \
  -DGROTTO_WITH_PKCS11=OFF -DGROTTO_WITH_HW_WALLET=OFF
cmake --build build --target apps/grotto/all
```

### Installation

```bash
sudo cmake --install build/apps/grotto
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

The window menu's **Connect an Application…** opens paste, native camera/image scan, explicit identity selection, shared pairing review, and daemon-issued single-use QR offers. Relay settings apply without a daemon restart. See [pairing, headless configuration, and the systemd operator check](docs/pairing.md). Linux GUI builds require libqrencode, zbar, and GStreamer (core/video/appsink plus the native capture/base/good runtime plugins).

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
- Requests use the shared daemon gate: explicit denies, scoped permissions,
  bounded ephemeral consent and versioned access presets determine whether review is needed.
- Grotto is not a security boundary against same-user malware. PID-attested Linux
  checks installed executable paths/inodes; UID-only buses have weaker inherited
  caller/approver guarantees (see the D-Bus interface limitations).
- Signing is not publication; client apps decide whether to publish returned events.
- Communication between components is secured using D-Bus authentication (Linux) or Unix sockets (macOS)
- macOS builds use hardened runtime with appropriate entitlements for Keychain access

## Documentation

- **[README.md](README.md)** - This file, overview and basic usage
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - System architecture and design
- **[DEVELOPMENT.md](DEVELOPMENT.md)** - Developer guide and coding standards
- **[DAEMON_QUICKSTART.md](DAEMON_QUICKSTART.md)** - Quick start guide for the daemon
- **[DAEMON_DEPLOYMENT.md](DAEMON_DEPLOYMENT.md)** - Comprehensive deployment guide
- **[Release readiness](../../docs/reviews/grotto-release-readiness-2026-10-14.md)** - Current evidence, limitations and promotion gates
- **[packaging/macos/README.md](packaging/macos/README.md)** - macOS packaging and distribution guide

## Daemon and upgrades

The daemon is D-Bus activated, one per user session. Its native unit preserves
host `/proc` caller attestation and blocks remote network access by default.
Use the explicit bunker drop-in for remote signing and the headless example for
`--bunker --no-approval-ui`; pre-paired scoped operations can run without the GUI,
new consent fails closed. Closing/locking the GUI is not cryptographic pause.

Migration removes unsafe broad legacy event allows from executable authority and
shows them as **Needs review**. Non-event grants and broad denials preserve their
scope; legacy allowlists never become signing grants. See
[deployment and rollback](DAEMON_DEPLOYMENT.md#upgrade-migration-and-rollback)
for versioned files, backup and the unsafe-old-binary rollback limit.
No installed GNOME, physical-camera, Orca or manual release-matrix step is claimed
by the local automated suites.

## Contributing

Contributions are welcome! Please read our [DEVELOPMENT.md](DEVELOPMENT.md) for details on our code of conduct and the process for submitting pull requests.

## License

This project is licensed under the MIT License - see the [LICENSE](../../LICENSE) file for details.
