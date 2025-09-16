# Testing the Nostr GOA Overlay (Headless)

## Prerequisites

- dbus-run-session
- meson, ninja, pkg-config
- Python 3 (for fake services)
- xvfb-run (optional if driving gnome-control-center)

## Build and install overlay

```
./overlay/install-overlay.sh
```

## Headless test rig

- `tests/integ/spawn_user_dbus.sh` launches a clean session bus via dbus-run-session and runs a provided command.
- `tests/integ/fake_signer.py` provides a minimal `org.nostr.Signer` service that returns a fixed npub.
- `tests/integ/fake_dav.py` provides simple CalDAV/CardDAV endpoints serving static ICS/VCF.
- `tests/integ/test_add_account.sh` wires the above together and verifies EDS sources exist after adding the account, then removes and checks cleanup.

Run:

```
meson test -C _build --suite integ || true
```

The test suite skips gracefully if prerequisites are missing (dbus-run-session, python, etc.).
