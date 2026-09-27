# NIP-29 conformance audit — docs/nips `d37b566b → db5fe3d` (nostrc-rxxx)

**Date:** 2026-09-26 · **Spec:** `docs/nips/29.md` at submodule pin `db5fe3d`
**Scope:** `nips/nip29/` (library), `apps/gnostr/plugins/nip29-groups/` (client plugin,
built only with `-DENABLE_NIP29=ON`, `NipOptions.cmake`), `gnome/nostr-homed/src/notify/`
(read-only, worker E), `apps/relayd/src/session/session_routing.{c,h}` + F's in-flight
`session_fed_policy.c` (read-only, worker F).

## Per-revision table

| Upstream | Change | Library `nips/nip29` | Plugin | Notify (E) | Session relay (F) |
| --- | --- | --- | --- | --- | --- |
| `db5fe3d` | group = (relay, id); migration & forks; clients consult admins' `10009` | model already keys on `relay'id` — conformant | groups keyed `relay'id` (forks are separate groups) — conformant; **no 10009 migration/fork detection** → nostrc-7n4t | withdraw id / title keyed by bare `h` → forks coalesce → **nostrc-a33z** | resolution by bare id, first 10009 match → **nostrc-ytua** |
| `bdfa7e6` | `a` tags allowed in pin list | **implemented**: `nostr_group_pin_t` e/a, `nostr_group_pin_is_valid()` | via 39005 merge | n/a | n/a |
| `da1629e` | `banner` on kind 39000 | **implemented**: `group->banner` merge + emit | item `get_banner`; create dialog sends `banner` | n/a (display name only) | n/a |
| `223ddb3` | subgroups: `parent` (≤1) / ordered `child` tags | **implemented**: `parent`, `children[]`, `nostr_group_is_root()` | item `get_parent`; no tree UI → nostrc-prjb | n/a | n/a (39000 already group-routed) |
| `6834e8b` | `naddr1…?invite=<code>` | **implemented**: `nostr_group_reference_parse()` (naddr / `nostr:` / legacy `relay'id`, percent-decoded) | dialog accepts pasted reference; code kept in memory and sent as the 9021 `code` tag | n/a | n/a |
| `f19d0e3` | pinning: kind 9010 `update-pin-list`, kind 39005 | **implemented**: kinds in `nostr-kinds.h`, `NOSTR_PERMISSION_UPDATE_PIN_LIST`, 39005 merge/emit | 39005 in snapshot query + live sub; no display / no 9010 sender → nostrc-prjb | not subscribed — correct (pins are state, not messages) | 9010 covered by F's 9000-9030; **39005 → home_relays** → **nostrc-zi3j** |
| `436d9fd` | kind 10011 favorite follow sets (NIP-51) | n/a | n/a | not subscribed | falls to `home_relays` — correct for a NIP-51 list |
| `f0af204` | NIP-46: unknown methods MUST get an error reply | n/a | n/a | n/a | n/a — the `nips/nip46` bunker already replies `method_not_supported` for unhandled methods (`nip46_session.c` request dispatch default) |

Kind 10011 is not referenced anywhere in the tree; every consumer treats it generically as a
10000–19999 replaceable event, so nothing misroutes it. No constant was added (no user).

## Library behaviour choices (deliberate)

- **Snapshots, not patches.** A newer 39000 without `banner`/`parent`/`child` clears them;
  an empty newer 39005 clears the pins (the spec's "clearing pins is submitting a new list").
- **Subgroup tags on read:** the first well-formed `parent` wins (spec: at most one);
  a self-referencing `parent`/`child` (a cycle relays MUST reject) and ids outside the
  library's group-id charset are dropped; duplicate children are dropped; child order is kept
  (it is the display order).
- **Pins on read:** order is kept; malformed refs (non-lowercase-hex id, bad `kind:pubkey:d`)
  and duplicates are skipped rather than failing the whole list; an `a` ref's `d` may be empty
  or contain `:`.
- **Group-id charset** stays `[a-z0-9_-]` (the relay29/go-nostr convention); NIP-29 says "any
  string", so a group whose id falls outside it cannot be tracked. Pre-existing, unchanged.
- **Invite suffix:** only `invite` is interpreted; other query parameters are ignored; an empty
  code is "no code"; a malformed `%` escape (or `%00`) rejects the reference rather than
  truncating the code. For the legacy form the suffix is searched after the `'` so a relay URL
  with its own query string survives. `naddr` must be kind 39000 with a relay hint; the relay
  `self` pubkey in the naddr is not verified (the model has no field for it).
- **Invite codes are never persisted** (`saved-groups.json` is unchanged): they are bearer
  secrets; the code lives in memory until the next join without an explicit code.

## Out-of-revision findings

- Plugin `create-group` puts metadata on kind 9007 instead of a follow-up 9002 → nostrc-4gf4.
- F's resolver also derives a relay from an `h` value shaped `host'id`, and reads a relay hint
  from `["h", id, relay]`; neither is a NIP-29 shape (noted in nostrc-ytua).
- Not implemented and not required for conformance: NIP-11 `nip29.subgroups` detection, LiveKit.

## Verification

`test_nip29` gained banner, subgroup, pin (e + a) and reference/invite cases
(`nips/nip29/tests/test_nip29.c`); green on macOS (incl. ASan/UBSan) and aarch64 with
`-DENABLE_NIP29=ON`. The plugin builds (`nip29-groups` target) on macOS with `-Wall -Werror`;
the lab has no libpeas, so apps are not built there.
