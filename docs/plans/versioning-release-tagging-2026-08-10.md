# Versioning and Release Tagging: Plan

**Status:** partial — `cmake/VersionHelpers.cmake`, `scripts/tag_release.py`, and per-component tag parsing in `.github/workflows/release.yml` exist; `VERSION_MANIFEST.md` is incomplete (no signet, no libhanami); `packaging/rpm/nostr-login.spec` uses one flat version; `debian/rules` tracks its own ad-hoc scheme.

## Goal
Implement a robust, independent versioning and release tagging system across the nostrc project, ensuring each component (libnostr, libgo, nostr-gobject, gnostr app) can be versioned and tagged independently while maintaining dependency integrity.

## Background
The project currently has fragmented versioning patterns:
- **gnostr app**: Uses CMake `project(VERSION 0.1.0)` propagated to `gnostr-build-info.h`.
- **nostr-gobject**: Uses `.h.in` templates providing public `MAJOR`, `MINOR`, `MICRO` macros.
- **libnostr**: Uses `nostr-config.h.in` for feature flags, but lacks a dedicated public version header.
- **libgo**: Currently lacks explicit versioning constants or files.
- **Current Tagging**: No standardized component-specific tagging convention in place.

## Approach
I will implement a component-centric versioning system using a shared CMake helper module. This ensures that while each piece of the project is versioned independently, the mechanism for doing so remains consistent.

Key strategies:
- **Standardized Helper**: A new `VersionHelpers.cmake` module will provide `declare_component_version` and `apply_versioning` functions to avoid duplication.
- **Consistent Library Pattern**: All libraries will adopt the `MAJOR.MINOR.MICRO` pattern with a generated public version header and a `pkg-config` file.
- **Independent Git Tags**: We will adopt prefixed tagging (e.g., `libnostr-v1.0.0`) to support independent release cycles.
- **Strict Dependency Checks**: Downstream components will declare minimum required versions of their dependencies using `find_package(NAME VERSION REQUIRED CONFIG)`, ensuring build-time safety.

## Work Items
1. [x] **Infrastructure**: Create `cmake/VersionHelpers.cmake` and include it in the root `CMakeLists.txt`.
2. [x] **libnostr Versioning**: Implement version headers and `pkg-config` for `libnostr`.
3. [x] **libgo Versioning**: Implement version headers and `pkg-config` for `libgo`.
4. [x] **Component Standardization**: Migrate `nostr-gobject`, `nostr-gtk`, `libmarmot`, and `marmot-gobject` to use the new `VersionHelpers` module.
5. [x] **App Integration**: Update `gnostr` app to utilize the helper for its versioning.
6. [x] **Release Automation**: Create `scripts/tag_release.py` to automate the creation of component-specific Git tags.
7. [x] **Dependency Locking**: Add minimum version requirements to all downstream `CMakeLists.txt` files.
8. [x] **Verification**: Execute the full CI matrix (Linux, macOS, Windows) and finalize documentation.

## Open Questions
- How should we automate the detection of "breaking changes" to trigger MAJOR bumps (e.g., via Conventional Commits)?
- Should we implement a "version manifest" file for the entire monorepo to track the current released versions of all components in one place?

## References
- [changesets.dev](https://changesets.dev) - Industry standard for monorepo independent versioning.
- [conventionalcommits.org](https://www.conventionalcommits.org) - Standard for automated versioning.
