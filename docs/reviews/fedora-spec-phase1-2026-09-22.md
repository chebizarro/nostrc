# Fedora .spec — Phase 1 headless nostr-login stack (evidence)

**Date:** 2026-09-22 · **Beads:** nostrc-rb0e.3 (D2), nostrc-rb0e.10 (D9)
**Reconstructed** after a worktree-cleanup mishap discarded the original doc; the
spec (`packaging/rpm/nostr-login.spec`) + `rpmlint.toml` were recovered from the
guest rpmbuild tree and re-validated. Content reflects the validated build.

## Deliverables
- `packaging/rpm/nostr-login.spec` (629 lines) — one `nostrc` SRPM → 12 headless
  binary RPMs mirroring the 13 Debian debs (`libnostr1`/`-dev` collapse to
  `libnostr`/`-devel` per Fedora convention). Placement mirrors the top-level
  `debian/`. The existing `apps/gnostr-signer/packaging/rpm/*.spec` is left alone
  (Phase 4).
- `packaging/rpm/rpmlint.toml` — suppression filter, each entry justified inline.
- `packaging/rpm/README.md` — operator `mock` build guide + gate scripts.

## Build method
`mock` is not shippable on the Ubuntu lab host, so validation used a real Fedora 41
chroot via `podman run --rm fedora:41` (functionally what mock's inner build does,
minus network isolation). `rpmbuild -bb` produced all 23 RPMs (12 binary + 11
debuginfo). The operator `mock -r fedora-41-x86_64 --rebuild` path is documented in
`packaging/rpm/README.md`.

## Gates (Fedora 41 x86_64)
- **rpmlint spec:** 0 errors (21 cosmetic `macro-in-comment` warnings).
- **rpmlint RPMs:** 0 errors, 0 warnings across all 23 packages.
- **Dependency purity:** 10/10 nostr-login packages have no
  `gtk|adwaita|gvfs|gnome|gstreamer|fuse|libsecret|libpeas|webkit|glib|gobject|gio`
  in their Requires closure. `libnostr` links only OpenSSL, secp256k1, jansson,
  libwebsockets, nsync, libgcc — the D-14 headless ABI.
- **Content:** NSS at `%{_libdir}/libnss_nostr.so.2`; PAM at
  `%{_libdir}/security/pam_nostr.so`; unit at `%{_unitdir}/nostr-authd.service`;
  seeder at `%{_sbindir}/nostr-homed-seed`; three `%config(noreplace)` conffiles
  (`/etc/nss_nostr.conf`, `/etc/pam.d/nostr-login`, `/etc/nostr-auth/auth.conf`);
  authselect drop-in at `%{_datadir}/nostr-homed/authselect/`.

## Fedora-specific decisions honoured
- Always `%{_libdir}` (never literal `/usr/lib64`).
- `%global __provides_exclude_from` for the NSS + PAM plugins (no bogus SONAME Provides).
- SRPM `nostrc` with no `%files` for the main package; only subpackages ship.
- `BuildRequires:` at top level (before any `%package -n`) — spec-parser rule.
- Fedora activation is **authselect**, not pam-auth-update.
- systemd unit shipped **disabled** (`%systemd_post` respects default-disabled preset).

## Known gap (corrects packaging-plan D-1)
`nsync-devel` is **not** in Fedora 41/42 stock repos as of 2026-09-22 (D-1 assumed
it was). Validation built nsync from upstream in the chroot. A real Fedora build
needs a COPR overlay or a vendored `third_party/nsync/`. Filed as a follow-up;
revisit D-1 toward vendoring nsync in-tree (de-risks Debian/other distros too).
