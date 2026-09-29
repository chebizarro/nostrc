# Component Version Manifest

This file is the repository-wide ledger for independently releasable components
covered by the versioning initiative. **Declared version** is the version in the
build metadata; it is not evidence that a release exists. **Latest release** is
updated only after the matching component-prefixed Git tag has been published.

As of 2026-08-10, the repository has no component-specific release tags.
Consequently, every component below is currently unreleased even where its build
files already declare a version.

| Component | Path | Declared version | Latest release | Release tag | Authoritative version source(s) |
| --- | --- | --- | --- | --- | --- |
| libnostr | `libnostr/` | 1.0.10 | Unreleased | — | `libnostr/CMakeLists.txt` |
| libgo | `libgo/` | 0.1.2 | Unreleased | — | `libgo/CMakeLists.txt` |
| nostr-gobject | `nostr-gobject/` | 2.0.2 | Unreleased | — | `nostr-gobject/CMakeLists.txt`, `nostr-gobject/meson.build` |
| nostr-gtk | `nostr-gtk/` | 1.0.1 | Unreleased | — | `nostr-gtk/CMakeLists.txt`, `nostr-gtk/meson.build` |
| libmarmot | `libmarmot/` | 0.5.0 | Unreleased | — | `libmarmot/CMakeLists.txt`, `libmarmot/meson.build` |
| marmot-gobject | `marmot-gobject/` | 1.2.0 | Unreleased | — | `marmot-gobject/CMakeLists.txt`, `marmot-gobject/meson.build` |
| gnostr | `apps/gnostr/` | 0.1.0 | 0.1.0-preview | `gnostr-v0.1.0-preview` | `apps/gnostr/CMakeLists.txt` |
| groundhog | `gnome/groundhog/` | 0.10.0 | Unreleased | — | `gnome/groundhog/CMakeLists.txt` |
| nostr-homed | `gnome/nostr-homed/` | 0.2.2 | Unreleased | — | `gnome/nostr-homed/CMakeLists.txt`, `gnome/nostr-homed/meson.build`, `gnome/nostr-homed/nostr-homed.pc.in` |
| NIP-46 client/provider | `nips/nip46/` | Unversioned | Unreleased | — | None; authoritative version ownership must be established before release |
| nip19 (NIP-19 codec) | `nips/nip19/` | 0.1.0 | Unreleased | — | `nips/nip19/CMakeLists.txt` (`declare_component_version`; SONAME `libnip19.so.0`) |
| nip34 (NIP-34 git events) | `nips/nip34/` | 0.1.0 | Unreleased | — | `nips/nip34/CMakeLists.txt` (`declare_component_version`; SONAME `libnip34.so.0`) |
| nip55l (Linux signer) | `nips/nip55l/` | 0.5.1 | Unreleased | — | `nips/nip55l/include/nostr/nip55l/signer_ops.h` (`NOSTR_NIP55L_VERSION_*`) |
| nostr-seal | `gnome/nostr-seal/` | 0.1.0 (format nsealed v1) | Unreleased | — | `gnome/nostr-seal/src/main.c` (`NOSTR_SEAL_VERSION`), `gnome/nostr-seal/include/nostr-seal.h` (`NSEAL_FORMAT_VERSION`) |

## Recorded version decisions

Decisions for components affected by another component's change (AGENTS.md,
"Updating versions", step 5).

| Change | Component | Declared | Decision |
| --- | --- | --- | --- |
| libmarmot 0.3.6 -> 0.4.0 (MINOR: breaking LeafNodeTBS wire change, nostrc-2io4; plus nostrc-lz4f, -va60, -5q55, -8u1k) | libmarmot | 0.4.0 | MINOR bump (0.x breaking wire change; migration notes in `libmarmot/README.md`). |
| same | marmot-gobject | 1.1.0 | No bump: no source, API or ABI change. It links libmarmot statically, so its next (first) 1.1.0 release embeds 0.4.0 and must carry libmarmot's wire-compatibility note. |
| same | gnostr | 0.1.0 | No bump: 0.1.0 is not yet released (only `gnostr-v0.1.0-preview`), so the statically linked libmarmot 0.4.0 and the mls-groups `group-error` signal/toast ship in 0.1.0. Its release notes must say that 0.1.0 cannot follow path Commits from the 0.1.0-preview (libmarmot 0.1.0). |
| same | groundhog | 0.9.0 | MINOR in the same wave for G09 Tor and G20b relay groups; the libmarmot change itself only needs a rebuild. Groundhog links libmarmot statically, but its Marmot app integration is not live (nostrc-qp24.13) and `GhStoreMarmot` stores opaque state, so only a rebuild is needed. |
| libnostr 1.0.8 -> 1.0.9 (PATCH: `nostr_envelope_serialize_compact()` REQ/COUNT frames keep their closing `]`; `event_envelope_marshal_json()` escapes the subscription id; nostrc-ptwq) | libnostr | 1.0.9 | PATCH: bug fix, no API or ABI change. |
| same | groundhog | 0.9.0 | No bump for this change: the libsoup transport now builds its REQ with the library serializer instead of by hand, which puts the same bytes on the wire. |
| groundhog 0.9.0 -> 0.9.1 (PATCH: a build without G09 fails closed on network-mode tor or an unknown mode, and never dials .onion; nostrc-6v0i) | groundhog | 0.9.1 | PATCH: privacy bug fix (charter P5). The new `tor-unavailable` status input, banner and Preferences note are app-internal, not a public surface. |
| libmarmot 0.4.0 -> 0.4.1 (PATCH: Welcome GroupSecrets.path_secret sent and applied, nostrc-il4i) | libmarmot | 0.4.1 | PATCH bump: RFC 9420 conformance fix, no API/ABI or state-format change. Wire-compatible both ways: 0.4.0 joiners ignore the new `path_secret` (and keep the old inability to follow some Commits); 0.4.1 joiners accept 0.4.0 Welcomes without it. |
| same | marmot-gobject | 1.1.0 | No bump: no source, API or ABI change; rebuild picks up the fix. |
| same | gnostr, groundhog | 0.1.0, 0.9.1 | No bump: rebuild only (static link, no API change). |
| libgo 0.1.1 -> 0.1.2, libnostr 1.0.9 -> 1.0.10 (PATCH: MPMC channel slots no longer lose an element claimed but not yet filled, capacity-1 rings get two slots; libnostr re-arms a WebSocket write that raced the pending flag; nostrc-75rv) | libgo, libnostr | 0.1.2, 1.0.10 | PATCH: data-loss bug fixes, no API/ABI change. libnostr takes 1.0.10 because nostrc-ptwq already claimed 1.0.9. |
| groundhog 0.9.1 -> 0.10.0 (MINOR: NIP-17 multi-recipient send, nostrc-qp24.78; encrypted attachments core with receive, cache binding and purge, G21 nostrc-qp24.39; absorbs 0.9.1) | groundhog | 0.10.0 | MINOR: new user-visible capability (group DMs). 0.9.1 was never pushed on its own. |
| libmarmot 0.4.1 -> 0.5.0 (MINOR: `marmot_update_group_metadata` gains `out_commit_json`; kind:445 Commits NIP-44-encrypted and ingested by `marmot_process_message`, nostrc-9ata) | libmarmot | 0.5.0 | MINOR bump: 0.x breaking API change (new required parameter) plus a new capability and a wire change of Commit events; migration notes in `libmarmot/README.md`. |
| same | marmot-gobject | 1.2.0 | MINOR bump: new `marmot_gobject_client_update_group_metadata_async/_finish` and `MarmotGobjectClient::group-updated` signal (backward compatible). |
| same | gnostr | 0.1.0 | No bump: 0.1.0 is unreleased. The mls-groups plugin gains an admin-only group rename that publishes its Commit to the group relays, and refreshes views from the client's `group-updated` (the router's post-Commit lookup used the nostr_group_id as an MLS group id and never found the group). |
| same | groundhog | 0.10.0 | No bump beyond 0.10.0 (unreleased). GhStoreMarmot snapshots now also cover the `mls_group_parent` label; test callers follow the new signature. |
| libmarmot 0.5.0 review fixes (W17 review B1, B2, N2, N3, N5; still unreleased 0.5.0) | libmarmot | 0.5.0 | No further bump: folded into the unreleased 0.5.0 MINOR. Adds `marmot_clear_pending_commit()`; producers now leave a pending Commit that `marmot_merge_pending_commit()` applies after a relay OK; every kind:445 (`marmot_create_message()` too) is signed by a fresh ephemeral key; multi-member add/remove/create are one Commit (nostrc-wc6v); new `mls_kv` label `mls_group_pending`. Migration notes in `libmarmot/README.md`. |
| same | marmot-gobject | 1.2.0 | No further bump: folded into unreleased 1.2.0 (adds `merge_pending_commit_async/_finish`, `clear_pending_commit_async/_finish`; `update_group_metadata` no longer emits ::group-updated before the merge). |
| same | gnostr | 0.1.0 | No bump (unreleased): add-member and rename Commits are merged only after a group relay's OK and cleared otherwise, with the outcome shown in the view. |
| same | groundhog | 0.10.0 | No bump (unreleased): GhStoreMarmot snapshots also cover `mls_group_pending`; tests merge their Commits. |
| libmarmot 0.5.0 re-review fixes (W17b R1, R2; still unreleased 0.5.0) | libmarmot | 0.5.0 | No further bump: folded into unreleased 0.5.0. Pending record v2 bound to its parent state and carrying the signed event and Welcomes; merge on our own relay echo; idempotent merge; new `marmot_get_pending_commit()`, `marmot_get_unsent_welcomes()`, `marmot_mark_welcomes_sent()`, `MarmotUnsentWelcome`; new `mls_kv` label `mls_group_welcomes`. |
| same | marmot-gobject | 1.2.0 | No further bump (unreleased 1.2.0): adds `get_pending_commit`, `get_unsent_welcomes`, `mark_welcomes_sent`. |
| same | gnostr | 0.1.0 | No bump (unreleased): pending Commits resolved by a plugin-level resolver (publish until a relay OK, clear only when every relay refused, keep and retry when uncertain, resolve leftovers and unsent Welcomes at startup). |
| same | groundhog | 0.10.0 | No bump (unreleased): GhStoreMarmot snapshots also cover `mls_group_welcomes`. |
| libmarmot 0.5.0 addendum fixes (W17b C1, C2, N1; still unreleased 0.5.0) | libmarmot | 0.5.0 | No further bump: folded into unreleased 0.5.0. Documented that a `Marmot` is not thread-safe; Welcome outbox append-only with stable ids (`MarmotUnsentWelcome.id`), `marmot_mark_welcomes_sent()` now takes the ids to remove (outbox record v2); a duplicate Welcome for a group already joined is refused (`MARMOT_ERR_WELCOME_ALREADY_ACCEPTED`). |
| same | marmot-gobject | 1.2.0 | No further bump (unreleased 1.2.0): every libmarmot call serialized per client; new `marmot_gobject_client_lock/unlock`; `get_unsent_welcomes` returns ids and `mark_welcomes_sent` takes them. |
| same | gnostr | 0.1.0 | No bump (unreleased): direct libmarmot calls hold the client lock; each Welcome is marked sent after its own send; resolver coalesces per group, backs off exponentially with jitter, stops on deactivation, and flushes the outbox on every group update. |
| W17b addendum 2 (D1 and low items; still unreleased) | marmot-gobject | 1.2.0 | No further bump: client signals are posted as idle sources to the context captured at construction instead of `g_main_context_invoke()` from workers holding the lock (no API change; documented on `marmot_gobject_client_new()`). |
| same | libmarmot | 0.5.0 | No further bump: an uncomputable Welcome id fails the outbox operation (`MARMOT_ERR_MEMORY`) instead of using an all-zero id. |
| same | gnostr | 0.1.0 | No bump (unreleased): failed Welcome sends are retried on their own backoff timer, cancelled on deactivation. |

## Maintenance

Follow the versioning policy and update procedure in `AGENTS.md`. In
particular:

- Change a component's declared version here in the same commit as all of its
  authoritative build sources.
- Keep **Latest release** and **Release tag** unchanged for unreleased work.
- When publishing a release, use the tag
  `<component>-v<MAJOR>.<MINOR>.<PATCH>[-<PRERELEASE>]` (for example,
  `libnostr-v1.2.3` or `gnostr-v0.1.0-preview`) and then record that exact
  release version and tag here. Prereleases retain the base declared version
  in their authoritative build sources.
- Before publishing, inspect existing tags with
  `git tag --list '<component>-v*' --sort=-version:refname` and verify every
  listed source agrees with **Declared version**.
- A component marked **Unversioned** must gain an authoritative SemVer source
  before its first release.

## Automation status

Manifest maintenance is currently manual: component versions are fragmented
between build systems, and two components have no explicit version source. The
planned `scripts/tag_release.py` automation must treat this manifest as the
release ledger, reject mismatches between the manifest and authoritative
sources, and update the release columns when it creates a tag.
