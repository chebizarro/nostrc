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
| libnostr | `libnostr/` | 1.0.9 | Unreleased | — | `libnostr/CMakeLists.txt` |
| libgo | `libgo/` | 0.1.2 | Unreleased | — | `libgo/CMakeLists.txt` |
| nostr-gobject | `nostr-gobject/` | 2.0.2 | Unreleased | — | `nostr-gobject/CMakeLists.txt`, `nostr-gobject/meson.build` |
| nostr-gtk | `nostr-gtk/` | 1.0.1 | Unreleased | — | `nostr-gtk/CMakeLists.txt`, `nostr-gtk/meson.build` |
| libmarmot | `libmarmot/` | 0.4.1 | Unreleased | — | `libmarmot/CMakeLists.txt`, `libmarmot/meson.build` |
| marmot-gobject | `marmot-gobject/` | 1.1.0 | Unreleased | — | `marmot-gobject/CMakeLists.txt`, `marmot-gobject/meson.build` |
| gnostr | `apps/gnostr/` | 0.1.0 | 0.1.0-preview | `gnostr-v0.1.0-preview` | `apps/gnostr/CMakeLists.txt` |
| groundhog | `gnome/groundhog/` | 0.9.1 | Unreleased | — | `gnome/groundhog/CMakeLists.txt` |
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
