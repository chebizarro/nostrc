# Fedora / RPM packaging — headless `nostr-login` stack

`nostr-login.spec` is a single `nostrc` SRPM that produces the Phase-1 headless
login-stack RPMs (GLib-free, D-14 headless ABI), mirroring the Debian set:

`libnostr`/`-devel`, `libnostrgo`/`-devel`, `libnostr-json`/`-devel`,
`nss-nostr`, `pam-nostr`, `nostr-authd`, `nostr-homectl`, `nostr-homed-smb`,
`nostr-homed-domain`, and the `nostr-login` metapackage.

## Build (operator, on a Fedora host)

```bash
# from a checkout at the tagged version:
V=0.3.0
git archive --prefix=nostrc-$V/ -o ~/rpmbuild/SOURCES/nostrc-$V.tar.gz HEAD
mock -r fedora-41-x86_64 --buildsrpm --spec packaging/rpm/nostr-login.spec \
     --sources ~/rpmbuild/SOURCES
mock -r fedora-41-x86_64 --rebuild /var/lib/mock/fedora-41-x86_64/result/nostrc-*.src.rpm
```

On a non-Fedora host without `mock`, a real Fedora chroot works for validation:
`podman run --rm -v <rpmbuild>:/rpmbuild:Z fedora:41 rpmbuild -bb /rpmbuild/SPECS/nostr-login.spec`.

## Gates

```bash
# rpmlint (config filters justified suppressions)
rpmlint --file packaging/rpm/rpmlint.toml packaging/rpm/nostr-login.spec RPMS/**/*.rpm

# dependency purity (headless: no desktop/GLib deps)
for r in RPMS/**/*.rpm; do rpm -qp --requires "$r"; done | \
  grep -Ei 'gtk|adwaita|gvfs|gnome|gstreamer|fuse|libsecret|libpeas|webkit|glib|gobject|gio' \
  && echo "IMPURE" || echo "PURE"

# content check
rpm -qlp RPMS/**/nss-nostr-*.rpm   # -> %{_libdir}/libnss_nostr.so.2
rpm -qlp RPMS/**/pam-nostr-*.rpm   # -> %{_libdir}/security/pam_nostr.so + authselect drop-in
```

## PAM activation (Fedora uses authselect, not pam-auth-update)

`pam-nostr` ships an authselect drop-in under `%{_datadir}/nostr-homed/authselect/`.
Enable/disable per the fragments there (`nostr-auth.snippet` / `nostr-account.snippet`);
the broker unit is shipped **disabled** (an unseeded broker has no accounts).

## Known dependency gap

`nsync-devel` is **not** in Fedora 41/42 stock repos (as of 2026-09-22). Until it
lands, either publish `nsync` to a COPR overlay or vendor it in-tree
(`third_party/nsync/`) — see beads. The spec keeps `BuildRequires: nsync-devel`.
