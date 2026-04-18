# GOA Overlay & Custom GoaProvider Postmortem

**Beads**: `nostrc-hock`, `nostrc-sb1p`
**Parent epic**: `nostrc-9qqt` (GNOME desktop integration)
**Date removed**: 2026-04-17

## What was removed

### `gnome/nostr-goa-overlay/` (nostrc-hock)

A per-user overlay intended to inject a Nostr provider into GNOME
Online Accounts (GOA). Included:

- A vendored fork of `gnome-online-accounts` (submodule:
  `chebizarro/gnome-online-accounts`, branch `nostr-overlay`)
- A minimal `GoaNostrProvider` (15-line stub, commit `ab209dd5`)
- Overlay install/uninstall scripts replacing `goa-daemon` with
  an `XDG_DATA_DIRS`-shimmed session
- Meson build, CI workflow, integration test harness
- Provisioning templates for EDS calendar/contacts sources
- systemd user units for planned nostr-dav/nostr-notify/nostrfs

### `gnome/goa/` (nostrc-sb1p)

A `GoaProvider` subclass built as `goa-gnostr.so`, installed into
`${CMAKE_INSTALL_LIBDIR}/goa-1.0/providers`. Included ~400 lines of
GTK4 account-picker UI code (commit `312fc3d6`, Feb 2026).

## Why they were removed

### Technical: providers cannot be loaded externally

GOA providers are statically registered inside `goa-daemon` via
`ensure_builtins_loaded()` in `src/goabackend/goaprovider.c`. The
call chain is:

```
goa_provider_get_all() → ensure_builtins_loaded() →
  goa_exchange_provider_get_type()
  goa_google_provider_get_type()
  goa_owncloud_provider_get_type()
  ...
```

There is no `dlopen` scan, no plugin directory, and no
`g_io_extension_point` registration path for third-party providers.
A built `goa-gnostr.so` will install but never be loaded by the
system `goa-daemon`.

The overlay approach (replacing goa-daemon with a custom session
entry) was fragile: it required forking the entire GOA daemon binary,
was untested against GNOME 46+ session management changes, and would
not work in sandboxed (Flatpak) deployments.

### Strategic: local DAV bridge is more robust

The replacement approach (`gnome/nostr-dav/`, tracked under
`nostrc-y0wo`) implements a localhost CalDAV/CardDAV/WebDAV server
that GOA connects to as a standard WebDAV account. This:

- Works with unmodified GNOME Online Accounts
- Works inside Flatpak sandboxes (portal-accessible localhost)
- Requires no forked system binaries
- Follows the same pattern used by Nextcloud, Radicale, etc.

### Documentation: self-contradictory README

The overlay README simultaneously claimed "no vendoring is performed"
and "Build vendor GOA with the Nostr provider patch". The
`DUPLICATE_ITEM_ANALYSIS.md` documented known issues with the overlay
session creating duplicate GOA accounts.

## Preserved artifacts

- The GTK4 account-picker UI from `gnome/goa/` (commit `312fc3d6`)
  can be recovered from git history if needed for the signer app.
- The EDS source templates (`calendar.source.tmpl`,
  `contacts.source.tmpl`) from the overlay's `provision/` directory
  may be useful reference for the nostr-dav bridge.
- The integration test harness pattern (`fake_dav.py`,
  `fake_signer.py`) informs the nostr-dav test design.

## Lessons

1. **Verify extension mechanisms before building**. GOA's provider
   registration is compile-time, not runtime. This was not discovered
   until the overlay was nearly complete.
2. **Prefer standard protocols over system hooks**. A localhost DAV
   server integrates with every DAV-aware desktop, not just GNOME.
3. **Vendored forks of core system daemons are high-maintenance**.
   Every GNOME release would require rebasing the overlay fork.
