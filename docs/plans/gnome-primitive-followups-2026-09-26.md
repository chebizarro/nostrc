# GNOME-primitive follow-ups — wave 4 orchestration checklist

**Status:** in progress (orchestrator-owned checklist; sub-agents treat as read-only).
Parent epic: `nostrc-prqu`. Source of findings: `docs/investigations/gnome-gtk-nostr-planned-not-implemented-2026-09-26.md` + worker reports on the 16 landed children.

Five disjoint work items, one worktree each. Merge when verified; delete worktree.

| # | Scope (owned paths) | Beads | Done when |
|---|---|---|---|
| A | `apps/relayd/**`, relay systemd units, `debian/rules` relay flags, `gnome/nostr-homed/systemd/user/nostr-home-*.service.in` | prqu.5, prqu.14, q9ba (part 2: WS upgrade over relay.sock), prqu.17 (if 8rxk design permits), ig4k | packaged session relay has storage (or an explicit, tested cache-less contract) and always answers REQ with EOSE/CLOSED; WS upgrade over the Unix socket works; porthome user units start clean |
| B | `nips/nip55l/**`, `apps/gnostr-signer/**` (NOT `native-host/`), `gnome/seahorse/` additive | y02q, phk4, 1e31, eie5, f7hk, prqu.2, 8x29, llh3, vul2, de9h | decrypt/pubkey calls approval-gated; app_id bound to caller; remember-ACL round-trips; kind-aware dialog; dormant writers gone; builds/tests green on macOS + aarch64 |
| C | `apps/gnostr/**`, `nostr-gtk/**` | prqu.3, prqu.13, prqu.15, prqu.11, vuwu, lwzv, ao0d | gnostr opens nostr: URIs + implements Handler1; zap dialog uses Wallet1; search action; mls KeyPackage discovery via 10002; signer sessions restored + selected npub passed; read-only mode disables send |
| D | `libnostr/**`, `gnome/nostr-dav/**`, `nips/nip47/**`, `nips/nip19/**` build, `debian/**`, RPM spec, `tests/**` hygiene | prqu.4, ir7c, 862u, prqu.6, 4c0o, iq04, xw95, m4y1 | subscriptions register + NULL ctx ok; DAV writes bump created_at/ETag; upstream_mode enforced; libnip19 SONAME + shipped once; nip47 tag spelling; freestanding -Werror + full ctest green on aarch64 |
| E | `gnome/nostr-wallet-agent/**`, `gnome/nostr-settings/**` (wallet page), `gnome/nostr-homed/src/notify/**`, `gnome/nostr-share/**` (xattr only), `browser-extension/**` + `apps/gnostr-signer/native-host/**` (tumh, 4qbt) | prqu.19, prqu.20, prqu.12, prqu.8, whxk, 4qbt, tumh, prqu.16, prqu.18, tepd | non-prompting wallet read; Settings grant/revoke; agent-owned NWC keys; LNURL; site wording; paired-wallet E2E; Flatpak/Epiphany verified or documented; notify honors keys + exposes unread count; share writes xattr |

Deferred to a later wave: k95e, wu3s, hby8, prqu.1, prqu.7, prqu.9, prqu.10, rxxx, 7d96.

## Progress
- [ ] A relayd
- [ ] B signer
- [ ] C gnostr client
- [ ] D libnostr/dav/packaging
- [ ] E wallet/notify/share/webext follow-ups
