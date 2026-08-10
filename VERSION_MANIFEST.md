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
| libnostr | `libnostr/` | Unversioned | Unreleased | — | Not yet established; planned for `libnostr/CMakeLists.txt` |
| libgo | `libgo/` | Unversioned | Unreleased | — | Not yet established; planned for `libgo/CMakeLists.txt` |
| nostr-gobject | `nostr-gobject/` | 1.0.0 | Unreleased | — | `nostr-gobject/CMakeLists.txt`, `nostr-gobject/meson.build` |
| nostr-gtk | `nostr-gtk/` | 1.0.0 | Unreleased | — | `nostr-gtk/CMakeLists.txt`, `nostr-gtk/meson.build` |
| libmarmot | `libmarmot/` | 0.1.0 | Unreleased | — | `libmarmot/CMakeLists.txt`, `libmarmot/meson.build` |
| marmot-gobject | `marmot-gobject/` | 1.0.0 | Unreleased | — | `marmot-gobject/CMakeLists.txt`, `marmot-gobject/meson.build` |
| gnostr | `apps/gnostr/` | 0.1.0 | Unreleased | — | `apps/gnostr/CMakeLists.txt` |

## Maintenance

Follow the versioning policy and update procedure in `AGENTS.md`. In
particular:

- Change a component's declared version here in the same commit as all of its
  authoritative build sources.
- Keep **Latest release** and **Release tag** unchanged for unreleased work.
- When publishing a release, use the tag
  `<component>-v<MAJOR>.<MINOR>.<PATCH>` (for example,
  `libnostr-v1.2.3`) and then record that exact version and tag here.
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
