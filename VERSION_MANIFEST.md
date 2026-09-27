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
| libnostr | `libnostr/` | 1.0.0 | Unreleased | — | `libnostr/CMakeLists.txt` |
| libgo | `libgo/` | 0.1.1 | Unreleased | — | `libgo/CMakeLists.txt` |
| nostr-gobject | `nostr-gobject/` | 2.0.0 | Unreleased | — | `nostr-gobject/CMakeLists.txt`, `nostr-gobject/meson.build` |
| nostr-gtk | `nostr-gtk/` | 1.0.1 | Unreleased | — | `nostr-gtk/CMakeLists.txt`, `nostr-gtk/meson.build` |
| libmarmot | `libmarmot/` | 0.3.1 | Unreleased | — | `libmarmot/CMakeLists.txt`, `libmarmot/meson.build` |
| marmot-gobject | `marmot-gobject/` | 1.1.0 | Unreleased | — | `marmot-gobject/CMakeLists.txt`, `marmot-gobject/meson.build` |
| gnostr | `apps/gnostr/` | 0.1.0 | 0.1.0-preview | `gnostr-v0.1.0-preview` | `apps/gnostr/CMakeLists.txt` |
| groundhog | `gnome/groundhog/` | 0.1.0 | Unreleased | — | `gnome/groundhog/CMakeLists.txt` |
| nostr-homed | `gnome/nostr-homed/` | 0.2.2 | Unreleased | — | `gnome/nostr-homed/CMakeLists.txt`, `gnome/nostr-homed/meson.build`, `gnome/nostr-homed/nostr-homed.pc.in` |
| NIP-46 client/provider | `nips/nip46/` | Unversioned | Unreleased | — | None; authoritative version ownership must be established before release |
| nip19 (NIP-19 codec) | `nips/nip19/` | 0.1.0 | Unreleased | — | `nips/nip19/CMakeLists.txt` (`declare_component_version`; SONAME `libnip19.so.0`) |
| nip34 (NIP-34 git events) | `nips/nip34/` | 0.1.0 | Unreleased | — | `nips/nip34/CMakeLists.txt` (`declare_component_version`; SONAME `libnip34.so.0`) |
| nip55l (Linux signer) | `nips/nip55l/` | 0.5.0 | Unreleased | — | `nips/nip55l/include/nostr/nip55l/signer_ops.h` (`NOSTR_NIP55L_VERSION_*`) |
| nostr-seal | `gnome/nostr-seal/` | 0.1.0 (format nsealed v1) | Unreleased | — | `gnome/nostr-seal/src/main.c` (`NOSTR_SEAL_VERSION`), `gnome/nostr-seal/include/nostr-seal.h` (`NSEAL_FORMAT_VERSION`) |

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
