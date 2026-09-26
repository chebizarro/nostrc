# Investigation: GNOME/GTK Nostr features planned in repo — partial or unimplemented

**Date:** 2026-09-26  
**Investigator:** Deep-investigation orchestrator + 3 parallel explore agents + orchestrator verification pass (spot-checked initial findings; found initial scope insufficient — added FUSE + share-to + plugin roster + Seahorse connectivity to the audit).

**Correction note (2026-09-26 pm):** The first pass of this report missed the FUSE stack, the "share to" primitives gap, and the fact that `apps/gnostr/plugins/` contains **14** plugins (not just NIP-29). It also over-claimed the Seahorse helper's integration completeness. This revision incorporates those gaps with verified file:line evidence.

## Summary

- **FUSE/relay-as-filesystem**: Two FUSE trees exist (`nostrfs` legacy + `nostr-home-fuse` Phase-4 overlay), both experimental-gated OFF by default. They mount portable-home (namespace from a snapshot, content via Blossom/CAS), **not** raw relay events. No plan proposes "each event as a file under `/nostr/wss:…/`."
- **"Share to" primitives**: **Not implemented, not planned.** Gnostr registers `x-scheme-handler/nostr` as an *inbound* URL handler but has zero share-target/XDG-portal integration. There is no `SendToNostr` action, no `org.freedesktop.portal.Share` handler, no MIME registration for arbitrary files/text/images.
- **`apps/gnostr/plugins/` roster**: **14 plugins**, LOC ranging 164→9306; 11 always-built + 3 gated by `TARGET nip{29,communikeys,concord}` (all three OFF by default in `NipOptions.cmake`). This means the largest plugin (`concord-communities`, 9306 LOC) is **not built by default** on typical distributions.
- **Groundhog GNOME messaging app**: 49 KB plan, zero code (confirmed). Marmot kind-30443 prerequisite: only in test vectors, no source implementation.
- **Seahorse helper**: Real code (154 lines), but the schema it defines (`org.gnostr.Key`) is **not what the main gnostr client writes**. Gnostr's keystore uses a *different* schema (`org.gnostr.NostrKey`) with different attributes. The helper is only linked into `gnostr-signer` and only used for a single `delete-by-identity` call. Net effect: incomplete — the "Nostr keys show up nicely in Seahorse" story is partly aspirational.

## Symptoms

The user asked what's been planned for GNOME/GTK Nostr features that remains partially or not implemented. Initial three-explore fan-out surfaced messaging + timeline + signet + version + Seahorse clusters. Follow-up user challenge ("what about FUSE for relays as filesystems? What about share-to primitives? Is Seahorse actually supported or hallucinated?") caught three real gaps in the initial pass.

## Background / Prior Research

### Cluster 1 — Messaging / NIP-29 / Groundhog (initial explore verified)

- `docs/plans/nip-29-gnostr-plugin-2026-05-18.md` (untracked): implementation landed as commit `b5362717` (41 files, +6867 lines) as `apps/gnostr/plugins/nip29-groups/` — 4389 LOC. Plan doc never committed. Built only when `ENABLE_NIP29=ON` (default OFF, `NipOptions.cmake:123`, gated via `TARGET nip29` at `apps/gnostr/CMakeLists.txt:408-412`). Missing: test harness the plan named (5 scenarios), `PENDING_JOIN`/`PENDING_LEAVE` UI states, reply composing, kind-10009 discovery, moderation kinds 9000/9001/9002/9005/9009.
- `docs/plans/groundhog-gnome-messaging-2026-09-25.md` (untracked, 49 KB): a separate GTK4/libadwaita app in `gnome/groundhog/` covering NIP-17 DMs + NIP-29 relay groups + Marmot MLS in one conversation list. **Zero code.** `gnome/` contains only `dbus/`, `nostr-dav/`, `nostr-homed/`, `seahorse/`. The Marmot kind-30443 KeyPackage migration it depends on is also unstarted: `30443` appears only in `libmarmot/tests/test_interop.c:559-560` and test vectors, zero implementation source.
- `docs/reviews/groundhog-gnome-messaging-critique-2026-09-25.md` (untracked): a review folded back into the plan; adds no new gaps.

### Cluster 2 — Timeline compositor + rich-media (initial explore verified)

- 4-doc compositor track (`docs/designs/gnostr-timeline-compositor.md` + `gnostr-timeline-compositor-definitive-2026-05-20.md` + `gnostr-timeline-compositor-full-implementation.md` + `gnostr-timeline-performance-2026-08-06.md`): **fully landed.** `GtkListView` in `nostr-gtk/src/gnostr-timeline-view.c:404-406`; snapshot model in `apps/gnostr/src/model/gnostr-timeline-snapshot-model.c:211-239, 341-363`; row factory in `apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c:1125-1152, 324-336`; performance parameters (150-row window / 90 pending-head / 600 geometry cache) in `apps/gnostr/src/ui/gnostr-timeline-feed-controller.c:10-15, 585-600`.
- `docs/plans/gnostr-rich-media-timeline-2026-08-03.md`: parser/caps/thumbnailer/media service landed (`apps/gnostr/src/model/gnostr-timeline-hydrator.c:119-121`, `apps/gnostr/src/services/gnostr-media-service.c:1972-1983, 3624-3629`). **Missing: "+N more" affordance for overflow.** Getter exists (`gnostr-timeline-item-view-model.c:881-884`) but has no UI caller — verified by orchestrator: only `overflow` reference in `apps/gnostr/src/ui/` is `GTK_OVERFLOW_HIDDEN` (unrelated widget attr) in `gnostr-video-player.c:906`.
- `docs/designs/nostrdb-retention-eviction-policy.md`: explicitly "not implemented" per its own header.

### Cluster 3 — Signet passkeys / Versioning / Seahorse (initial explore, Seahorse claim revised below)

- `docs/plans/signet-passkeys-fido-2026-07-02.md`: **fully landed** end-to-end. 8 commits `e08341d6` → `4a4d039c`. Real code: `signet/src/{fido,fido_cbor,fido_crypto_openssl,fido_ctaphid,store_passkeys}.c` + `signet/tests/*` including virtual CTAP-HID device via `/dev/uhid`. **Only outstanding item**: no `signet` row in `VERSION_MANIFEST.md`.
- `docs/plans/versioning-release-tagging-2026-08-10.md` (untracked): **partial.** `cmake/VersionHelpers.cmake`, `scripts/tag_release.py`, and per-component tag parsing in `.github/workflows/release.yml` all exist. But `VERSION_MANIFEST.md` is incomplete (no signet, no libhanami); `packaging/rpm/nostr-login.spec` uses one flat `Version: 0.4.0`; `debian/rules` tracks an ad-hoc "Phase 1/5" scheme; and the plan doc itself never landed in git.

### Cluster 4 — FUSE stack (missed by initial pass; added on user challenge)

**Two independent FUSE 3 trees in the repo:**

1. **`gnome/nostr-homed/src/fs/nostrfs.c`** — **954 lines.** Legacy roaming stack. FUSE 3 high-level ops. Namespace from a flat `nh_manifest` JSON blob in the NSS cache under `settings.manifest.<ns>`. Content from unencrypted CAS at `/var/cache/nostrfs/<uid>/<cid>`. Uses NIP-98 kind-27235 auth (which HFR-D7 retires) and publishes kind **30081** (not 30078). Hardcoded fallback relays `wss://nos.lol,wss://nostr.wine` at `nostrfs.c:273`. Build gate: `EXPERIMENTAL_ROAMING` + `FUSE3` availability. Units: `gnome/nostr-homed/systemd/nostrfs@.service` (system) + `.../user/nostrfs@.service` (user). Ships integration test shells (`run_nostrfs_{basic,writeback,writeback_fake_blossom}.sh`). Does **not** reference `porthome` at all: no `nh_porthome_*`, no `nh_syncd_*`, no encryption, no chunking. On CAS miss, returns the literal string `CID:<hex>` as file content.

2. **`gnome/nostr-homed/src/porthome-fuse/nostr-home-fuse.c`** — Phase-4 read-only overlay. Marked "EXPERIMENTAL AND UNREVIEWED" at file top. Gated by `NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL` (default OFF). Namespace from `$XDG_STATE_HOME/nostr-homed/snapshot.json`; content flows through a 4-tier ladder (`nh_fuse_source`); every mutator returns `EROFS`; separate systemd user unit (`Type=notify`); no `allow_other`/`allow_root`/`CAP_SYS_ADMIN`. Design at `docs/designs/nostrfs-porthome-overlay.md` (extensive spec: D1-D16 decisions, D4 APPROVED 2026-09-23, but many decisions still "open for maintainer" per §15). Bead: `nostrc-1u55` (found via commit reference).

**Key clarification**: neither is "relays as filesystems" in the sense of *browsing raw events by kind/pubkey through a filesystem view*. Both mount **portable home** (a user's home directory materialized from a snapshot + Blossom/CAS backing store). The legacy `nostrfs` was closer to the "browse relays" idea (its `RELAYS_DEFAULT` env var hints at that lineage), but it doesn't actually expose per-relay/per-event filesystem hierarchy — it just uses relays as one of the fetch backends for a manifest-driven flat file tree.

**Status**: Neither is on by default; the legacy is largely superseded by the Phase-4 overlay; the Phase-4 overlay's own design doc concedes it's "largely redundant with `$HOME`" until lazy-mode lands (§10, "the single largest open decision" per §15).

### Cluster 5 — "Share to" primitives (missed by initial pass; user-flagged specifically)

Verified: **no code, no plan.**

- `apps/gnostr/data/org.gnostr.gnostr.desktop:12` — `MimeType=x-scheme-handler/nostr;x-scheme-handler/web+nostr;`
- `apps/gnostr-signer/data/org.gnostr.Signer.desktop:16` — `MimeType=x-scheme-handler/nostr;x-scheme-handler/bunker;`

Both are **inbound** URL handlers only (clicking `nostr:` links opens gnostr). Neither registers as a share target for text/images/files. Grep confirms:

- No `org.freedesktop.portal.Share` / `XdpPortal` / `open_uri_portal` references anywhere in `apps/`, `gnome/`, or the docs.
- No `MimeType=image/*` / `text/plain` / any non-scheme MIME on any gnostr `.desktop` file.
- No plan doc in `docs/plans/` proposes a share-to workflow. The word "share" in the Groundhog plan refers to sharing identity/keystore between apps, not filesystem-share primitives (verified by reading `docs/plans/groundhog-gnome-messaging-2026-09-25.md:9,42`).

Impact: a user cannot "Share this URL / image / selection to Nostr" from Firefox, Nautilus, GNOME Files, Loupe, or Text Editor. Publishing requires opening gnostr and composing manually.

### Cluster 6 — `apps/gnostr/plugins/` full roster (missed by initial pass)

**Fourteen plugins**, not one:

| Plugin | LOC (approx, `*.c` under plugin dir) | Build gate | Notes |
|---|---|---|---|
| `concord-communities` | **9306** | `TARGET nip_concord` (option `ENABLE_NIP_CONCORD` **OFF** default) | Largest plugin; NIP-CAS-0008 Concord communities; not built by default |
| `mls-groups` | **7469** | always-built (`add_subdirectory` at `apps/gnostr/CMakeLists.txt:407`) | Marmot-adjacent MLS UI |
| `nip29-groups` | 4389 | `TARGET nip29` (option `ENABLE_NIP29` **OFF** default) | Covered in Cluster 1 |
| `nip34-git` | 3154 | always-built | git-over-Nostr |
| `communikeys-communities` | 2957 | `TARGET nip_communikeys` (option `ENABLE_NIP_COMMUNIKEYS` **OFF** default) | NIP-CAS-0007; not built by default |
| `nip47-nwc` | 1194 | always-built | Nostr Wallet Connect |
| `nip99-marketplace` | 569 | always-built | classifieds/marketplace |
| `nip57-zaps` | 555 | always-built | Lightning zaps |
| `nip77-negentropy` | 484 | always-built | negentropy relay sync |
| `nip64-chess` | 478 | always-built | Chess over Nostr |
| `nip17-dms` | 398 | always-built | Private DMs (NIP-17) |
| `nip55-androidsigner` | 253 | always-built | Android signer bridge (Linux-side only) |
| `nip98-httpauth` | 237 | always-built | HTTP auth headers |
| `nip49-keyencrypt` | 164 | always-built | Key encryption (nsec1 → ncryptsec1) |

**Verdict**: Most are small feature plugins that appear to be shipped-and-linked. However **three of the largest — `concord-communities` (9306 LOC), `nip29-groups` (4389 LOC), `communikeys-communities` (2957 LOC) — are OFF by default**. Their combined 16,652 LOC represents a significant amount of code that a default `dpkg-buildpackage`-built gnostr binary does NOT contain. `mls-groups` at 7469 LOC IS default-built.

I did not audit each plugin's completeness against a plan doc (only NIP-29 has a plan doc in `docs/plans/`). Each is a candidate for its own investigation.

### Cluster 7 — Seahorse helper (initial claim revised on user challenge)

**What EXISTS**:
- `gnome/seahorse/CMakeLists.txt`: builds `gnostr-secret` as a STATIC lib, installs at `${_libdir}/gnostr-secret.a`, installs `org.gnostr.secret.schema.txt` doc to `${_datadir}/gnostr/`.
- `gnome/seahorse/secret_store.c` (92 lines): defines `SecretSchema gnostr_secret_schema` with name **`org.gnostr.Key`** and attributes `{type, npub, uid, curve, origin, hardware_slot}`. Exposes 3 functions:
  - `gnostr_secret_store_save_software_key(npub, uid, secret, error)` — writes to Secret Service
  - `gnostr_secret_store_find_all(error)` — enumerates
  - `gnostr_secret_store_delete_by_identity(npub, uid, error)` — deletes matching items
- `gnome/seahorse/secret_store.h` (37 lines): header.
- `gnome/seahorse/org.gnostr.secret.schema.txt`: documentation describing the label format (`"Nostr key: [display_name or uid] (npub1...)"`) that Seahorse would show.

**What the initial explore claimed**: "gnostr Nostr keys appear inside the actual GNOME Seahorse GUI." **This is not accurate as stated.**

**What the code actually does** (verified via `grep`):
- The main gnostr binary (`apps/gnostr/src/util/keystore_libsecret.c`) uses a **different** schema:
  - Name: **`org.gnostr.NostrKey`** (vs. Seahorse helper's `org.gnostr.Key`)
  - Attributes: `{npub, application}` (vs. Seahorse helper's `{type, npub, uid, curve, origin, hardware_slot}`)
  - Application: `"org.gnostr.Client"` (via `#define GNOSTR_APP_ID "org.gnostr.Client"` at line 29)
- Verified via `grep -rn "gnostr-secret\|gnostr_secret_" apps/gnostr/CMakeLists.txt` — **the main gnostr binary does NOT link `gnostr-secret`**. Not built into the client.
- The only outside consumer of `gnostr-secret`:
  - `apps/gnostr-signer/CMakeLists.txt:212-213` — links it into `${APP_SIGNER_BIN}`
  - `apps/gnostr-signer/src/ui/settings_page.c:88` — one call site: `gnostr_secret_store_delete_by_identity`. That's it. Only a delete path; no save/find call sites in the shipped code.

**Verdict**: The Seahorse helper is **real** (compiled, installed, working code that would render properly if used) but **not actually connected end-to-end**. The main client writes keys to `org.gnostr.NostrKey` (different name, different attributes, no `curve`/`origin`/`hardware_slot` metadata for pretty rendering), so Nostr keys stored by gnostr will NOT appear in Seahorse under the "Nostr key: …" label the schema doc promises. They appear under gnostr's own generic schema instead.

To make the initial explore's claim TRUE, the following would need to happen:
1. Link `gnostr-secret` into the `gnostr` binary (`apps/gnostr/CMakeLists.txt` addition).
2. Migrate `apps/gnostr/src/util/keystore_libsecret.c` to use `gnostr_secret_store_save_software_key` (or refactor its own schema to match `org.gnostr.Key`).
3. Provide a migration path for keys already stored under `org.gnostr.NostrKey`.

Absent those steps, the Seahorse helper is a **156-line dormant utility** with one live delete call site.

## Investigator Findings

_Not run as a separate agent — findings synthesized directly from explore reports + orchestrator verification pass. Verification touched: `NipOptions.cmake:123`, plugin CMakeLists gates, `apps/gnostr/src/util/keystore_libsecret.c:14-40`, `apps/gnostr-signer/CMakeLists.txt:211-220`, `apps/gnostr-signer/src/ui/settings_page.c:88`, `gnome/nostr-homed/src/fs/nostrfs.c` file header + line 273, `gnome/nostr-homed/src/porthome-fuse/nostr-home-fuse.c` file header, `docs/designs/nostrfs-porthome-overlay.md`, all 14 `apps/gnostr/plugins/*/` LOC counts, all `.desktop` files under `apps/`._

## Investigation Log

### Phase 1 — Initial three-cluster explore

- **Coverage**: messaging (NIP-29 + Groundhog), timeline (compositor + rich-media), auth/version (signet, versioning, seahorse-helper).
- **Result**: identified 3 partial + 1 never-started + 3 fully-landed tracks; **missed FUSE, share-to, and 13 of the 14 plugins**.

### Phase 2 — User challenge & verification pass

- **Prompt**: user asked about "FUSE for viewing relays as filesystems", "share-to primitives", and whether the Seahorse claim was hallucinated.
- **Findings**:
  - FUSE stack exists (two independent trees, both experimental-gated); the "relays as filesystems" framing doesn't quite match — it's "portable-home from relay-backed content as filesystem." No plan for a "raw relay events as file tree" mount.
  - Share-to primitives: neither code nor plan. Only inbound URL handling.
  - Seahorse helper is real but **imperfectly integrated**: schema mismatch between the helper library and the main client's keystore.
  - Plugin roster is 14, not 1.
- **Conclusion**: initial audit was materially incomplete. Report rewritten with additions.

## Root cause — audit gaps by track (revised)

**Not implemented at all**:
- Groundhog GNOME messaging app (49 KB plan, plan-only, no code)
- Marmot kind-30443 KeyPackage migration (blocker for Groundhog MLS)
- Share-to primitives (no code, no plan)
- "Relays as filesystems" as a browsable event tree (neither FUSE binary implements this shape)

**Partially implemented**:
- **NIP-29 plugin** — shipped 4389 LOC, OFF by default, missing tests/pending-states/moderation/replies/kind-10009 discovery
- **NIP-29 spec conformance** vs `docs/nips` `db5fe3d` — tracked as `nostrc-rxxx` filed earlier this session
- **Rich-media "+N more" affordance** — getter exists, no UI caller
- **Versioning/release-tagging** — CMake+CI landed, manifest+RPM+Debian misaligned
- **`nostrfs` legacy FUSE** — 954 LOC, `EXPERIMENTAL_ROAMING`-gated, uses retiring NIP-98 kind-27235 auth + kind 30081 events (design doc calls out both as legacy-only)
- **`nostr-home-fuse` Phase-4 overlay** — real implementation of design doc, `NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL=OFF` default, design's own §15 lists lazy-mode as "the single largest open decision"
- **Seahorse helper** — 154 LOC library, installed, but not connected to gnostr's own keystore (schema mismatch means it's a dormant delete-only utility in `gnostr-signer` settings)
- **Concord + Communikeys plugins** — 9306 + 2957 LOC of community plugin code, **OFF by default**; unknown completeness against any plan doc

**Fully landed (for calibration)**:
- Timeline compositor track (4 docs, all landed with matching code)
- Signet passkeys / FIDO2 (rare plan-committed-alongside-implementation exemplar)
- MLS groups plugin (7469 LOC, always-built)
- Most small plugins (nip17-dms, nip47-nwc, nip57-zaps, nip77-negentropy, nip98-httpauth, nip49-keyencrypt, nip34-git, nip55-androidsigner, nip64-chess, nip99-marketplace)

## Recommendations (revised)

### File beads that don't exist for real gaps

1. **`[epic] Groundhog GNOME messaging app`** — P2. Currently ZERO tracked work for a 49 KB plan.
2. **`Marmot: migrate KeyPackage kind 443 → 30443`** — P2. Prerequisite for Groundhog MLS. Test vectors already exist so scope is scoped.
3. **`gnostr share-to primitives (GNOME/XDG Share portal integration)`** — P3. Blank-slate feature — no code, no plan. Would let users publish text/images/URLs from Firefox / Files / Text Editor via the OS share sheet. Concrete acceptance: `apps/gnostr/data/*.desktop` gains a `[Desktop Action share]` + implements `org.freedesktop.portal.Share`-adjacent handling, OR gnostr registers a share-target via `MimeType=image/*;text/plain;text/uri-list;`.
4. **`Seahorse helper: unify schema with gnostr keystore OR remove the dead helper`** — P3. Currently a 156-line dormant library with schema mismatch. Options: (a) migrate `apps/gnostr/src/util/keystore_libsecret.c` to use `gnostr-secret`, (b) update `gnostr-secret`'s schema to match `org.gnostr.NostrKey`, or (c) delete the helper as unused.
5. **`Audit concord-communities plugin (9306 LOC) for completeness / default-on candidacy`** — P3.
6. **`Audit communikeys-communities plugin (2957 LOC) for completeness / default-on candidacy`** — P3.
7. **`docs/designs/nostrfs-porthome-overlay.md §15 open decisions`** — P3. Lazy mode + `~/.config/nostr-homed/lazy` policy remains explicitly deferred; design says it's the single largest open decision.
8. **`Investigate 'raw relay events as filesystem tree' feature idea`** — P3 or backlog. User's suggestion is legitimately unimplemented AND unplanned. Would be a new plan-doc-driven track.

### From the earlier draft (still valid)

9. **Complete NIP-29 plugin v2** (tests + pending states + moderation + replies + kind-10009) before flipping `ENABLE_NIP29=ON` default. Combine with the pending `nostrc-rxxx` (NIP-29 spec conformance).
10. **Wire the "+N more" affordance** in the rich-media timeline (UI caller in `apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c`).
11. **Reconcile `VERSION_MANIFEST.md`** with reality (add signet, libhanami; align RPM/Debian).
12. **Resolve `nostrc-14d`** (timeline compositor IN_PROGRESS since May, "committed as b171bda1 but not pushed").
13. **Commit or delete the untracked plans** in working tree (nip-29-plugin, groundhog + critique, versioning).

## Preventive Measures

- **Commit plan docs alongside their epic beads.** Strongest correlation: untracked plan → incomplete implementation. Signet-passkeys is the exemplar of "plan + code + bead all commit together, ships end-to-end."
- **Add a `Status:` header at the top of each plan doc** with values `plan-only` / `partial` / `landed` / `superseded-by <ref>`. Only the Groundhog plan currently does this.
- **When a helper library defines a schema for interop (like `gnostr-secret`), add a runtime assertion or CI test** that the schema name/attributes match the consumer's usage. Would have caught the Seahorse helper's dormancy at first link.
- **Tag beads with a `plan-doc:<name>` label** so they surface as a family; would let `bd list --label=plan-doc:nip-29-gnostr-plugin` show all work items driving the plan.
- **Add a top-level `docs/plans/README.md`** with a table cross-referencing plan → status → primary bead → shipped commit. Would prevent the current situation where an investigator must guess whether an uncommitted plan represents active work.
- **On investigations: mandate the pair investigator step** even when explores look complete. The three explores I ran covered known plan-doc clusters but missed anything not named in their briefs (FUSE, share-to, plugin roster). A pair with a fresh grep across `apps/`, `gnome/`, `docs/plans/`, `docs/designs/` would have caught these.

## Sources referenced

- Plans (11): `docs/plans/*.md`
- Designs (7): `docs/designs/*.md`, with focus on `gnostr-timeline-compositor.md`, `nostrdb-retention-eviction-policy.md`, `nostrfs-porthome-overlay.md`, `home-from-relay.md`, `packaging-plan-debian-fedora.md`
- Reviews (51): `docs/reviews/*.md`, focus on rich-media critique + groundhog critique + samba acceptance runs
- Code:
  - `apps/gnostr/plugins/*/` (14 plugins)
  - `apps/gnostr/src/util/keystore_libsecret.c:14-40`
  - `apps/gnostr/data/org.gnostr.gnostr.desktop:12`
  - `apps/gnostr-signer/CMakeLists.txt:211-220`
  - `apps/gnostr-signer/src/ui/settings_page.c:88`
  - `apps/gnostr-signer/data/org.gnostr.Signer.desktop:16`
  - `gnome/seahorse/{secret_store.c,secret_store.h,CMakeLists.txt,org.gnostr.secret.schema.txt}`
  - `gnome/nostr-homed/src/fs/nostrfs.c` (954 LOC, `nostrfs.c:273` hardcoded fallback)
  - `gnome/nostr-homed/src/porthome-fuse/nostr-home-fuse.c` (file header)
  - `NipOptions.cmake:123` + related option lines
  - `apps/gnostr/CMakeLists.txt:396-424` (plugin registration + gates)
- `VERSION_MANIFEST.md`, `cmake/VersionHelpers.cmake`, `scripts/tag_release.py`, `.github/workflows/release.yml`
- Beads: `nostrc-14d`, `nostrc-rxxx`, `nostrc-vr1u`, `nostrc-svsj`, `nostrc-72ox`, `nostrc-ecrx`, `nostrc-1u55` (porthome-fuse), plus the 5 P2/P3 beads from Wave 6 tail
- Implementation commits referenced: `b5362717` (nip29 plugin), `70075c05` (initial GNOME/GOA + Seahorse), `568732a1` (compositor), `7211edbe`/`77ca1372` (rich rows), `f16f2d56`→`3a8b2e87` (performance), `e08341d6`→`4a4d039c` (signet passkeys phases), `b171bda1` (timeline compositor stability — local-only unpushed per `nostrc-14d` notes)

## Work items filed (2026-09-26)

Epic **`nostrc-prqu`** — Nostr as a first-class GNOME primitive. Children (→ = blocked-by):

| Bead | P | Item | Depends on |
|---|---|---|---|
| `nostrc-2thp` | P1 | Fix `x-scheme-handler/nostr` collision (signer drops `nostr:`); add `X-Nostr-Kinds=` to desktop files | — |
| `nostrc-djvs` | P3 | Seahorse helper label from uid/npub (`secret_store.c:27`) | — |
| `nostrc-bml6` | P1 | Unify libsecret key schemas → one identity schema in `gnome/seahorse`, adopted by nip55l, with migration | — |
| `nostrc-e5nz` | P2 | gnostr client stops holding nsec; signatures only via `org.nostr.Signer` | bml6 |
| `nostrc-1v65` | P1 | `nostr-dispatcher` (`org.nostr.Dispatcher1`): kind→handler registry, owns `nostr:`/`web+nostr:`, `handlers.list`, `org.nostr.Handler1`, NIP-89 hook, notify deep-link migration | — |
| `nostrc-tmsc` | P2 | `libnostr-publish`: factor NIP-65 outbox commit out of `nd-publisher.c` | — |
| `nostrc-1xak` | P1 | `nostr-share` CLI + dialog, Open-With MIME registration, Blossom + imeta/1063, 30023, 30617 | tmsc |
| `nostrc-da9c` | P2 | `nostr-seal`: NIP-44 key agreement + porthome chunked AEAD container; NIP-49 passphrase mode; `NIP44DeriveConversationKey` on signer | — |
| `nostrc-xlf3` | P2 | `nostr-nautilus` Files extension (Share / Encrypt for… / Upload to Blossom) | 1xak, da9c |
| `nostrc-yka8` | P1 | `nostr-wallet-agent` (`org.nostr.Wallet1`) + `lightning:` / `bitcoin:` / `nostr+walletconnect:` handlers | bml6, tmsc |
| `nostrc-jjyp` | P1 | Browser WebExtension: NIP-07 → signer, WebLN → wallet agent (native messaging) | yka8 (WebLN half only) |
| `nostrc-janr` | P2 | `org.nostr.Settings` preferences app + `org.nostr.SessionRelay1` stats | yka8 |
| `nostrc-33f1` | P3 | GNOME Shell extension: Quick Settings relay toggle + indicators | janr, yka8 |
| `nostrc-hwwn` | P2 | Shell search provider over local nostrdb | 1v65 |
| `nostrc-jaxi` | P2 | GVfs backends `blossom://`, `nostr://npub/files/` (design doc first) | — |
| `nostrc-00i0` | P3 | Native EDS backends (design doc first) | — |
| `nostrc-qp24` | P2 | [epic] Groundhog messaging app (plan exists, zero code) | s2bz, 1v65 |
| `nostrc-s2bz` | P2 | Marmot KeyPackage kind 443 → 30443 | — |
| `nostrc-s94z` | P3 | Commit the four untracked plan docs with `Status:` headers | — |

**Wave 1 dispatch (this session):** `bml6`+`djvs` (keyring, one worker), `1v65`+`2thp` (dispatcher owns all `.desktop` edits, one worker), `tmsc` (publish lib). `da9c`/`s2bz`/`s94z` are independent but held to keep the wave to three disjoint file scopes.
