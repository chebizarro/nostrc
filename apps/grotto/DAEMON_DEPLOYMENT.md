# Grotto daemon deployment (Preview 0.1.0)

Native Linux is the primary distribution. The one `grotto` package includes
`grotto`, `grotto-daemon`, their activation assets and optional network examples.
The GNOME Shell extension is recommended, never a security dependency. This is a
post-login, user-session signer, not a greeter, PAM service or system-bus broker.
Installed desktop acceptance is pending; see the
[manual matrix](../../docs/testing/grotto-manual-acceptance.md).

## Native installation and activation

Use GTK ≥4.14, libadwaita ≥1.5 and GLib ≥2.80. Build from the **monorepo root**, not
`apps/grotto`, with complete submodules:

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_GNOSTR_APP=OFF -DBUILD_TESTING=OFF -DBUILD_SIGNER_TESTS=OFF \
  -DGROTTO_WITH_PKCS11=OFF -DGROTTO_WITH_HW_WALLET=OFF
cmake --build build --target apps/grotto/all
sudo cmake --install build/apps/grotto
systemctl --user daemon-reload
```

Never install a testing build (it carries guarded test seams). Native recipes use
CMake's component install, not the AppImage desktop file or legacy appdata.
A consumer call to session name `org.nostr.Signer` starts the daemon on demand;
GUI approvals activate `org.nostr.Grotto` with `--gapplication-service` (windowless
until explicit review). Do not enable a resident daemon or login GUI merely to
make activation work. Explicitly starting `systemctl --user start
grotto-daemon.service` is optional. Do not run a second daemon manually beside it.

The canonical user unit is under `share/systemd/user`; package recipes may place
it in the distro user-unit directory. The GUI's opt-in launch-on-login copies
`share/grotto/org.nostr.Grotto-autostart.desktop` into the user's autostart
folder; the package does **not** install an always-enabled login entry. Background
review is on after disclosure, login launch off. Portal denial disables continuous
GUI residence but preserves request-scoped activation and the pending inbox.

The NIP-07 native host retains name `org.nostr.signer_bridge`. Where a recipe
includes it, its browser manifests and explicit user-install helper are under
`share/nostr-signer-webext-host`; installing Grotto alone does not install a browser
extension. Verify browser origin isolation with the actual installed host.

## Pause and local operation

All local NIP-55L, optional NIP-5F socket and remote NIP-46 operations use the same
daemon-owned gate. NIP-5F is off unless explicitly configured, for example:

```ini
[Service]
Environment=NOSTR_SIGNER_ENDPOINT=unix:%t/gnostr/signer.sock
```

Use **Pause signing** to stop remembered signing too. Pause invalidates pending
and queued work; an in-flight backend result is suppressed before delivery if its
security generation changed. Session pause is not persisted. Optional pause on
screen lock is daemon-enforced; inability to observe the session fails closed
when enabled. Unlock does not resume; **Resume signing** needs trusted GUI
confirmation and an unlocked session. A management-window lock is not a key lock.
Ctrl+Q quits approval UI only: the daemon and persisted permissions continue.
Later review-required requests can re-summon it.

## Remote signing and headless deployment (G05)

Pair through **Connect** using paste, physical camera/file QR or an identity-bound
single-use offer. Choose the identity before contacting supplied relays. A foreign
`bunker://` link is not an identity import. See the exact
[pairing/configuration and installed-unit procedure](docs/pairing.md).

The transport credential is distinct from account keys. Its public key names the
endpoint; `get_public_key` and cryptography use the authenticated client's pinned
account. Missing credentials produce ERROR, not automatic regeneration. Use the
trusted **Set up / Reset remote signing endpoint** action to create/replace the
credential; this revokes remote clients and permissions, not identity keys.

Custody roles: Secret Service schema `org.nostr.Grotto.BunkerTransport`, attribute
`slot=default`; Keychain service `Grotto Bunker Transport`, account `default`.
This item is never enumerated as an identity. Explicit seed backup uses a separate
mnemonic item. Client apps never receive private keys, while Grotto's explicit
create/import/export UI may transiently handle them. Never alter a developer's
default keychain or search list, even temporarily.

Configure relays explicitly (no fallback); `wss://` or loopback `ws://` only.
Tor/onion routing is not supported. Direct connections expose IP/timing and
transport-key linkage to the selected relays. Runtime states are DISABLED,
STARTING, LISTENING (EOSE on every configured relay), DEGRADED (some usable),
OFFLINE, STOPPING or ERROR. Desired enabled state and websocket-send success are
not readiness. CLOSED, AUTH and rejected response publication remove readiness;
AUTH is not automatically signed. A client-URI pairing stays pending-ack until
its encrypted acknowledgement receives `OK true`; failed delivery is not admission.

### Explicit network drop-in

The shipped unit keeps `IPAddressDeny=any` and localhost-only allowance. Remote
relays do **not** work through that policy. For an interactive bunker:

```sh
install -d -m700 ~/.config/systemd/user/grotto-daemon.service.d
install -m600 /usr/share/doc/grotto/examples/bunker.conf.example \
  ~/.config/systemd/user/grotto-daemon.service.d/bunker.conf
systemctl --user daemon-reload
systemctl --user restart grotto-daemon.service
systemctl --user show grotto-daemon.service -p ExecStart -p IPAddressDeny -p MainPID
```

Inspect the example before applying: it resets `ExecStart` and `IPAddressDeny`.
For headless operation use **headless-bunker.conf.example instead**, whose command
is `grotto-daemon --bunker --no-approval-ui`; do not install both variants.
The one restart applies systemd policy. Subsequent `ConfigureBunker` changes do
not restart the daemon. Record PID stability, outage/reconnect and native policy
in manual acceptance. Preserve namespace-free hardening and host `/proc` access;
do not add PrivateTmp/ProtectHome/ProtectSystem to a user unit: these break caller
attestation. There is no SIGHUP reload contract; do not send HUP.

Provision the transport credential, actual owner-approved client pairings and
scoped permissions **before** headless use. Existing scoped/pre-approved work
succeeds; new consent or pairing fails promptly with typed NoApprovalAgent rather
than silently allowing or starting a GUI. There is no unattended token-admission
or allow-all mode. Arbitrary `gdbus` calls cannot administer a PID-attested daemon.

## Upgrade, migration and rollback

Stop the daemon before backing up or provisioning its files; store copies
privately. Preserve both `$XDG_CONFIG_HOME/gnostr` and `$XDG_CONFIG_HOME/grotto`,
plus the matching custody items through the owner's key manager. Config files
alone are not a key backup. Never put pairing tokens in logs/configuration.

| File | Ownership and upgrade rule |
|---|---|
| `gnostr/signer-grants.ini` | Legacy compatibility; every load re-sanitizes unsafe broad event allows |
| `gnostr/signer-permissions-v2.ini` | Scoped identity/operation/exact-kind decisions and absolute expiry; broad legacy allows become non-executable Needs review |
| `grotto/clients.ini` v1 (`[clients] version=1`) | SHA-256 principal sections, explicit consent, selected identity, trust tier, paired admission and approved client relays; pending-ack is not paired |
| `grotto/bunker.ini` v1 (`[meta] version=1`) | Authoritative durable desired enabled/relays/allowed-methods; legacy GSettings imported only if absent |
| `grotto/bunker-seen.ini` v1 | Bounded replay fences written before execution; preserve across restart |
| `grotto/activity.json` | Metadata only, bounded retention; logging failure cannot change a crypto result |

Use private 0700 directories and atomic 0600 files; reject higher schema versions.
Legacy non-event grants keep scope; broad event denials remain denials. Never
convert a global auto-approve-kind setting or pubkey allowlist into a signing
grant. An allowlisted but unpaired client still needs explicit connect review.
Use the GUI/Control1 or canonical in-tree writers while stopped for offline
provisioning, not hand-invented sections. The E2E fixture's guarded file credential
backend is **test-only**, not a production vault.

Unsafe broad allows are removed from the live legacy file before scoped policy
is enabled. Rolling back binaries cannot reconstruct scoped permissions; an old
binary may reintroduce unsafe grants. Do not restore an old broad-allow backup
into a live signer. Use an owner-reviewed migration/restore, or stay stopped and
re-pair/review. Never delete the replay guard to make a restart work; corrupt or
unwritable replay state fails closed. Endpoint reset is the authenticated recovery
path and revokes remote admission first. Pairing tokens and session pause are not
persisted. After installing an upgrade, reload systemd, restart once, inspect
status/store health, then exercise one explicitly allowed and one denied request.

## Diagnostics and limits

```sh
systemctl --user status grotto-daemon.service
journalctl --user -u grotto-daemon.service -n 50
# Coarse status is available to same-session callers; this may activate the daemon.
gdbus call --session --dest org.nostr.Signer --object-path /org/nostr/signer/grotto \
  --method org.nostr.Grotto.Control1.GetStatus
```

Never infer signing success from an IPC send, window state or a returned
ApprovalCompleted boolean; check the operation's result. Signing is not publication.
Activity contains metadata, not raw event/message bodies or reasons. Avoid debug
logging of secret-bearing input. See the canonical XML and
[D-Bus contract](../../docs/dbus-interface.md) for typed errors and management auth.

Same-user malware is outside the boundary. Linux PID/path/inode attestation is
retained; UID-only buses inherit weaker caller and approver isolation as documented
there. Flatpak/AppImage cannot bypass those checks for a smoke test; their caller
attestation, trusted-approver, Secret-portal custody, activation ownership and
browser-host gates remain open. No production readiness, physical-camera or live
GNOME/Orca acceptance is inferred from a local test run.
