# nostr-login.spec — Fedora RPM spec for the headless nostr-login stack.
#
# Phase 1 (nostrc-rb0e.3 / D2, nostrc-rb0e.10 / D9): build ONE SRPM (nostr-login)
# from the nostrc monorepo tarball and produce the same headless login stack
# shipped by the Debian side of Phase 1 (merged 3e3ae0da).  This is the
# Fedora mirror of top-level debian/ — decisions D-2, D-3, D-4, D-6, D-8, D-9,
# D-11, D-14 apply verbatim.  See docs/designs/packaging-plan-debian-fedora.md.
#
# Phase 5 (bead nostrc-h10m.2): additive portable-home stack — libhanami +
# nostr-home-sync (nostr-home-syncd + user unit + fetch helper + sysusers
# snippet) + nostr-home-fuse (FUSE overlay + user unit + ignore.d snippet).
# Gated behind NOSTR_HOMED_ENABLE_PORTHOME_* — the headless nostr-login
# closure is UNCHANGED (dep-purity gate stays green).  See design docs
# home-from-relay.md §7.2 and nostrfs-porthome-overlay.md §D9.
#
# Build:
#   1. Build a source tarball whose top directory is nostrc-%{version}/:
#        git archive --format=tar.gz --prefix=nostrc-0.3.0/ \
#            -o ~/rpmbuild/SOURCES/nostrc-0.3.0.tar.gz HEAD
#      (or use a `tar czf ... nostrc-0.3.0/` on a clean checkout).
#   2. Preferred (real Fedora chroot on a Fedora host):
#        rpmbuild -bs packaging/rpm/nostr-login.spec
#        mock -r fedora-41-x86_64 --rebuild \
#            ~/rpmbuild/SRPMS/nostr-login-0.3.0-1.*.src.rpm
#   3. Non-Fedora fallback (Ubuntu 24.04 buildbox — no Fedora chroot):
#        rpmbuild -bb packaging/rpm/nostr-login.spec
#      Layout is still Fedora-correct (%{_libdir}=/usr/lib64, %{_prefix}=/usr);
#      only the auto-generated Requires closure reflects the buildbox libc.
#
# See packaging/rpm/README.md for the full mock command, dependency-purity
# check (rpm -qp --requires) and content check (rpm -qlp).

# --- Global macros -----------------------------------------------------------

# Do NOT auto-generate rpm Provides for the NSS/PAM loadable modules.  glibc
# dlopens libnss_nostr.so.2 by fixed name; libpam dlopens pam_nostr.so from
# %{_libdir}/security/.  Neither is a public shared library, so we suppress
# the SONAME-derived Provides ("libnss_nostr.so.2()(64bit)" / "pam_nostr.so()...")
# that would otherwise let external packages accidentally satisfy a Requires
# on those file names.  Auto-Requires still run so libnostr/libpam/libc etc.
# still land in the plugin's Requires closure (P5 of the packaging plan).
%global __provides_exclude_from ^(%{_libdir}/(security/pam_nostr\\.so|libnss_nostr\\.so\\.2))$

# --- Package identity --------------------------------------------------------
#
# SRPM Name is `nostrc` (matches Debian's `Source: nostrc` in debian/control
# and the tarball prefix nostrc-<version>/).  There is deliberately NO %files
# section for the main `nostrc` package -- every deliverable is a %package -n
# subpackage below.  rpmbuild treats a main spec without %files as "skip the
# main binary RPM" (only the SRPM header is used), which avoids the
# `E: no-binary` rpmlint error a truly empty arch-tagged main package would
# raise.  The nostr-login metapackage is a proper noarch %package -n stanza.

Name:           nostrc
Version:        0.4.0
Release:        1%{?dist}
Summary:        Nostr protocol monorepo (headless login stack build)
License:        MIT
URL:            https://github.com/chebizarro/nostrc
Source0:        %{url}/archive/refs/tags/v%{version}/nostrc-%{version}.tar.gz

# --- BuildRequires (top-level: apply to the whole SRPM) ----------------------
#
# These MUST come before any %package -n stanza, otherwise the RPM parser
# attributes them to the last %package (rpmlint then emits repeated
# `tag-in-description BuildRequires:` for that subpackage's description).

BuildRequires:  cmake >= 3.22
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  pkgconfig
BuildRequires:  python3
BuildRequires:  chrpath

BuildRequires:  pkgconfig(openssl) >= 3.0
BuildRequires:  pkgconfig(libsecp256k1)
BuildRequires:  pkgconfig(jansson)
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  pkgconfig(libwebsockets)
BuildRequires:  pkgconfig(libsodium)
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(libgit2)
BuildRequires:  pkgconfig(fuse3)
BuildRequires:  pkgconfig(libsystemd)
BuildRequires:  pam-devel
# Wave 4 (#22b): GLib + libsoup + libxml2 + json-glib are needed by
# nostr-dav (localhost DAV bridge) and nostr-notify-daemon (GNotification
# via gio-2.0). libsecret is optional for the DAV keyring mirror.
BuildRequires:  pkgconfig(glib-2.0) >= 2.60
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gobject-2.0)
BuildRequires:  pkgconfig(libsoup-3.0)
BuildRequires:  pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(libsecret-1)
# nsync is vendored in-tree at third_party/nsync (git submodule pinned to a
# release tag) and built statically as part of the CMake configure step.
# The vendored copy is folded into libnostrgo.so via --whole-archive, so
# no runtime nsync-devel/libnsync package is required and no DT_NEEDED
# libnsync.so entry is emitted.  See cmake/FindOrVendorNsync.cmake and the
# tracked issue nostrc-dd5y.
#
# Opt back into a system nsync (COPR overlay etc.) by passing
# `--with system_nsync` to rpmbuild / mock and adding
# -DNOSTR_USE_SYSTEM_NSYNC=ON to the CMake invocation below.  Left off by
# default so Fedora 41/42 (which do not ship nsync-devel) build natively
# without a third-party overlay enabled.
# (Macro names in this comment are intentionally NOT prefixed with a percent
# sign so rpm does not expand them during parse; see %%bcond_with and %%cmake
# in the rpm docs.)
%bcond_with system_nsync
%if %{with system_nsync}
BuildRequires:  nsync-devel
%endif
BuildRequires:  systemd-rpm-macros

%description
nostrc is the Nostr protocol monorepo (libnostr, libnostrgo, libnostr-json,
the NIP catalog, nostr-homed's headless login stack, and the GNOME desktop
apps).  This SRPM builds the Phase-1 headless nostr-login stack (D-14 GLib-
free ABI) and ships it as a set of subpackages: libnostr, libnostr-devel,
libnostrgo, libnostrgo-devel, libnostr-json, libnostr-json-devel, nss-nostr,
pam-nostr, nostr-authd, nostr-homectl, nostr-homed-smb, nostr-homed-domain,
and the noarch metapackage nostr-login.  See
docs/designs/packaging-plan-debian-fedora.md for the phased rollout.

# --- Sub-package: nostr-login (noarch metapackage) ---------------------------
%package -n nostr-login
Summary:        Headless Nostr login stack (metapackage)
BuildArch:      noarch
# The metapackage is noarch; its Requires target the arch-specific subpackages
# via %{?_isa} (which resolves to `(x86-64)` on x86_64 etc.).  This is the
# direct analogue of Debian's `Package: nostr-login  Architecture: all` in
# debian/control.  Row 12 of §9 (packaging plan).
Requires:       nss-nostr%{?_isa}     = %{version}-%{release}
Requires:       pam-nostr%{?_isa}     = %{version}-%{release}
Requires:       nostr-authd%{?_isa}   = %{version}-%{release}
Requires:       nostr-homectl%{?_isa} = %{version}-%{release}
Recommends:     nostr-homed-smb%{?_isa} = %{version}-%{release}
Suggests:       nostr-homed-domain      = %{version}-%{release}

%description -n nostr-login
The nostr-login metapackage pulls the four components that make up a working
nostr login stack on a headless server: the NSS projection (nss-nostr), the
broker-backed PAM module (pam-nostr), the auth broker daemon (nostr-authd)
and the identity-authority admin CLI (nostr-homectl).

The dependency closure is deliberately headless (decision D-14 in the
packaging plan): no GTK / GNOME / gvfs / libsecret / libpeas / webkit /
gstreamer / FUSE, and no GLib.

# --- Sub-package: libnostr (SONAME .so.1) ------------------------------------
%package -n libnostr
Summary:        Nostr protocol implementation for C (headless ABI)

%description -n libnostr
libnostr is a C implementation of the Nostr protocol: event/tag encoding,
signing/verification (secp256k1), transport (libwebsockets) and helper
types.  Ships the shared library libnostr.so.1 built with the GLib-free
(headless) ABI (NOSTR_WITH_GLIB=OFF, decision D-14) so servers install the
nostr-login stack with no GLib/GTK/GNOME dependency.

%package -n libnostr-devel
Summary:        Development files for libnostr
Requires:       libnostr%{?_isa}         = %{version}-%{release}
Requires:       libnostr-json-devel%{?_isa} = %{version}-%{release}
Requires:       libnostrgo-devel%{?_isa} = %{version}-%{release}
Requires:       openssl-devel
Requires:       libsecp256k1-devel
Requires:       libwebsockets-devel
%if %{with system_nsync}
Requires:       nsync-devel
%endif

%description -n libnostr-devel
Headers under %{_includedir}/nostr/ and the nostr.pc pkg-config metadata
for the GLib-free (headless) libnostr ABI.

# --- Sub-package: libnostrgo (SONAME .so.0) ----------------------------------
%package -n libnostrgo
Summary:        Go-inspired concurrency primitives for C

%description -n libnostrgo
libnostrgo provides Go-inspired concurrency primitives (channels, wait
groups, tickers, contexts, refcounts, error propagation) used by libnostr
and the nostrc runtime.  The library was renamed from `libgo` (which
collides with gccgo's runtime) as decision D-3; the on-disk soname is
libnostrgo.so.0.

%package -n libnostrgo-devel
Summary:        Development files for libnostrgo
Requires:       libnostrgo%{?_isa} = %{version}-%{release}
%if %{with system_nsync}
Requires:       nsync-devel
%endif

%description -n libnostrgo-devel
Headers under %{_includedir}/ (go.h and closure) and the libnostrgo.pc
pkg-config metadata (with a libgo.pc compatibility symlink for one
release, decision D-3).

# --- Sub-package: libnostr-json (SONAME .so.1) -------------------------------
%package -n libnostr-json
Summary:        Nostr JSON interop layer (jansson wrappers for libnostr)

%description -n libnostr-json
libnostr-json is the jansson-backed JSON interop layer for libnostr
(encoding/decoding events, filters and query strings).  Renamed from
`nostr_json` per decision D-4; a compatibility symlink libnostr_json.so
is shipped alongside for one release.

%package -n libnostr-json-devel
Summary:        Development files for libnostr-json
Requires:       libnostr-json%{?_isa} = %{version}-%{release}
Requires:       jansson-devel

%description -n libnostr-json-devel
Headers and pkg-config metadata for libnostr-json (libnostr-json.pc,
with a nostr_json.pc compatibility symlink for one release).

# --- Sub-package: nss-nostr --------------------------------------------------
%package -n nss-nostr
Summary:        NSS module projecting the nostr identity authority
Requires:       libnostr%{?_isa} = %{version}-%{release}

%description -n nss-nostr
Installs the read-only glibc NSS module %{_libdir}/libnss_nostr.so.2 which
projects nostr-authd's identity authority (a SQLite projection written
atomically by the broker) into passwd/group lookups.  Admins wire it into
/etc/nsswitch.conf (Fedora typically via authselect); the module reads its
configuration from /etc/nss_nostr.conf.

Install nostr-login for the full broker + PAM + NSS login stack.

# --- Sub-package: pam-nostr --------------------------------------------------
%package -n pam-nostr
Summary:        PAM module authenticating against the nostr auth broker
Requires:       libnostr%{?_isa}     = %{version}-%{release}
Requires:       nostr-authd%{?_isa}  = %{version}-%{release}
Requires:       pam
Recommends:     nss-nostr%{?_isa}    = %{version}-%{release}
Recommends:     authselect

%description -n pam-nostr
Installs pam_nostr.so, the broker-backed PAM authenticator that consults
nostr-authd via /run/nostr-auth/auth.sock.  Semantics are the strict-deny
posture (beads nostrc-o1ho): known nostr accounts with a failed proof are
refused (default=die), unknown accounts fall through to Unix auth
(user_unknown=ignore), and a stopped broker never bricks local login
(authinfo_unavail=ignore).

On Fedora, activation goes through **authselect**, not pam-auth-update.
See /usr/share/doc/pam-nostr/README.authselect.md for the authselect
custom-profile drop-in and the manual /etc/pam.d editing recipe.  Also
ships /etc/pam.d/nostr-login as a config(noreplace) proof service for
pamtester(1) so an admin can validate the module against the running
broker without touching system-auth.

# --- Sub-package: nostr-authd ------------------------------------------------
%package -n nostr-authd
Summary:        Nostr authentication broker daemon
Requires:       libnostr%{?_isa} = %{version}-%{release}
%{?systemd_requires}

%description -n nostr-authd
nostr-authd is the SOCK_SEQPACKET broker that mediates authentication for
the nostr-homed login stack.  It vends CHECK_ACCOUNT / BEGIN_LOGIN /
SUBMIT_PROOF over the privileged /run/nostr-auth/auth.sock, backs a
local-vault provider (encrypted authority.db) and a NIP-46 external
signer provider, and re-publishes a read-only NSS projection consumed by
libnss_nostr.so.2.

Ships a hardened systemd unit
(RuntimeDirectory=nostr-auth/0711, StateDirectory=nostr-auth/0700,
ProtectSystem=strict, RestrictAddressFamilies=AF_UNIX,
MemoryDenyWriteExecute=yes) and the /etc/nostr-auth/auth.conf conffile.
The unit is NOT auto-enabled by systemd_post -- an unseeded broker has
no accounts and admins must run `nostr-homed-seed` before the first
`systemctl start nostr-authd`.

# --- Sub-package: nostr-homectl ----------------------------------------------
%package -n nostr-homectl
Summary:        Headless identity-authority admin CLI for nostr-homed
Requires:       libnostr%{?_isa}    = %{version}-%{release}
Requires:       nostr-authd%{?_isa} = %{version}-%{release}

%description -n nostr-homectl
Headless (GLib-free / FUSE-free) identity-authority admin CLI
(NOSTR_HOMED_ENABLE_CTL, decision D-11).  Queries and mutates the
authority store owned by nostr-authd -- listing accounts, exporting the
read-only NSS projection, and driving disable/enable transitions.

Also ships nostr-homed-seed (the provisioning tool renamed from
nh-seed-authority) in %{_sbindir}, which mints the encrypted vault and
the NSS projection for new accounts.

# --- Sub-package: nostr-homed-smb --------------------------------------------
%package -n nostr-homed-smb
Summary:        SMB credential acquisition tools for nostr-homed
Requires:       libnostr%{?_isa}    = %{version}-%{release}
Requires:       nostr-authd%{?_isa} = %{version}-%{release}
Requires:       cifs-utils
Recommends:     samba-common-tools

%description -n nostr-homed-smb
nostr-smb-acquire drives BEGIN_SMB_PROOF against nostr-authd's
/run/nostr-auth/user.sock and delivers the minted, short-lived SMB
password as a Samba credentials= file (mode 0600) or, with --stdout, to
a one-shot pipe.  nostr-smb-mount is the shell wrapper that consumes
that credentials file and mounts the share via mount.cifs (default) or
gvfs (--mode gvfs).

Install this together with nostr-login for a headless server that
authenticates SMB shares through the nostr broker.

# --- Sub-package: nostr-homed-domain (noarch) --------------------------------
%package -n nostr-homed-domain
Summary:        Inert winbind / Samba AD domain samples for nostr-homed
BuildArch:      noarch
Requires:       python3

%description -n nostr-homed-domain
Ships the winbind and Samba AD configuration samples used by nostr-homed
to document a domain-joined deployment, plus the
validate_domain_profile.py profile validator.  Nothing in this package
activates PAM or winbind -- it is inert by design and is safe to install
on a stock host.

# --- Portable-home Phase 5 (bead nostrc-h10m.2) --------------------------------
#
# Additive subpackages for the experimental portable-home stack matching the
# split in docs/designs/home-from-relay.md §7.2 and
# docs/designs/nostrfs-porthome-overlay.md §D9 (§D16 packaging decision).
# The base nostr-login stack (headless, D-14) is UNCHANGED — these packages
# only pull in when the operator opts in, and libhanami is factored out so
# both the sync daemon and the FUSE overlay share it.

# --- Sub-package: libhanami --------------------------------------------------
%package -n libhanami
Summary:        Blossom / git / SQLite helper library (nostrc portable-home)

%description -n libhanami
libhanami is the shared substrate the nostrc portable-home stack links
against: the Blossom BUD-01/02/04 client (HTTP via libcurl), a
Merkle-friendly SQLite index, and small git/libgit2 helpers.  Ships
libhanami.so.0.  Consumed only by nostr-home-sync and nostr-home-fuse;
no public API commitment yet.

# --- Sub-package: nostr-home-sync --------------------------------------------
%package -n nostr-home-sync
Summary:        Portable-home sync daemon for nostr-homed (EXPERIMENTAL)
Requires:       libhanami%{?_isa}  = %{version}-%{release}
Requires:       libnostr%{?_isa}   = %{version}-%{release}
Requires:       nostr-authd%{?_isa} = %{version}-%{release}
Recommends:     libnotify
%{?systemd_requires}

%description -n nostr-home-sync
nostr-home-syncd is the user-scoped sync daemon that materializes, tracks
and re-uploads a portable $HOME snapshot to Blossom under kind-30078 relay
pointers (design docs/designs/home-from-relay.md §6).  It runs unprivileged
as the login user under nostr-home-sync.service (systemd --user).

Also ships the unprivileged fetch helper
%{_libexecdir}/nostr-homed/nostr-home-fetch that nostr-authd's
PROVISION_HOME job forks + drop-privs execs, and the sysusers.d(5) snippet
that provisions the dedicated `nostr-home-fetch` account used by the
sandbox (docs/designs/home-from-relay.md §8.2).

EXPERIMENTAL: the wire format is versioned but not yet frozen; the daemon
carries the NH_PORTHOME_EXPERIMENTAL / NH_SYNCD_EXPERIMENTAL compile-time
flags and every public artefact is documented as such.

# --- Sub-package: nostr-home-fuse --------------------------------------------
%package -n nostr-home-fuse
Summary:        Read-only portable-home FUSE overlay for nostr-homed (EXPERIMENTAL)
Requires:       libhanami%{?_isa}  = %{version}-%{release}
Requires:       libnostr%{?_isa}   = %{version}-%{release}
Requires:       fuse3
Recommends:     nostr-home-sync%{?_isa} = %{version}-%{release}
%{?systemd_requires}

%description -n nostr-home-fuse
nostr-home-fuse mounts the current portable-home snapshot as a read-only
view under $HOME/Portable, streaming cold subtrees from Blossom on demand
(design docs/designs/nostrfs-porthome-overlay.md).  The mount is torn down
at logout via PartOf=graphical-session.target.

Depends on fuse3 for the setuid fusermount3 helper that performs the mount;
the daemon itself runs as the login user and holds no privileges beyond
the FUSE session.  Requires a running nostr-home-syncd to produce
snapshot.json; Recommends the companion package for that reason.

EXPERIMENTAL: gated behind NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL
at build time; the wire format and mount policy may change.

# --- Wave 4 packaging (#22b): four new subpackages ---------------------------
#
# Mirror of the Debian split landed in debian/control (Wave 4 / #22a). See
# docs/plans/gnome-integration-and-samba-server-2026-09-25.md §5.2 for the
# split rationale.

# --- Sub-package: nostrc-session-relay ---------------------------------------
%package -n nostrc-session-relay
Summary:        Per-user session-scoped local Nostr relay (Wave 4)
Requires:       libnostr%{?_isa} = %{version}-%{release}
Suggests:       nostr-notify%{?_isa} = %{version}-%{release}
%{?systemd_requires}

%description -n nostrc-session-relay
Ships the socket-activated per-user relay (`nostr-session-relayd`) that
hosts the desktop session's private event bus. The relay is bound to
`$XDG_RUNTIME_DIR/nostr/relay.sock` (0600 + SO_PEERCRED UID-equality
admission) and is started on demand by the shipped user socket + service
pair.

The socket owner is the socket unit (never the service); logging out
stops the pair and reclaims the fd (`PartOf=graphical-session.target`).

The postinst DOES NOT auto-enable anything; the user activates the pair
with `systemctl --user enable --now nostr-session-relay.socket`.

# --- Sub-package: nostr-notify -----------------------------------------------
# --- Sub-package: nostrc-relayd ----------------------------------------------
%package -n nostrc-relayd
Summary:        Network Nostr relay server daemon (Wave 4)
%{?systemd_requires}

%description -n nostrc-relayd
Network-facing Nostr relay server: a libwebsockets-backed relay that
speaks NIP-01/11/42 over WebSockets and persists events into nostrdb
(LMDB) when built with NostrDB support. Runs as a system daemon under
systemd (`nostr-relayd.service`), anchored to `/etc/nostrc` (drop a
`relay.toml` alongside the shipped `/usr/share/nostrc/relay.toml.example`).

The daemon is a HARDENED sibling of the per-user session relay in
`nostrc-session-relay` (which binds a Unix socket for local
desktop-session traffic). This package installs the *system* network
daemon; install both if a host runs both kinds of relay.

Not auto-enabled. The operator activates it with
`systemctl enable --now nostr-relayd.service` after seeding
`/etc/nostrc/relay.toml`.

%package -n nostr-notify
Summary:        Nostr NIP-29 + NIP-17 background notification daemon (Wave 4)
Requires:       libnostr%{?_isa} = %{version}-%{release}
Requires:       glib2
Recommends:     nostrc-session-relay%{?_isa} = %{version}-%{release}
Recommends:     nostr-dav%{?_isa} = %{version}-%{release}
%{?systemd_requires}

%description -n nostr-notify
`nostr-notify-daemon` is the background observer that subscribes to
NIP-17 direct messages (kind-1059 gift-wraps) and NIP-29 group events
and emits `GNotification` toasts via `org.nostr.NotifyDaemon` on the
session bus. It owns its own GApplication ID (never shares GNostr's
`org.gnostr.Client`, per Finding 4 of the plan).

DM notifications are OPAQUE — the outer gift-wrap pubkey is random per
NIP-17 so the daemon never leaks the sender; only NIP-29 group events
show a ≤80-char preview.  The daemon prefers the session-relay socket
(`Recommends: nostrc-session-relay`) and falls back to `home_relays`
when the socket is absent.

Enable with `systemctl --user enable --now nostr-notify.service`.

# --- Sub-package: nostr-dav --------------------------------------------------
%package -n nostr-dav
Summary:        Localhost CalDAV / CardDAV / WebDAV bridge for Nostr (Wave 4)
Requires:       libsoup3
Requires:       glib2
Requires:       libxml2
Requires:       sqlite
Requires:       json-glib
Recommends:     gnostr-signer-daemon
Recommends:     nostrc-session-relay%{?_isa} = %{version}-%{release}
%{?systemd_requires}

%description -n nostr-dav
`nostr-dav` is the loopback (`127.0.0.1:7680`) DAV bridge that lets
GNOME Evolution Data Server (Calendar / Contacts) and GNOME Files
speak to Nostr events. Ships the daemon, a user service unit, a
support oneshot (`nostr-dav-dirs.service`) that creates the token +
store directories outside the daemon sandbox, and the D-Bus session
activation file for `org.nostr.Dav`.

The WebSocket transport (plan bead tu6y) is now enabled by default;
the daemon uses libsoup-3 for both the HTTP DAV server and the
upstream WebSocket relay client.

Enable with `systemctl --user enable --now nostr-dav.service`. Token
bootstrap happens through `gnostr-signer-daemon`; the token file
lives under `$XDG_STATE_HOME/nostr-dav/`.

# --- Sub-package: nostrc-samba-server ----------------------------------------
%package -n nostrc-samba-server
Summary:        Dedicated Samba file-server for the Nostr auth broker (Wave 4)
Requires:       samba
Requires:       nostr-authd%{?_isa}    = %{version}-%{release}
Requires:       nostr-home-fuse%{?_isa} = %{version}-%{release}
%{?systemd_requires}

%description -n nostrc-samba-server
Dedicated Samba subpackage: a standalone `nostr-smbd.service` that runs
its own `smbd` bound to `/etc/nostr-auth/smb.conf` against a private
`tdbsam` passdb at `/var/lib/nostr-auth/smbpasswd`, ISOLATED from the
host's default `samba.service`. See `SAMBA_STANDALONE.md` for the
mediation contract.

The share path defaults to `/var/lib/nostr-auth/share-root` — the FUSE
mount materialized by `nostr-home-fuse.service` (Wave 4 uses share-
declaration v1, not a VFS plugin; a FUSE failure surfaces as share-
level ENOENT / EIO, not an smbd crash).

Ships five files:
  * `/etc/nostr-auth/smb.conf` — dedicated standalone smb.conf
    (renamed from `smb.conf.standalone.sample`);
  * `/etc/nostr-auth/smb-credentiald.conf` — reserved authority
    config for the future rename-and-lift daemon (renamed from
    `.sample`);
  * `/etc/nostr-auth/servers.d/example.conf` — server-identity
    drop-in example;
  * `%{_unitdir}/nostr-smbd.service` — dedicated smbd unit;
  * `%{_sysusersdir}/nostr-smb-share.conf` — sysusers.d entry for
    the `nostr-smb-share` account force-user'd by the share.

The postinst DOES NOT auto-enable the unit — the operator activates
it after seeding the passdb via `nostr-authd` and configuring shares
(`systemctl enable --now nostr-smbd.service`).

# --- Prep / build / install --------------------------------------------------

%prep
%autosetup -n nostrc-%{version}

%build
# Phase-1 headless flag set: mirrors debian/rules exactly.  Build the
# GLib-free libnostr ABI and only the components that constitute the
# headless login stack -- no GTK, no GObject, no apps, no libhanami, no
# signet, no marmot, no roaming/FUSE.
%cmake \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DNOSTR_WITH_GLIB=OFF \
    -DBUILD_APPS=OFF \
    -DBUILD_NOSTR_GOBJECT=OFF \
    -DBUILD_NOSTR_GTK=OFF \
    -DBUILD_LIBHANAMI=OFF \
    -DBUILD_LIBMARMOT=OFF \
    -DBUILD_MARMOT_GOBJECT=OFF \
    -DBUILD_RELAYD=ON \
    -DENABLE_NOSTR_DAV=ON \
    -DSIGNET_ENABLE=OFF \
    -DWITH_NOSTRDB=OFF \
    -DLIBNOSTR_WITH_NOSTRDB=OFF \
    -DWITH_NIP77_NOSTRDB=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_TESTING_FRAMEWORK=OFF \
    -DENABLE_NIP57=OFF \
    -DENABLE_NIP5F=OFF \
    -DENABLE_NIP55L_STANDALONE_ACTIVATION=OFF \
    -DENABLE_NOSTR_HOMED=ON \
    -DNOSTR_HOMED_ENABLE_AUTH_CORE=ON \
    -DNOSTR_HOMED_ENABLE_IDENTITY_CORE=ON \
    -DNOSTR_HOMED_ENABLE_NSS=ON \
    -DNOSTR_HOMED_ENABLE_AUTH_RUNTIME=ON \
    -DNOSTR_HOMED_ENABLE_PAM=ON \
    -DNOSTR_HOMED_ENABLE_SMB=ON \
    -DNOSTR_HOMED_ENABLE_CTL=ON \
    -DNOSTR_HOMED_ENABLE_DOMAIN_CONFIG=ON \
    -DNOSTR_HOMED_ENABLE_AUTH_INSTALL=ON \
    -DNOSTR_HOMED_ENABLE_SESSION_RELAY_UNITS=ON \
    -DNOSTR_HOMED_BUILD_TESTS=OFF \
    -DBUILD_LIBHANAMI=ON \
    -DNOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL=ON \
    -DNOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL=ON \
    -DNOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL=ON \
    %{nil}

%cmake_build

%install
%cmake_install

# ---- Conffile staging (mirrors debian/rules override_dh_auto_install) --------
#
# 1. /etc/nostr-auth/auth.conf is a real conffile.  CMake only stages the
#    sample under %{_datadir}/nostr-homed/auth.conf.sample so admin edits
#    survive upgrades; copy the sample verbatim to /etc/nostr-auth/auth.conf.
install -d %{buildroot}%{_sysconfdir}/nostr-auth
install -m 0644 \
    %{buildroot}%{_datadir}/nostr-homed/auth.conf.sample \
    %{buildroot}%{_sysconfdir}/nostr-auth/auth.conf

# 1b. Wave 4 (#22b): the samba-server subpackage owns the standalone
#     smb.conf pair as real conffiles under /etc/nostr-auth/, plus a
#     drop-in server-identity example under servers.d/.  CMake stages
#     the `.sample` copies under %{_datadir}/nostr-homed/ and
#     %{_docdir}/nostrc/examples/servers.d/; rename them into their
#     runtime homes here so `smbpasswd -c` / `pdbedit -s` operate on
#     the operator's live copy and rpm marks them as %config(noreplace).
if [ -f %{buildroot}%{_datadir}/nostr-homed/smb.conf.standalone.sample ]; then
    install -m 0644 \
        %{buildroot}%{_datadir}/nostr-homed/smb.conf.standalone.sample \
        %{buildroot}%{_sysconfdir}/nostr-auth/smb.conf
fi
if [ -f %{buildroot}%{_datadir}/nostr-homed/smb-credentiald.conf.sample ]; then
    install -m 0644 \
        %{buildroot}%{_datadir}/nostr-homed/smb-credentiald.conf.sample \
        %{buildroot}%{_sysconfdir}/nostr-auth/smb-credentiald.conf
fi
install -d %{buildroot}%{_sysconfdir}/nostr-auth/servers.d
if [ -f %{buildroot}%{_docdir}/nostrc/examples/servers.d/example.conf ]; then
    install -m 0644 \
        %{buildroot}%{_docdir}/nostrc/examples/servers.d/example.conf \
        %{buildroot}%{_sysconfdir}/nostr-auth/servers.d/example.conf
fi

# 2. Ship /etc/pam.d/nostr-login as a real config file (the proof service
#    consumed by pamtester(1)).  Sourced from packaging/pam/nostr-login.sample.
install -d %{buildroot}%{_sysconfdir}/pam.d
install -m 0644 \
    gnome/nostr-homed/packaging/pam/nostr-login.sample \
    %{buildroot}%{_sysconfdir}/pam.d/nostr-login

# 3. authselect drop-in / documented recipe for pam_nostr on Fedora.
#    Fedora does NOT use pam-auth-update; the equivalent activation surface
#    is authselect.  We do NOT ship a full authselect vendor profile here
#    (creating a whole "sssd-alike" profile just to add two auth lines is
#    heavier than the phase warrants); instead we ship a README and the
#    exact pam_nostr lines admins drop into a `authselect create-profile`
#    custom profile.  This satisfies the plan's "authselect custom profile
#    / feature (or a documented drop-in), NOT a copy of the Debian
#    pam-configs" clause.
install -d %{buildroot}%{_datadir}/nostr-homed/authselect
cat > %{buildroot}%{_datadir}/nostr-homed/authselect/README.md <<'EOF'
# Enabling pam_nostr on Fedora (authselect)

Fedora composes PAM via **authselect**, not pam-auth-update.  There are two
supported ways to activate pam_nostr on a Fedora host:

## A. Custom profile derived from `sssd` (recommended)

    sudo authselect create-profile nostr -b sssd --symlink-meta
    # -> writes an editable copy of the sssd profile to
    #    /etc/authselect/custom/nostr/{system-auth,password-auth,...}

Then add the pam_nostr lines to the `Auth` and `Account` stacks of
`/etc/authselect/custom/nostr/system-auth` and `.../password-auth` -- see
`nostr-auth.snippet` and `nostr-account.snippet` in this directory.

Finally:

    sudo authselect select custom/nostr with-sudo         # or your feature set
    sudo authselect apply-changes

## B. Manual /etc/pam.d edit (no authselect)

If your host does not use authselect (or you run a non-authselect config),
edit `/etc/pam.d/system-auth` directly and prepend the `Auth`/`Account`
lines from `nostr-auth.snippet` / `nostr-account.snippet` above the
existing `pam_unix.so` lines.  Priority mirrors the Debian pam-configs
`Priority: 384` (pam_nostr sees the account before pam_unix).

## C. Proof-only (pamtester)

`pam-nostr` also ships `/etc/pam.d/nostr-login` as a %config(noreplace)
proof service.  It is not wired into login; it exists so admins can
validate pam_nostr against a running nostr-authd without changing
system-auth:

    sudo systemctl start nostr-authd
    sudo nostr-homed-seed /var/lib/nostr-auth n_alice '<passphrase>'
    printf 'passphrase\n' | sudo pamtester -v nostr-login n_alice authenticate

## Rollback

    sudo authselect select <previous-profile>
    sudo authselect apply-changes

For method B, restore /etc/pam.d/system-auth from your backup or run
`authselect apply-changes` after re-selecting a stock profile.

## Semantics (strict-deny, beads nostrc-o1ho)

    success           -> done   (broker accepted the proof; stack proceeds
                                 and pam_unix is NOT consulted)
    user_unknown      -> ignore (not a nostr account; fall through to Unix)
    authinfo_unavail  -> ignore (broker unreachable; do not brick login)
    default           -> die    (known nostr account, failed proof: refuse
                                 immediately; do NOT fall through to Unix)
EOF

cat > %{buildroot}%{_datadir}/nostr-homed/authselect/nostr-auth.snippet <<'EOF'
# Insert above the existing "auth ... pam_unix.so ..." line of
# /etc/authselect/custom/nostr/system-auth (and password-auth).
auth        [success=done new_authtok_reqd=done user_unknown=ignore authinfo_unavail=ignore default=die] pam_nostr.so socket=/run/nostr-auth/auth.sock
EOF

cat > %{buildroot}%{_datadir}/nostr-homed/authselect/nostr-account.snippet <<'EOF'
# Insert above the existing "account ... pam_unix.so ..." line of
# /etc/authselect/custom/nostr/system-auth (and password-auth).
account     [success=done user_unknown=ignore authinfo_unavail=ignore default=die] pam_nostr.so socket=/run/nostr-auth/auth.sock
EOF

# 4. Drop the pam-auth-update profile that CMake installs (Debian-only).
#    Fedora has no pam-auth-update -- keeping /usr/share/pam-configs/nostr on
#    a Fedora host would be dead weight.
rm -f %{buildroot}%{_datadir}/pam-configs/nostr
rmdir --ignore-fail-on-non-empty \
    %{buildroot}%{_datadir}/pam-configs 2>/dev/null || :

# 5. Sanitize the seeder binary's RUNPATH.  install(PROGRAMS $<TARGET_FILE:...>
#    RENAME ...) copies the build-tree binary verbatim, preserving the
#    build-tree RUNPATH set by CMake.  Strip it so rpmlint's
#    binary-or-shlib-defines-rpath does not fire (mirrors debian/rules
#    override_dh_shlibdeps).
if [ -x %{buildroot}%{_sbindir}/nostr-homed-seed ]; then
    chrpath -d %{buildroot}%{_sbindir}/nostr-homed-seed || :
fi

# 6. Drop files that Phase 1 does NOT ship.  These come from CMake
#    install() rules that fire even when the top-level component (apps,
#    signer, per-NIP shared libs, nostr-homed.pc) is disabled -- the exact
#    list is the Fedora analogue of debian/not-installed.  We do it here
#    (rather than in %files with %exclude) so rpmbuild's "installed but not
#    packaged" scanner passes cleanly.
#
#    a) Apps / signer bits that leak through when BUILD_APPS is off.
rm -f  %{buildroot}%{_bindir}/gnostr-neg
rm -f  %{buildroot}%{_bindir}/nostr-signer-cli
rm -f  %{buildroot}%{_bindir}/nostr-signer-daemon
rm -f  %{buildroot}%{_datadir}/dbus-1/services/org.nostr.Signer.service
rmdir --ignore-fail-on-non-empty %{buildroot}%{_datadir}/dbus-1/services 2>/dev/null || :
rmdir --ignore-fail-on-non-empty %{buildroot}%{_datadir}/dbus-1          2>/dev/null || :
#
#    b) NIP libraries (D-6): folded into libnostr, no per-NIP packages in
#       Phase 1.  Drop stray .so / .a artifacts.  Some NIP CMakeLists use
#       GNUInstallDirs (%{_libdir}=/usr/lib64) and some hardcode `lib`
#       (which resolves to /usr/lib on Fedora) -- purge both.  Same story
#       as debian/not-installed lines mixing /usr/lib/ and
#       /usr/lib/x86_64-linux-gnu/.
rm -f  %{buildroot}%{_includedir}/nip40.h
rm -f  %{buildroot}/usr/lib/libnip04.a
rm -f  %{buildroot}/usr/lib/libnip05.so
rm -f  %{buildroot}/usr/lib/libnip25.so
rm -f  %{buildroot}/usr/lib/libnostr_nip77.a
rm -f  %{buildroot}%{_libdir}/libnip19.so
rm -f  %{buildroot}%{_libdir}/libnostr_nip55l_core.so
rm -f  %{buildroot}%{_libdir}/libnostr_nip55l_glib.so
#
#    c) nostr-homed.pc: multiarch-specific pkg-config for the nostr-homed
#       framework; deferred to a future -devel package (matches the Debian
#       not-installed exclusion for the same file).
rm -f  %{buildroot}%{_libdir}/pkgconfig/nostr-homed.pc
#
#    e) libhanami-devel bits (Phase 5): no third-party consumers this cycle,
#       so drop hanami.pc + the public headers to keep the runtime lib alone
#       in libhanami.  Matches debian/not-installed.
rm -f  %{buildroot}%{_libdir}/pkgconfig/hanami.pc
rm -rf %{buildroot}%{_includedir}/hanami
#
#    d) test_relay_eose: an upstream CMake install() rule ships a build-tree
#       binary to an ABSOLUTE build-tree path (out of DESTDIR discipline).
#       rpmbuild still catches it inside %{buildroot}/... -- scrub the
#       leaked build path and any orphan tools/ directory.  Matches the
#       "Test binary that installs via a stray CMake rule (build-tree
#       leak)" line in debian/not-installed.
find %{buildroot} -type f -name test_relay_eose -delete
find %{buildroot} -type d -empty -name tools -delete 2>/dev/null || :

#    f) Wave 3 leftovers not yet folded into nostr-homed-smb: the
#       sweep timer + service, close-session user teardown unit,
#       browse helper + autostart entry, admin CLI nostr-authctl.
#       Drop for Wave 4 packaging (#22b); tracked as a follow-up
#       bead.  Mirrors debian/not-installed.
rm -f %{buildroot}%{_sbindir}/nostr-authctl
rm -f %{buildroot}%{_unitdir}/nostr-smb-sweep.service
rm -f %{buildroot}%{_unitdir}/nostr-smb-sweep.timer
rm -f %{buildroot}%{_userunitdir}/nostr-smb-teardown.service
rm -f %{buildroot}%{_bindir}/nostr-smb-browse
rm -f %{buildroot}%{_sysconfdir}/xdg/autostart/nostr-smb-browse.desktop
rmdir --ignore-fail-on-non-empty %{buildroot}%{_sysconfdir}/xdg/autostart 2>/dev/null || :
rmdir --ignore-fail-on-non-empty %{buildroot}%{_sysconfdir}/xdg 2>/dev/null || :
rm -f %{buildroot}%{_datadir}/nostr-homed/nsswitch.conf.snippet.sample
rm -f %{buildroot}%{_docdir}/nostrc/examples/servers.d/example.conf.batch-client

find %{buildroot} -depth -type d -empty -delete 2>/dev/null || :

# --- Scriptlets --------------------------------------------------------------

%post -n nostr-authd
%systemd_post nostr-authd.service

%preun -n nostr-authd
%systemd_preun nostr-authd.service

%postun -n nostr-authd
%systemd_postun_with_restart nostr-authd.service

# --- Portable-home Phase 5 scriptlets ---------------------------------------
# Both user units follow the same posture as nostr-authd: do NOT auto-enable
# or auto-start.  The sync daemon needs a broker seed drop before it does
# anything useful; the FUSE overlay needs a snapshot.  Users run
# `systemctl --user enable nostr-home-sync nostr-home-fuse` after
# provisioning their portable identity.  systemd-sysusers runs on install
# (via %sysusers_create_compat below) so the nostr-home-fetch helper
# account exists.

%post -n nostr-home-sync
%systemd_user_post nostr-home-sync.service
%sysusers_create_compat %{_sysusersdir}/nostr-home-fetch.conf

%preun -n nostr-home-sync
%systemd_user_preun nostr-home-sync.service

%postun -n nostr-home-sync
%systemd_user_postun nostr-home-sync.service

%post -n nostr-home-fuse
%systemd_user_post nostr-home-fuse.service

%preun -n nostr-home-fuse
%systemd_user_preun nostr-home-fuse.service

%postun -n nostr-home-fuse
%systemd_user_postun nostr-home-fuse.service

# --- Wave 4 scriptlets ------------------------------------------------------
# All four new packages follow the same posture as nostr-authd + the
# portable-home pair above: DO NOT auto-enable or auto-start.  The
# operator opts in per user (session-relay + notify + dav) or after
# passdb seeding (samba-server).

%post -n nostrc-session-relay
%systemd_user_post nostr-session-relay.socket
%systemd_user_post nostr-session-relay.service

%preun -n nostrc-session-relay
%systemd_user_preun nostr-session-relay.socket
%systemd_user_preun nostr-session-relay.service

%postun -n nostrc-session-relay
%systemd_user_postun nostr-session-relay.socket
%systemd_user_postun nostr-session-relay.service

%post -n nostrc-relayd
%systemd_post nostr-relayd.service

%preun -n nostrc-relayd
%systemd_preun nostr-relayd.service

%postun -n nostrc-relayd
%systemd_postun_with_restart nostr-relayd.service

%post -n nostr-notify
%systemd_user_post nostr-notify.service

%preun -n nostr-notify
%systemd_user_preun nostr-notify.service

%postun -n nostr-notify
%systemd_user_postun nostr-notify.service

%post -n nostr-dav
%systemd_user_post nostr-dav.service
%systemd_user_post nostr-dav-dirs.service

%preun -n nostr-dav
%systemd_user_preun nostr-dav.service
%systemd_user_preun nostr-dav-dirs.service

%postun -n nostr-dav
%systemd_user_postun nostr-dav.service
%systemd_user_postun nostr-dav-dirs.service

%post -n nostrc-samba-server
%systemd_post nostr-smbd.service
%sysusers_create_compat %{_sysusersdir}/nostr-smb-share.conf

%preun -n nostrc-samba-server
%systemd_preun nostr-smbd.service

%postun -n nostrc-samba-server
%systemd_postun nostr-smbd.service

# Fedora rpmbuild auto-generates ldconfig %post / %postun for any subpackage
# owning %{_libdir}/*.so.<SOVERSION>.  We do NOT ship manual ldconfig
# scriptlets -- rpmlint (rightly) flags them as `non-empty-%postun
# /sbin/ldconfig` since the auto-generated block is already there.

# --- %check: Wave 4 dep-purity gate (#21a/#22b) ------------------------------
#
# Run the pinned dep-purity gate against the installed artifacts inside
# %{buildroot}.  Fails the RPM build the moment the auth broker or PAM
# module gain a link edge to hanami / porthome / nip55l-client / FUSE
# surfaces (plan §5.1 #21a).  Script exit-code 77 == "skip" (tool
# missing); rpmbuild honours a non-zero exit here.
#
# The dep-purity check is the same one nightly CI runs; using the
# INSTALLED path (not obj-*) catches strip- or RPATH-related shifts
# that would otherwise slip through the build tree.

%check
_authd=%{buildroot}%{_sbindir}/nostr-authd
_pam=%{buildroot}%{_libdir}/security/pam_nostr.so
if [ -x "$_authd" ]; then
    echo "dep-purity: checking $_authd $_pam"
    %{_builddir}/nostrc-%{version}/scripts/check-authd-dep-purity.sh \
        "$_authd" "$_pam" || exit $?
else
    echo "dep-purity: nostr-authd not staged under %%buildroot; skipping"
fi

# --- File lists --------------------------------------------------------------
#
# The main `nostrc` package has NO %files section.  rpmbuild skips the main
# binary RPM entirely and only produces the subpackage RPMs listed below --
# the SRPM is the only artifact that carries the `nostrc` name.

%files -n nostr-login
%license LICENSE
%doc README.md docs/designs/packaging-plan-debian-fedora.md

%files -n libnostr
%license LICENSE
%{_libdir}/libnostr.so.1
%{_libdir}/libnostr.so.1.*

%files -n libnostr-devel
%{_libdir}/libnostr.so
%{_libdir}/pkgconfig/nostr.pc
%{_includedir}/nostr/

%files -n libnostrgo
%license LICENSE
%{_libdir}/libnostrgo.so.0
%{_libdir}/libnostrgo.so.0.*

%files -n libnostrgo-devel
%{_libdir}/libnostrgo.so
%{_libdir}/pkgconfig/libnostrgo.pc
%{_libdir}/pkgconfig/libgo.pc
%{_includedir}/go.h
%{_includedir}/blocking_executor.h
%{_includedir}/channel.h
%{_includedir}/context.h
%{_includedir}/counter.h
%{_includedir}/error.h
%{_includedir}/go_auto.h
%{_includedir}/gtime.h
%{_includedir}/hash_map.h
%{_includedir}/int_array.h
%{_includedir}/refptr.h
%{_includedir}/select.h
%{_includedir}/string_array.h
%{_includedir}/ticker.h
%{_includedir}/wait_group.h
%{_includedir}/libgo/
# Vendored nsync public headers (installed by cmake/FindOrVendorNsync.cmake
# when NOSTRC_NSYNC_VENDORED=TRUE, i.e. the default Fedora build).  Owned
# by libnostrgo-devel so downstream consumers of libnostrgo can #include
# <nsync.h> without installing a separate nsync-devel package.
%if %{without system_nsync}
%{_includedir}/nsync.h
%{_includedir}/nsync_atomic.h
%{_includedir}/nsync_counter.h
%{_includedir}/nsync_cpp.h
%{_includedir}/nsync_cv.h
%{_includedir}/nsync_debug.h
%{_includedir}/nsync_mu.h
%{_includedir}/nsync_mu_wait.h
%{_includedir}/nsync_note.h
%{_includedir}/nsync_once.h
%{_includedir}/nsync_time.h
%{_includedir}/nsync_time_internal.h
%{_includedir}/nsync_waiter.h
%endif

%files -n libnostr-json
%license LICENSE
%{_libdir}/libnostr-json.so.1
%{_libdir}/libnostr-json.so.1.*
# D-4 compat symlink: shipped in the runtime package for one release.
%{_libdir}/libnostr_json.so

%files -n libnostr-json-devel
%{_libdir}/libnostr-json.so
%{_libdir}/pkgconfig/libnostr-json.pc
%{_libdir}/pkgconfig/nostr_json.pc

%files -n nss-nostr
%license LICENSE
%config(noreplace) %{_sysconfdir}/nss_nostr.conf
%{_libdir}/libnss_nostr.so.2
%{_datadir}/nostr-homed/nostr-homed-cache.legacy.conf.sample

%files -n pam-nostr
%license LICENSE
%config(noreplace) %{_sysconfdir}/pam.d/nostr-login
%{_libdir}/security/pam_nostr.so
%dir %{_datadir}/nostr-homed/authselect
%{_datadir}/nostr-homed/authselect/README.md
%{_datadir}/nostr-homed/authselect/nostr-auth.snippet
%{_datadir}/nostr-homed/authselect/nostr-account.snippet
%doc %{_docdir}/nostrc/examples/pam/*

%files -n nostr-authd
%license LICENSE
%dir %{_sysconfdir}/nostr-auth
%config(noreplace) %{_sysconfdir}/nostr-auth/auth.conf
%{_sbindir}/nostr-authd
%{_unitdir}/nostr-authd.service
%{_datadir}/nostr-homed/auth.conf.sample

%files -n nostr-homectl
%license LICENSE
%{_bindir}/nostr-homectl
%{_sbindir}/nostr-homed-seed

%files -n nostr-homed-smb
%license LICENSE
%{_bindir}/nostr-smb-acquire
%{_bindir}/nostr-smb-mount

%files -n nostr-homed-domain
%license LICENSE
%dir %{_datadir}/nostr-homed
%dir %{_datadir}/nostr-homed/domain
%{_datadir}/nostr-homed/domain/*
%{_libexecdir}/nostr-homed/validate_domain_profile.py


%files -n libhanami
%license LICENSE
%{_libdir}/libhanami.so.0
%{_libdir}/libhanami.so.0.*

%files -n nostr-home-sync
%license LICENSE
%{_bindir}/nostr-home-syncd
%{_bindir}/nostr-home-status
%{_bindir}/nostr-homed-provision
%{_userunitdir}/nostr-home-sync.service
%{_libexecdir}/nostr-homed/nostr-home-fetch
%{_sysusersdir}/nostr-home-fetch.conf
# nostrc-s6ke: manpages
%{_mandir}/man1/nostr-homed-provision.1*
%{_mandir}/man1/nostr-homed-provision-enroll.1*
%{_mandir}/man1/nostr-homed-provision-status.1*
%{_mandir}/man1/nostr-homed-provision-pull.1*
%{_mandir}/man1/nostr-homed-provision-push.1*
%{_mandir}/man1/nostr-homed-provision-verify.1*
%{_mandir}/man1/nostr-home-status.1*
%{_mandir}/man1/nostr-home-fetch.1*
%{_mandir}/man8/nostr-home-syncd.8*

%files -n nostr-home-fuse
%license LICENSE
%{_bindir}/nostr-home-fuse
%{_userunitdir}/nostr-home-fuse.service
%dir %{_datadir}/nostr-homed/ignore.d
%{_datadir}/nostr-homed/ignore.d/portable
# nostrc-s6ke: manpages
%{_mandir}/man8/nostr-home-fuse.8*

# --- Wave 4 (#22b) file lists -----------------------------------------------

%files -n nostrc-session-relay
%license LICENSE
%{_libexecdir}/nostr-session-relayd
%{_userunitdir}/nostr-session-relay.service
%{_userunitdir}/nostr-session-relay.socket

%files -n nostrc-relayd
%license LICENSE
%{_sbindir}/nostrc-relayd
%{_unitdir}/nostr-relayd.service
%dir %{_datadir}/nostrc
%{_datadir}/nostrc/relay.toml.example

%files -n nostr-notify
%license LICENSE
%{_libexecdir}/nostr-notify-daemon
%{_userunitdir}/nostr-notify.service
%dir %{_datadir}/nostr-notify
%{_datadir}/nostr-notify/nostr-notify.conf.example

%files -n nostr-dav
%license LICENSE
%{_bindir}/nostr-dav
%{_userunitdir}/nostr-dav.service
%{_userunitdir}/nostr-dav-dirs.service
%{_datadir}/dbus-1/services/org.nostr.Dav.service
%dir %{_datadir}/doc/nostr-dav
%{_datadir}/doc/nostr-dav/nostr-dav.conf.sample

%files -n nostrc-samba-server
%license LICENSE
%dir %{_sysconfdir}/nostr-auth/servers.d
%config(noreplace) %{_sysconfdir}/nostr-auth/smb.conf
%config(noreplace) %{_sysconfdir}/nostr-auth/smb-credentiald.conf
%config(noreplace) %{_sysconfdir}/nostr-auth/servers.d/example.conf
%{_unitdir}/nostr-smbd.service
%{_sysusersdir}/nostr-smb-share.conf
# Sample originals retained for provenance / operator diff base.
%{_datadir}/nostr-homed/smb.conf.standalone.sample
%{_datadir}/nostr-homed/smb-credentiald.conf.sample
%doc %{_docdir}/nostrc/examples/servers.d/example.conf
%doc %{_docdir}/nostrc/SAMBA_STANDALONE.md

# --- Changelog ---------------------------------------------------------------

%changelog
* Fri Sep 25 2026 GNostr Project <gnostr@example.com> - 0.4.0-1
- Wave 4 packaging (#22b): add the four new subpackages for the
  GNOME-integration + Samba-server milestones.
  * nostrc-session-relay: nostr-session-relayd + user socket/service.
  * nostr-notify: nostr-notify-daemon + user service.
  * nostr-dav: localhost CalDAV/CardDAV/WebDAV bridge + user service +
    D-Bus session activation for org.nostr.Dav.
  * nostrc-samba-server: dedicated smb.conf + smb-credentiald.conf +
    servers.d/example.conf + nostr-smbd.service + sysusers entry.
- Enable BUILD_RELAYD, ENABLE_NOSTR_DAV and NOSTR_HOMED_ENABLE_SESSION_
  RELAY_UNITS in %build; add BuildRequires for glib2/libsoup3/libxml2/
  json-glib/libsecret.
- New %check block runs scripts/check-authd-dep-purity.sh against the
  installed nostr-authd + pam_nostr.so under %{buildroot}; rpmbuild
  fails if either gains a link edge to hanami / porthome / nip55l-
  client / FUSE surfaces (plan §5.1 #21a).
- ENABLE_NIP55L_STANDALONE_ACTIVATION explicit OFF; the pre-existing
  cleanup that removes org.nostr.Signer.service from the buildroot is
  retained so gnostr-signer's own spec remains the sole owner
  (Milestone-A D3 dedup).
- Rename smb.conf.standalone.sample and smb-credentiald.conf.sample
  into /etc/nostr-auth/ as %config(noreplace) conffiles; sample
  originals kept under %{_datadir}/nostr-homed/ for provenance.

* Tue Sep 22 2026 GNostr Project <gnostr@example.com> - 0.3.0-1
- Initial Fedora SRPM for the nostrc monorepo.
- Phase 1 (nostrc-rb0e.3 D2, nostrc-rb0e.10 D9): headless nostr-login stack.
- Builds one SRPM (nostr-login) that produces the metapackage + the
  headless login stack: nss-nostr, pam-nostr, nostr-authd, nostr-homectl,
  nostr-homed-smb, nostr-homed-domain, plus the core runtime shared libs
  the stack links against (libnostr, libnostrgo, libnostr-json) and their
  -devel packages.
- Built with NOSTR_WITH_GLIB=OFF (D-14 headless ABI) so the whole
  nostr-login Requires closure carries no GLib / GTK / GNOME.
- Fedora-specific activation: authselect drop-in documented under
  %{_datadir}/nostr-homed/authselect/README.md, plus /etc/pam.d/nostr-login
  proof service for pamtester(1).  No pam-auth-update integration.
