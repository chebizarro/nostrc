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
- [x] A relayd — merged; prqu.17 blocked on 8rxk
- [x] B signer — merged 5ea6cfc9 (12 beads + 8sya, o8mx nip55l half, a4w5); follow-ups wkzj (E), jppi, q23h, 9z5w (decision: Debian approver UI), yjky, 56id
- [x] C gnostr client — merged (7 beads); follow-ups tvoi, 46h7
- [x] D libnostr/dav/packaging — merged (12 beads incl. wr3t, w8y1, pnc7, 6tuz)
- [x] E wallet/notify/share/webext follow-ups — merged 290f0273 (10 beads incl. wkzj; tumh partial: Flatpak Firefox hd8u, Epiphany a5fr); follow-ups muhk, dnbf, eqdz, a16c
- [x] F session-relay federation — merged 7b657793 (7d96); follow-ups t24q (dav/share consume FederationState), jedb, elgy, 8cc1, abk1
- [x] H NIP-29 audit + ctest failures — merged (rxxx ohrz yk3t bvka twwl krqc); report docs/reviews/nip29-conformance-2026-09-26.md; follow-ups lqm2 (P1 syncd inotify), zi3j, ytua (relayd), a33z (notify), 7n4t, 4gf4, prjb (plugin), bn7z, 2w9b
- [ ] I share/dav federation consumers (t24q eqdz k95e wu3s) — running
- [ ] J relayd federation follow-ups (zi3j ytua jedb elgy 8cc1 abk1 tvoi) — running
- [ ] K homed syncd inotify P1 + notify + test hygiene (lqm2 a16c a33z bn7z 2w9b) — running
- [ ] L gnostr follow-ups (jppi 46h7 4gf4 7n4t prjb) — running
- [ ] M signer/nip5f/wallet follow-ups (q23h yjky 56id muhk dnbf) — running
- Deferred: 9z5w (decision: ship an approver UI/CLI in Debian?), hd8u (Flatpak Firefox portal), a5fr (Epiphany), G still running
