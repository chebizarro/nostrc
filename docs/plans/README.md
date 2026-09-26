# Plans index

Cross-reference of every plan doc in this directory: what it proposed, its
current status, the primary bead tracking it, and the commit(s) that shipped
it (if any). Statuses follow the convention in
`docs/investigations/gnome-gtk-nostr-planned-not-implemented-2026-09-26.md`
("Preventive Measures"): `plan-only` / `partial` / `landed` /
`superseded-by <ref>` / `in progress`.

| Plan | Status | Primary bead | Shipped commit(s) |
|---|---|---|---|
| [`concord-race-convergence.md`](concord-race-convergence.md) | landed | `nostrc-r874` (closed) | `70219277`, `6160c6af` |
| [`gnome-integration-and-samba-server-2026-09-25.md`](gnome-integration-and-samba-server-2026-09-25.md) | in progress (Wave 6) | `nostrc-rb0e` (bucket D of A/B/C/D: `nostrc-nxpb`/`nostrc-zcll`/`nostrc-ot2c`/`nostrc-rb0e`) | Waves 1-5 landed incrementally (see plan's Execution Index); Wave 6 tail filed as beads, not yet run |
| [`gnostr-rich-media-timeline-2026-08-03.md`](gnostr-rich-media-timeline-2026-08-03.md) | partial | `nostrc-9rs0` (epic, closed) | `8cacaeb3`, `72a103fb`, `5774d4c1`, `03cee1d4`, `7adcec3b`, `7211edbe`, `77ca1372` — but "+N more" overflow affordance has no UI caller (see investigation Cluster 2) |
| [`gnostr-timeline-compositor-definitive-2026-05-20.md`](gnostr-timeline-compositor-definitive-2026-05-20.md) | landed | `nostrc-14d` (follow-on stabilization still in_progress) | `81ae38a6`, `568732a1` |
| [`gnostr-timeline-compositor-full-implementation.md`](gnostr-timeline-compositor-full-implementation.md) | landed | `nostrc-14d` (follow-on stabilization still in_progress) | `568732a1` |
| [`gnostr-timeline-performance-2026-08-06.md`](gnostr-timeline-performance-2026-08-06.md) | landed | `nostrc-1urb` (epic, closed) | `f16f2d56`, `98c95332`, `22d4466f`, `b6271b91`, `900e7bae`, `3a8b2e87` |
| [`groundhog-gnome-messaging-2026-09-25.md`](groundhog-gnome-messaging-2026-09-25.md) | plan-only | `nostrc-qp24` (epic, open) | none — zero code in `gnome/groundhog/` |
| [`nip-29-gnostr-plugin-2026-05-18.md`](nip-29-gnostr-plugin-2026-05-18.md) | partial | `nostrc-rxxx` (upstream conformance audit, open) | `b5362717` — but `ENABLE_NIP29=OFF` by default and several plan items unbuilt (see file header) |
| [`nostr-linux-samba-login-2026-09-19.md`](nostr-linux-samba-login-2026-09-19.md) | superseded-by [`gnome-integration-and-samba-server-2026-09-25.md`](gnome-integration-and-samba-server-2026-09-25.md) | `nostrc-nxpb`/`nostrc-zcll`/`nostrc-ot2c`/`nostrc-rb0e` (buckets A-D), review epic `nostrc-1r5d` | `42355dcd` (buckets A-D initial), `d58fcd47` |
| [`signet-passkeys-fido-2026-07-02.md`](signet-passkeys-fido-2026-07-02.md) | landed | none dedicated (see investigation Cluster 3) | `e08341d6` → `4a4d039c` (8 commits, Phases 0-4) |
| [`versioning-release-tagging-2026-08-10.md`](versioning-release-tagging-2026-08-10.md) | partial | none filed yet | `cmake/VersionHelpers.cmake`, `scripts/tag_release.py`, per-component tag parsing in `.github/workflows/release.yml` landed; `VERSION_MANIFEST.md`/RPM/Debian reconciliation still open (investigation recommendation #11) |

See `docs/investigations/gnome-gtk-nostr-planned-not-implemented-2026-09-26.md`
for the full audit these statuses are drawn from.
