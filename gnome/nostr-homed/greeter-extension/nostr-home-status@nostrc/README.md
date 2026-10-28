# nostr-home-status@nostrc

A gnome-shell **user-session** extension that shows portable-home status in
the top panel:

* sync state, and home-loading progress while the provisioner runs;
* cache and home-size usage against their limits (text plus a bar);
* unresolved conflicts from the personal home and shared folders, each with a
  plain-language explanation, the choices syncd accepts for it, and
  "Show in Files".

It reads one file, never surfaces secret material and never edits files:

    $XDG_STATE_HOME/nostr-homed/porthome-status.json
    (fallback ~/.local/state/nostr-homed/porthome-status.json)

The only process it starts is `nostr-home-status conflicts resolve ID
--CHOICE`, when the user picks a resolution (argv, no shell). That queues a
request for `nostr-home-syncd`, which applies and publishes it; the outcome
is shown as a notification.

`model.js` turns the status document into what is displayed and has no GNOME
imports; `extension.js` only renders.

## Schema read (v1, additive)

```
{ "schema": 1,
  "syncd": {
     "state":               "idle"|"pulling"|"pushing"|"reconciling"|"limited"|"offline"|"error",
     "last_error_class":    "<slug>",
     "cache_bytes":         <u64>,
     "cache_quota_bytes":   <u64>,
     "conflicts_active_count": <u32>,
     "home_conflicts":      [ { id, path, kind, present, current_present, state,
                                choices[], expect, mine, destination|marker } ],
     "vault_conflicts":     [ { id, vault_id, path, writer, head, present,
                                current_present, state, choices[], expect, mine,
                                destination?, overflow, root_snapshot?, reason? } ],
     "resolutions":         [ { ts, request, conflict, choice, result, reason } ]
  },
  "provisioner": {
     "state":               "idle"|"preparing"|"fetching"|"verifying"|"publishing"|"done"|"error",
     "chunks_done":         <u32>, "chunks_pending": <u32>,
     "home_bytes":          <u64>, "home_quota_bytes": <u64>
  }
}
```

Other keys (`fuse`, `gc`, …) are ignored.

## Deploy (per-user)

    UUID=nostr-home-status@nostrc
    DEST=$HOME/.local/share/gnome-shell/extensions/$UUID
    mkdir -p "$DEST"
    cp metadata.json extension.js model.js stylesheet.css "$DEST"/
    gnome-extensions enable $UUID

## Deploy (system-wide, packagers)

Enable `-DNOSTR_HOMED_ENABLE_PORTHOME_STATUS_EXTENSION=ON`; the CMake
install rule stages the files under
`$CMAKE_INSTALL_DATADIR/gnome-shell/extensions/nostr-home-status@nostrc/`.
Enabling the extension for a user is policy — packagers should not
auto-enable it.

## Testing

* `homed_status_extension_model` (ctest, needs `gjs`): the whole view model
  headless — states, quota maths, progress, conflict text and choices, the
  resolve argv and CLI outcome mapping.
* `tests/integration/run_status_extension_shell.sh EXT_DIR [OUT]` (opt-in,
  exits 77 without gnome-shell): loads the extension into a real headless
  GNOME Shell (`--headless --virtual-monitor`) with a probe extension that
  reads the indicator, opens the conflict submenu, screenshots it
  (`OUT/menu.png`) and activates a choice against a stand-in CLI. Needs a
  system D-Bus; in a container without logind remove `/run/systemd/seats`
  first. It is not part of the default test set.

## Design gate

If a future status-file field looks like it could carry a bunker URI, a
pubkey label, or anything else user-identifiable beyond the account
already logged into this shell session — do NOT add it to the schema.
See the "producer contract" note in `../nostr-login-qr@nostrc/README.md`
for the sibling extension's rules.
