# nostr-home-status@nostrc

A tiny gnome-shell **user-session** extension that renders a compact
portable-home status indicator in the top panel.

The extension is a strict *reader* — it never writes, never spawns a
process, and never surfaces anything that could be secret material.
It watches a single file:

    $XDG_STATE_HOME/nostr-homed/porthome-status.json
    (fallback ~/.local/state/nostr-homed/porthome-status.json)

produced by the portable-home stack (see `nh_porthome_status.h`).

## Schema (v1)

```
{ "schema": 1,
  "syncd": {
     "state":                    "ready" | "partial" | "limited",
     "last_push_gen":            <u64>,
     "last_pull_gen":            <u64>,
     "last_error_class":         "<slug>",
     "last_error_ts":            <u64>,
     "pinned_gen_count":         <u32>,
     "cache_bytes":              <u64>,
     "quota_bytes":              <u64>,           /* I3 populates */
     "conflicts_active_count":   <u32>,
     "recent":                   [ { ts, cat, key, summary, delivered } ]
  },
  "fuse": {
     "mounted":                  <bool>,
     "mountpoint":               "<path>",
     "generation":               <u64>,
     "in_flight_fetches":        <u32>,
     "offline_misses_count":     <u64>,
     "tier2_poisoned_count":     <u64>
  },
  "provisioner": {
     "last_provisioned_ts":      <u64>,
     "last_state":               "ready" | "limited" | "internal_error"
  }
}
```

## Deploy (per-user)

    UUID=nostr-home-status@nostrc
    DEST=$HOME/.local/share/gnome-shell/extensions/$UUID
    mkdir -p "$DEST"
    cp metadata.json extension.js stylesheet.css "$DEST"/
    gnome-extensions enable $UUID

## Deploy (system-wide, packagers)

Enable `-DNOSTR_HOMED_ENABLE_PORTHOME_STATUS_EXTENSION=ON`; the CMake
install rule stages the three files under
`$CMAKE_INSTALL_DATADIR/gnome-shell/extensions/nostr-home-status@nostrc/`
alongside the greeter extension. Enabling the extension for a user is
policy — packagers should not auto-enable it.

## Design gate

If a future status-file field looks like it could carry a bunker URI, a
pubkey label, or anything else user-identifiable beyond the account
already logged into this shell session — do NOT add it to the schema.
See the "producer contract" note in `../nostr-login-qr@nostrc/README.md`
for the sibling extension's rules.
