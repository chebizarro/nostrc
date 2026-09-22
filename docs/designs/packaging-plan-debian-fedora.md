# Packaging Plan — Debian/Ubuntu + Fedora, headless vs GNOME split

**Status:** DRAFT FOR MAINTAINER REVIEW — design/analysis only. No `debian/control`,
no `.spec`, no CPack code is authored by this document.

**Beads:** `nostrc-svyg` (this document) → blocks `nostrc-rb0e.3` (D2: package split,
feature flags, file ownership, version plan) and `nostrc-rb0e.10` (D9: packaging
integration).

**Date:** 2026-09-21 · **Repo state:** `master`, monorepo `nostrc`

---

## 0. Scope, goal, and the one-sentence thesis

Decompose the `nostrc` monorepo into a set of distro packages that follow
Debian/Ubuntu and Fedora convention, with a **hard split between the headless
login stack and the GNOME desktop stack**. The maintainer's explicit constraint:

> The Samba (passdb/acquire), PAM and NSS libraries plus the auth broker are
> useful on headless servers. They must be independently installable without
> pulling in GTK/GNOME.

That constraint is achievable today at the *runtime dependency* level, with one
caveat. The nostr-homed components themselves (`libnss_nostr.so.2`, `pam_nostr.so`,
`nostr-authd`, `nostr-smb-acquire`, `nostr-smb-tdbsam`) link only OpenSSL, SQLite3,
jansson, libpam and libnostr. **None of them touch GTK4, libadwaita, libsecret, gvfs
or FUSE.** This is verifiable from `gnome/nostr-homed/CMakeLists.txt`: the `GLIB2`
`pkg_check_modules` call lives only inside the
`NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING` block (lines 345–450), never in the
auth/identity/NSS/PAM/SMB blocks.

⚠️ **The caveat: `libnostr` itself pulls GLib by default.** `libnostr/CMakeLists.txt:19`
declares `option(NOSTR_WITH_GLIB "Enable GLib integration headers" ON)`, and when
GLib is detected the target links it **PUBLIC** (`:189–191`) and `nostr.pc` gains
`Requires: glib-2.0 >= 2.50, gobject-2.0 >= 2.50` (`:387–391`). So a default build on
a buildd that happens to have `libglib2.0-dev` installed produces a `libnostr1` that
`Depends: libglib2.0-0` — and one without it produces a different, smaller ABI. That
is both a headless-purity problem and a **build reproducibility problem**: the same
source yields different libraries depending on what the builder had installed. GLib
is not GTK/GNOME and is present on nearly every system, but the flag must be pinned
explicitly either way. See blocker **B11** and decision **D-14**.

The work is therefore mostly **build-system and policy plumbing**, not a
re-architecture. Sections 6–8 enumerate the concrete blockers.

---

## 1. Component inventory + dependency graph

### 1.1 Build-tree reality (what actually exists today)

| Component | Path | CMake target(s) | Gate | Installed? | Lib type today |
|---|---|---|---|---|---|
| libgo | `libgo/` | `libgo`, `go_fiber` | always | yes (`install(TARGETS libgo)`) | **static** (`liblibgo.a`) |
| libnostr | `libnostr/` | `nostr` (alias `libnostr`), `nostrdb` | always; `WITH_NOSTRDB` / `LIBNOSTR_WITH_NOSTRDB` | yes | **static** (`libnostr.a`) |
| libjson | `libjson/` | `nostr_json` | always | yes | **shared, hardcoded** (`add_library(nostr_json SHARED …)`) |
| NIP libs | `nips/nip*` | ~50 targets | `ENABLE_NIP<nn>` in `NipOptions.cmake` | **partially** — some `install(TARGETS)`, most install headers only, many are `OBJECT` libs | static / OBJECT |
| nostr-gobject | `nostr-gobject/` | `nostr_gobject` → `libnostr-gobject-1.0` | `BUILD_NOSTR_GOBJECT` (ON) | yes + `.pc` + GIR `GNostr-1.0` | static by default, `SOVERSION` set |
| nostr-gtk | `nostr-gtk/` | `nostr_gtk` → `libnostr-gtk-1.0` | `BUILD_NOSTR_GTK` (ON) | yes + `.pc` + GIR `GNostrGtk-1.0` | static by default, `SOVERSION` set |
| libmarmot | `libmarmot/` | `marmot` | `BUILD_LIBMARMOT` (ON) | yes + `marmot.pc` | `SOVERSION 0` |
| marmot-gobject | `marmot-gobject/` | `marmot-gobject` | `BUILD_MARMOT_GOBJECT` (ON, needs `BUILD_LIBMARMOT`) | yes + `.pc` + GIR | `SOVERSION` set |
| libhanami | `libhanami/` | `hanami` | `BUILD_LIBHANAMI` (ON, forces `ENABLE_NIP34`) | yes + `hanami.pc` | static, **no SOVERSION** |
| signet bunker | `signet/` | `signet_core`, `signetd`, `signetctl`, `signet-git-credential` | `SIGNET_ENABLE` (ON) | yes, but `RUNTIME DESTINATION bin` (**relative, not `GNUInstallDirs`**) | — |
| relayd | `apps/relayd/` | `relayd_core`, `relay_security`, `nostrc-relayd` | `BUILD_RELAYD` (ON) | yes, `DESTINATION bin` (relative) | — |
| grelay / relayctl / blossom-cache | `apps/*` | various | `BUILD_APPS` → `ENABLE_APPS`; `ENABLE_BLOSSOM_CACHE` | — | — |
| gnostr (GTK app) | `apps/gnostr/` | `gnostr` | `BUILD_APPS` + `BUILD_GNOSTR_APP` (needs GTK4+libpeas-2) | yes (bin, gschema, desktop, appdata, icons, search-provider) | — |
| gnostr-signer (+daemon) | `apps/gnostr-signer/` | `gnostr-signer`, `gnostr-signer-daemon` | `BUILD_APPS` (needs GTK4+GLib) | yes (bin, gschema, D-Bus service, user unit, desktop, metainfo, icons) | — |
| nostr-dav | `gnome/nostr-dav/` | — | `ENABLE_NOSTR_DAV` (OFF) | — | — |
| seahorse helpers | `gnome/seahorse/` | — | `GNOSTR_ENABLE_SEAHORSE_HELPERS` (OFF) | — | — |
| p11-kit module | `pkcs11/` | — | `GNOSTR_WITH_PKCS11` (OFF) | — | — |

### 1.2 `gnome/nostr-homed` — the distinct installables

All gated behind the root option `ENABLE_NOSTR_HOMED` (default **OFF**), then
per-feature flags. This is the most important sub-tree for phase 1.

| Installable | Target(s) | Flag(s) | Install destination | Headless? |
|---|---|---|---|---|
| **NSS module** `libnss_nostr.so.2` | `nss_nostr` (MODULE, `PREFIX libnss_`, `SUFFIX .so.2`) | `NOSTR_HOMED_ENABLE_NSS` (requires `IDENTITY_CORE`) | `${CMAKE_INSTALL_LIBDIR}` — **multiarch libdir, NOT `security/`** | ✅ |
| **PAM module** `pam_nostr.so` | `pam_nostr` (MODULE) | `NOSTR_HOMED_ENABLE_PAM` (requires `AUTH_RUNTIME`) | `${NH_PAM_MODULE_DIR}` = `${CMAKE_INSTALL_LIBDIR}/security` (cache var, overridable) | ✅ |
| **Broker daemon** `nostr-authd` + unit | `nostr-authd`, `nostr_auth_runtime` | `NOSTR_HOMED_ENABLE_AUTH_RUNTIME`; installed by `AUTH_INSTALL` | `${CMAKE_INSTALL_SBINDIR}` + `systemdsystemunitdir` | ✅ |
| **SMB acquire client** `nostr-smb-acquire` | `nostr-smb-acquire` | `AUTH_RUNTIME` **and** `ENABLE_SMB`; installed by `AUTH_INSTALL` | `${CMAKE_INSTALL_BINDIR}` | ✅ |
| **SMB mount helper** `nostr-smb-mount` | shell script (`configure_file … COPYONLY`) | same | `${CMAKE_INSTALL_BINDIR}` | ✅ (cifs) / desktop-only in `--mode gvfs` |
| **tdbsam passdb adapter** | `nostr_smb_tdbsam` (STATIC), `tdbsam_driver` | `NOSTR_HOMED_ENABLE_SMB` | **not installed** — linked into `nostr-authd` | ✅ |
| **SMB credential core** | `nostr_smb_core` (STATIC) | `NOSTR_HOMED_ENABLE_SMB` | not installed (internal) | ✅ |
| **Identity authority core** | `nostr_identity_core` (STATIC) | `NOSTR_HOMED_ENABLE_IDENTITY_CORE` | not installed (internal) | ✅ |
| **Auth core** | `nostr_auth_core` (STATIC) | `NOSTR_HOMED_ENABLE_AUTH_CORE` | not installed (internal) | ✅ |
| **Seeder / provisioning tool** `nh-seed-authority` | `nh-seed-authority` | `AUTH_RUNTIME` **and** `NOSTR_HOMED_BUILD_TESTS` | ⚠️ **not installed at all** | ✅ |
| **`nostr-homectl` CLI** | `nostr-homectl` | ⚠️ `NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING` only | `${CMAKE_INSTALL_BINDIR}` | ❌ today (pulls GLib+FUSE-adjacent `nostr_homed_common`) |
| **nostrfs (FUSE)** | `nostrfs` | `NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING` | `${CMAKE_INSTALL_BINDIR}` + D-Bus iface XML + systemd **user** units | ❌ (FUSE3, GLib, curl) |
| **Domain/winbind config** | — (files) | `NOSTR_HOMED_ENABLE_DOMAIN_CONFIG` | `${datadir}/nostr-homed/domain` + `${libexecdir}/nostr-homed/validate_domain_profile.py` | ✅ (needs Python3) |
| **Version metadata** `nostr-homed.pc` | — | unconditional | `${libdir}/pkgconfig` | ✅ |

Flag dependency chain enforced by `message(FATAL_ERROR)` in-tree:

```
IDENTITY_CORE ──► NSS
IDENTITY_CORE ──► SMB
AUTH_CORE + IDENTITY_CORE ──► AUTH_RUNTIME ──► PAM
AUTH_RUNTIME + PAM + NSS ──► AUTH_INSTALL   (the installable login product)
AUTH_RUNTIME + SMB        ──► SMB proof endpoint + nostr-smb-acquire/mount
ENABLE_NIP19              ──► EXPERIMENTAL_ROAMING
```

### 1.3 Dependency graph (link-level)

```
                      nsync ── OpenSSL ── secp256k1 ── jansson ── libwebsockets
                         │        │           │           │            │
        libgo ◄──────────┘        └───────────┴───────────┴────────────┘
          │                                   │
       libnostr ◄───────────────────────────── (core protocol)
          │  └── nostrdb (LMDB, optional)
          ├── libjson (nostr_json)  ── jansson
          ├── nips/nip04 nip05 nip06 nip11 nip19 nip34 nip44 nip46 nip59 …
          │
          ├── libmarmot ── libsodium, OpenSSL
          │     └── marmot-gobject ── GLib/GObject/GIO
          ├── libhanami ── libgit2, libcurl, sqlite3, (lmdb), nip34, nip98
          ├── signet_core ── sqlite3|sqlcipher, libsodium, libcbor, libmicrohttpd, libcurl, GLib, json-glib
          │     └── signetd / signetctl / signet-git-credential
          ├── relayd_core + relay_security ── libwebsockets, OpenSSL, secp256k1, nsync
          │
          ├── nostr-gobject ── GLib/GObject/GIO  ── GIR GNostr-1.0
          │     └── nostr-gtk ── GTK4, libadwaita ── GIR GNostrGtk-1.0
          │           └── apps/gnostr ── libpeas-2, json-glib, libsoup3, sqlite3,
          │                              libsecret, (gstreamer, qrencode, webkitgtk-6.0)
          │           └── apps/gnostr-signer(+daemon) ── libsecret, json-glib, (p11-kit)
          │
          └── gnome/nostr-homed
                ├── nostr_identity_core ── sqlite3, OpenSSL::Crypto        [HEADLESS]
                ├── nostr_auth_core ── libnostr, jansson, OpenSSL, sqlite3,
                │                      nostr_nip46_core                     [HEADLESS]
                ├── nostr_auth_runtime ── + nostr_smb_core                  [HEADLESS]
                │     ├── nostr-authd (sbin) + systemd unit                 [HEADLESS]
                │     └── pam_nostr.so ── libpam                            [HEADLESS]
                ├── nss_nostr (libnss_nostr.so.2) ── identity_core          [HEADLESS]
                ├── nostr_smb_core / nostr_smb_tdbsam ── sqlite3, smbpasswd/pdbedit at runtime  [HEADLESS]
                │     └── nostr-smb-acquire, nostr-smb-mount (cifs-utils)   [HEADLESS]
                └── EXPERIMENTAL_ROAMING: nostr_homed_common ── GLib, jansson,
                      curl, sqlite3, secp256k1, nip19
                      ├── nostrfs ── FUSE3                                   [NOT headless]
                      └── nostr-homectl ── GLib                              [NOT headless today]
```

**Note the one genuine coupling:** `nostr_auth_core` links `nostr_nip46_core`
unconditionally (`provider_nip46.c`). `nostr_nip46_core` is a plain C static lib
(`_build/nips/nip46/libnostr_nip46_core.a`) and does **not** require GLib, so the
headless broker still has no desktop dependency — but it means the headless build
must keep `ENABLE_NIP46=ON`.

---

## 2. Proposed package split

### 2.1 Source-package strategy (first decision)

Three options:

| Option | Debian | Fedora | Trade-off |
|---|---|---|---|
| **A. One source, many binaries** (recommended) | source `nostrc` → ~25 binary packages | SRPM `nostrc` → ~25 subpackages | Single changelog/version; one build produces everything; **build-deps become the union** (GTK4+FUSE3+pam+libgit2 even for a headless-only rebuild) |
| **B. Split sources** | `nostrc` (core+headless) + `gnostr` (desktop) | two SRPMs from one tarball | Headless rebuilds stay light; but two changelogs from one upstream tarball is unusual and duplicates `Source0` |
| **C. One source + build profiles** | `nostrc` with `DEB_BUILD_PROFILES=pkg.nostrc.nogui` / `noinsttest` | Fedora `%bcond_with gui` | Best of both; standard mechanism in both distros; more `debian/rules` conditionals |

**Recommendation: A now, evolve to C.** Ship one source package. Runtime
independence — the maintainer's actual requirement — is a property of `Depends:`
fields, not of source packages: `libnss-nostr` can be installed on a headless box
with zero GTK on the system even though the *build host* had GTK4 installed. Add
build profiles (option C) once the headless stack is stable, so a server-only
COPR/PPA rebuild is cheap.

### 2.2 Debian/Ubuntu binary packages

Conventions applied: runtime shared-lib package = `lib<name><SOVERSION>`; matching
`lib<name>-dev`; NSS modules `libnss-<name>`; PAM modules `libpam-<name>`;
introspection `gir1.2-<Namespace>-<version>`; `Multi-Arch: same` for libs,
`Multi-Arch: foreign` for daemons/CLIs.

#### Headless — core libraries

| Package | Contents | Multi-Arch | GTK/GNOME? |
|---|---|---|---|
| `libnostr1` | `libnostr.so.1` | same | no |
| `libnostr-dev` | headers `/usr/include/nostr/`, `nostr.pc`, symlink | same | no |
| `libnostr-go0` *(name TBD, see D-3)* | `libnostrgo.so.0` | same | no |
| `libnostr-go-dev` | headers `/usr/include/go.h` + closure, `libgo.pc` | same | no |
| `libnostr-json1` | `libnostr-json.so.1` | same | no |
| `libnostr-json-dev` | headers, `nostr_json.pc` | same | no |
| `libmarmot0` | `libmarmot.so.0` | same | no |
| `libmarmot-dev` | headers `/usr/include/marmot/`, `marmot.pc` | same | no |
| `libhanami0` | `libhanami.so.0` | same | no |
| `libhanami-dev` | headers `/usr/include/hanami/`, `hanami.pc` | same | no |

NIP libraries: **do not ship as individual packages.** ~50 targets, mostly
`OBJECT` libs with no ABI story. Fold their compiled code into `libnostr1` (or a
single `libnostr-nips0`) and their headers into `libnostr-dev`. Revisit only if a
third-party consumer materialises. *(Decision D-6.)*

#### Headless — the nostr login stack

| Package | Contents | Notes |
|---|---|---|
| `libnss-nostr` | `/usr/lib/<triplet>/libnss_nostr.so.2`, conffile `/etc/nss_nostr.conf`, sample `/usr/share/nostr-homed/nss.conf.sample` | Arch: `any`, `Multi-Arch: same`. Must be excluded from `dh_makeshlibs`. |
| `libpam-nostr` | `/usr/lib/<triplet>/security/pam_nostr.so`, **`/usr/share/pam-configs/nostr`**, sample `/etc/pam.d/nostr-login` doc | Arch: `any`, `Multi-Arch: same`. `Depends: libpam-runtime (>= 1.0.1-6)` and calls `pam-auth-update --package` in postinst/prerm. |
| `nostr-authd` | `/usr/sbin/nostr-authd`, `/usr/lib/systemd/system/nostr-authd.service`, conffile `/etc/nostr-auth/auth.conf` | Arch: `any`, `Multi-Arch: foreign`. `dh_installsystemd`. |
| `nostr-homectl` | `/usr/bin/nostr-homectl`, `/usr/sbin/nostr-homed-seed` (renamed `nh-seed-authority`) | Arch: `any`. **Requires decoupling from roaming — see §6.4.** |
| `nostr-homed-smb` | `/usr/bin/nostr-smb-acquire`, `/usr/bin/nostr-smb-mount`, manpages | Arch: `any`. `Depends: cifs-utils`; `Recommends: samba-common-bin`; `Suggests: gvfs-backends`. |
| `nostr-homed-domain` | `/usr/share/nostr-homed/domain/*`, `/usr/libexec/nostr-homed/validate_domain_profile.py` | Arch: `all`. `Depends: python3`. Inert by design (never activates PAM). |
| **`nostr-login`** *(metapackage)* | nothing | Arch: `all`. `Depends: libnss-nostr, libpam-nostr, nostr-authd, nostr-homectl`; `Recommends: nostr-homed-smb`; `Suggests: nostr-homed-domain`. |

#### Headless — servers & tools

| Package | Contents |
|---|---|
| `nostr-relayd` | `/usr/bin/nostrc-relayd`, `/usr/share/nostrc/relay.toml.example`, systemd system unit *(to be added)* |
| `nostr-signet` | `/usr/bin/signetd`, `/usr/bin/signetctl`, `/usr/bin/signet-git-credential`, `signet.conf.example` |
| `blossom-cache` | `/usr/bin/blossom-cache` — GLib + libsoup3 + sqlite3, **no GTK** |
| `nostr-relayctl` / `grelay` | small CLI tools; candidates to fold into `nostr-relayd` |

#### Desktop — GObject / GTK layers

| Package | Contents |
|---|---|
| `libnostr-gobject-1.0-0` | `libnostr-gobject-1.0.so.1` |
| `libnostr-gobject-1.0-dev` | headers `/usr/include/nostr-gobject-1.0/`, `nostr-gobject-1.0.pc`, `/usr/share/gir-1.0/GNostr-1.0.gir` |
| `gir1.2-gnostr-1.0` | `/usr/lib/<triplet>/girepository-1.0/GNostr-1.0.typelib` |
| `libnostr-gtk-1.0-0` | `libnostr-gtk-1.0.so.1` — **GTK4 + libadwaita** |
| `libnostr-gtk-1.0-dev` | headers, `.pc`, `GNostrGtk-1.0.gir` |
| `gir1.2-gnostrgtk-1.0` | `GNostrGtk-1.0.typelib` |
| `libmarmot-gobject-1.0-0` / `-dev` / `gir1.2-marmot-1.0` | GLib-only (no GTK) — arguably headless-capable |

#### Desktop — applications

| Package | Contents |
|---|---|
| `gnostr` | `/usr/bin/gnostr`, gschema, desktop file, appdata, icons, search-provider ini |
| `gnostr-signer` | existing `debian/gnostr-signer.install` contents (bin, desktop, icons, gschema, metainfo) |
| `gnostr-signer-daemon` | existing `debian/gnostr-signer-daemon.install` (bin, D-Bus session service, systemd **user** unit) |
| `nostrfs` | `/usr/bin/nostrfs`, `org.nostr.Homed1.xml`, systemd **user** units — FUSE3, experimental |
| **`gnostr-desktop`** *(metapackage)* | `Depends: gnostr, gnostr-signer, gnostr-signer-daemon`; `Recommends: nostr-homed-smb` |

### 2.3 Fedora binary packages

Fedora convention: SONAME is **not** in the package name (it lives in the
auto-generated `Provides: libnostr.so.1()(64bit)`); introspection typelibs ship in
the main library package with an explicit `Provides: typelib(GNostr) = 1.0`; `.gir`
goes in `-devel`.

| Fedora package | Debian analogue | Headless? |
|---|---|---|
| `libnostr` / `libnostr-devel` | `libnostr1` / `libnostr-dev` | ✅ |
| `libnostr-go` / `libnostr-go-devel` | `libnostr-go0` / `-dev` | ✅ |
| `libnostr-json` / `libnostr-json-devel` | `libnostr-json1` / `-dev` | ✅ |
| `libmarmot` / `libmarmot-devel` | `libmarmot0` / `-dev` | ✅ |
| `libhanami` / `libhanami-devel` | `libhanami0` / `-dev` | ✅ |
| **`nss-nostr`** | `libnss-nostr` | ✅ |
| **`pam-nostr`** | `libpam-nostr` | ✅ |
| `nostr-authd` | `nostr-authd` | ✅ |
| `nostr-homectl` | `nostr-homectl` | ✅ |
| `nostr-homed-smb` | `nostr-homed-smb` | ✅ |
| `nostr-homed-domain` (noarch) | `nostr-homed-domain` | ✅ |
| **`nostr-login`** (noarch meta) | `nostr-login` | ✅ |
| `nostr-relayd` | `nostr-relayd` | ✅ |
| `nostr-signet` | `nostr-signet` | ✅ |
| `blossom-cache` | `blossom-cache` | ✅ |
| `nostr-gobject` / `nostr-gobject-devel` | `libnostr-gobject-1.0-0` + `gir1.2-gnostr-1.0` / `-dev` | ❌ (GLib) |
| `nostr-gtk` / `nostr-gtk-devel` | `libnostr-gtk-1.0-0` + `gir1.2-gnostrgtk-1.0` / `-dev` | ❌ |
| `marmot-gobject` / `-devel` | `libmarmot-gobject-1.0-0` / `-dev` | (GLib only) |
| `gnostr` | `gnostr` | ❌ |
| `gnostr-signer` / `gnostr-signer-daemon` | same | ❌ |
| `nostrfs` | `nostrfs` | ❌ |
| `gnostr-desktop` (noarch meta) | `gnostr-desktop` | ❌ |

Fedora naming notes worth a decision:
- NSS modules in Fedora are conventionally `nss-<name>` (`nss-mdns`, `nss-pam-ldapd`).
- PAM modules are inconsistent: older packages use underscores (`pam_ssh_agent_auth`,
  `pam_yubico`), newer guidance prefers hyphens (`pam-u2f`). **Recommend `pam-nostr`.**
- Fedora's `%{_libdir}` is `/usr/lib64`, which glibc's NSS loader does search, so
  the multiarch caveat in the CMake `WARNING` (lines 231–237) is satisfied by
  `%cmake`'s default prefix `/usr`. No change needed.

---

## 3. The headless "nostr login stack"

### What a headless admin installs

```
# Debian/Ubuntu
apt install nostr-login                    # pulls libnss-nostr, libpam-nostr,
                                           #       nostr-authd, nostr-homectl
apt install nostr-homed-smb                # + Samba/CIFS credential path
apt install nostr-homed-domain             # optional: inert winbind examples

# Fedora
dnf install nostr-login
dnf install nostr-homed-smb nostr-homed-domain
```

Resulting closure — **no GTK4, no libadwaita, no libsecret, no gvfs, no FUSE**
(and no GLib either, *if* `NOSTR_WITH_GLIB=OFF` is pinned for the headless
flavour — see B11/D-14):

```
nostr-login
 ├─ libnss-nostr      → libnostr1, libsqlite3-0, libssl3, libc6
 ├─ libpam-nostr      → libnostr1, libpam0g, libpam-runtime, libjansson4,
 │                      libsqlite3-0, libssl3, libsecp256k1-1, libwebsockets19
 ├─ nostr-authd       → same set + systemd
 └─ nostr-homectl     → libnostr1, libsqlite3-0, libssl3

   libnostr1          → libssl3, libsecp256k1-1, libwebsockets19, libnsync*,
                        libnostr-go0  [+ libglib2.0-0 unless NOSTR_WITH_GLIB=OFF]

nostr-homed-smb → cifs-utils (Depends), samba-common-bin (Recommends:
                  smbpasswd/pdbedit used by the tdbsam adapter),
                  gvfs-backends (Suggests: only for `nostr-smb-mount --mode gvfs`)
```

### What sits on top for GNOME

```
apt install gnostr-desktop     # gnostr + gnostr-signer + gnostr-signer-daemon
                               # → libnostr-gtk-1.0-0, libnostr-gobject-1.0-0,
                               #   gtk4, libadwaita, libpeas-2, json-glib,
                               #   libsoup3, libsecret, gir typelibs
```

The desktop bundle **may** co-install `nostr-login` (a GNOME workstation joined to
a nostr domain) but must never be required by it. Enforce this with a lintian/rpmlint
gate: `nostr-login`'s recursive `Depends` closure must contain no package whose name
matches `gtk|adwaita|gvfs|gnome-`. *(Proposed CI check — see §8.)*

---

## 4. Runtime vs build dependencies, per package

Legend: **[H]** heavy/desktop-only.

| Dependency | Debian build-dep | Debian runtime | Fedora build-dep | Used by |
|---|---|---|---|---|
| OpenSSL 3 | `libssl-dev` | `libssl3` | `pkgconfig(openssl) >= 3.0` | libnostr, marmot, identity/auth/SMB cores, relayd, signet |
| secp256k1 | `libsecp256k1-dev` | `libsecp256k1-1` | `pkgconfig(libsecp256k1)` | libnostr, relayd, roaming |
| jansson | `libjansson-dev` | `libjansson4` | `pkgconfig(jansson)` | libjson, auth core, roaming |
| SQLite3 | `libsqlite3-dev` | `libsqlite3-0` | `pkgconfig(sqlite3)` | identity, auth ratelimit, SMB journal, hanami, signet, gnostr |
| libwebsockets | `libwebsockets-dev` | `libwebsockets19` | `pkgconfig(libwebsockets)` | libnostr relay transport, relayd |
| **nsync** | `libnsync-dev` ⚠️ | `libnsync…` ⚠️ | `nsync-devel` | libgo, libnostr, libjson, relayd — **see D-1** |
| libsodium | `libsodium-dev` | `libsodium23` | `pkgconfig(libsodium)` | libmarmot, signet |
| libgit2 | `libgit2-dev` | `libgit2-1.x` | `pkgconfig(libgit2)` | libhanami |
| libcurl | `libcurl4-openssl-dev` | `libcurl4` | `pkgconfig(libcurl)` | hanami, signet, roaming |
| LMDB | `liblmdb-dev` | `liblmdb0` | `pkgconfig(lmdb)` | nostrdb (optional) |
| PAM | `libpam0g-dev` | `libpam0g`, `libpam-runtime` | `pam-devel` | `pam_nostr.so` |
| cifs-utils | — | `cifs-utils` (Depends of `nostr-homed-smb`) | `cifs-utils` | `nostr-smb-mount` |
| Samba tools | — | `samba-common-bin` (Recommends of `nostr-authd` when SMB is on) | `samba-common-tools` | tdbsam adapter shells to `smbpasswd`/`pdbedit` |
| FUSE 3 | `libfuse3-dev` **[H]** | `libfuse3-3`, `fuse3` | `fuse3-devel` | `nostrfs` only |
| Python 3 | `python3` | `python3` | `python3` | domain profile validator, configure-time checks |
| GLib 2 | `libglib2.0-dev` **[H]** | `libglib2.0-0` | `pkgconfig(glib-2.0)` | gobject layers, signet, blossom-cache, roaming — **and `libnostr` itself when `NOSTR_WITH_GLIB=ON` (the default)** |
| GTK4 | `libgtk-4-dev` **[H]** | `libgtk-4-1` | `pkgconfig(gtk4) >= 4.10` | nostr-gtk, apps |
| libadwaita | `libadwaita-1-dev` **[H]** | `libadwaita-1-0` | `pkgconfig(libadwaita-1) >= 1.3` | nostr-gtk, apps |
| json-glib | `libjson-glib-dev` **[H]** | `libjson-glib-1.0-0` | `pkgconfig(json-glib-1.0)` | apps, signet |
| libsoup3 | `libsoup-3.0-dev` **[H]** | `libsoup-3.0-0` | `pkgconfig(libsoup-3.0)` | gnostr, blossom-cache |
| libsecret | `libsecret-1-dev` **[H]** | `libsecret-1-0` + `gnome-keyring \| kwalletd5` | `pkgconfig(libsecret-1)` | gnostr-signer, gnostr |
| libpeas-2 | `libpeas-2-dev` **[H]** | `libpeas-2-0` | `pkgconfig(libpeas-2)` | gnostr plugins |
| GStreamer / qrencode / webkitgtk-6.0 | optional **[H]** | `Suggests:` | optional | gnostr media/QR/preview |
| p11-kit | `libp11-kit-dev` | `libp11-kit0` | `pkgconfig(p11-kit-1)` | gnostr-signer PKCS#11 (optional) |
| libmicrohttpd / libcbor / sqlcipher | optional | optional | optional | signet |
| gobject-introspection | `gobject-introspection`, `libgirepository1.0-dev` **[H]** | — | `gobject-introspection-devel` | GIR/typelib generation (`NOSTR_ENABLE_GI`, per-lib GIR blocks) |

**Heavy/desktop-only set:** GTK4, libadwaita, libpeas-2, libsecret, json-glib,
libsoup3, gvfs, GStreamer, webkitgtk-6.0, gobject-introspection, FUSE3.
None of these may appear in the `Depends`/`Requires` of any package in §3's first
list.

**GLib is deliberately *not* in that set.** It is a borderline case: GLib alone is a
plain C utility library with no display or session coupling, is in `Priority: standard`
on Debian, and is already required by `blossom-cache` and `signet`. The question is
narrower — should the *core* `libnostr1` carry it? See D-14.

---

## 5. Versioning + ABI policy

### 5.1 Current state (the blocker)

| Library | `SOVERSION` set? | Shared by default? | Shipped filename today |
|---|---|---|---|
| `libnostr` | ❌ **no** | ❌ static | `libnostr.a` |
| `libgo` | ❌ **no** | ❌ static | ⚠️ `liblibgo.a` (double prefix — target is named `libgo`) |
| `nostr_json` | ❌ **no** | ✅ hardcoded `SHARED` | `libnostr_json.so` (unversioned) |
| `hanami` | ❌ no | ❌ static | `libhanami.a` |
| `marmot` | ✅ `SOVERSION 0` | default | `libmarmot.so.0` |
| `nostr_gobject` | ✅ `SOVERSION ${MAJOR}` | default | `libnostr-gobject-1.0.so.1` |
| `nostr_gtk` | ✅ `SOVERSION ${MAJOR}` | default | `libnostr-gtk-1.0.so.1` |
| `marmot-gobject` | ✅ `SOVERSION ${MAJOR}` | default | — |
| `nip-communikeys` | ✅ `SOVERSION 2` | default | — |

Three problems for packaging:

1. **Unversioned shared objects cannot be shipped.** `libnostr_json.so` with no
   `SOVERSION` fails `dpkg-shlibdeps` cleanly and violates Debian Policy 8.1 and
   Fedora's shared-library guidelines.
2. **Default-static libraries cannot be split into runtime/`-dev` packages.** With
   `BUILD_SHARED_LIBS` unset, `libnostr`, `libgo`, `hanami`, `nostr_gobject`,
   `nostr_gtk`, `marmot` all build as `.a`. There is nothing to put in `libnostr1`.
3. **`liblibgo.a`** — the `-llibgo` in `libgo.pc` is consistent with the artifact
   but wrong as a distro library name, and `libgo` collides with **gccgo's
   `libgo`** in Debian (`libgo22` etc.). A rename is required before shipping.

`nostrc-ecrx` fixed the `BUILD_SHARED_LIBS=ON` link failure for `nostr_gobject`
(registration-seam bridges, commit `abee4510`); that unblocks item 2 but does not
by itself add SONAMEs.

### 5.2 Recommended policy

**P1 — Every installed library gets `VERSION` + `SOVERSION`.** Add to
`declare_component_version()` / `apply_versioning()` in `cmake/VersionHelpers.cmake`
so it is applied uniformly rather than per-file:

```
set_target_properties(<t> PROPERTIES
  VERSION   ${<NAME>_VERSION}          # e.g. 1.0.0
  SOVERSION ${<NAME>_VERSION_MAJOR})   # e.g. 1
```

For 0.x components (`libmarmot` 0.1.0, `libgo` 0.1.1, `libhanami` 0.1.0) treat
`SOVERSION = 0` and **bump it on every ABI break** until 1.0 — do not rely on the
minor field.

**P2 — Package name carries the SONAME (Debian only).** `libnostr.so.1` →
`libnostr1`. When the SOVERSION bumps, a **new** binary package name appears and
the old one may be co-installed; this is the whole point of the convention.
Fedora relies on auto-`Provides` and does not rename.

**P3 — `-dev` pins the runtime exactly.**
`Depends: libnostr1 (= ${binary:Version}), libnostr-go-dev, libssl-dev`.
Fedora: `Requires: %{name}%{?_isa} = %{version}-%{release}`.
Every `-dev` must also `Depends`/`Requires` the `-dev` of each package named in
its `.pc` `Requires:` field (e.g. `libnostr-json-dev` → `libjansson-dev`;
`libnostr-gtk-1.0-dev` → `libgtk-4-dev`, `libadwaita-1-dev`, `libnostr-gobject-1.0-dev`).

**P4 — Distro version ≠ component version.** A single source package has one
version. Use a **monorepo release version** (e.g. `0.3.0`) as the Debian/Fedora
`Version:`, and let per-library SONAMEs carry ABI meaning. `VERSION_MANIFEST.md`
and the `<component>-v<X.Y.Z>` tags remain the upstream semantic record. This is
the direct answer to the D2 acceptance criterion *"Version decision: declare
nostr-homed 0.2.0 in all authoritative sources … assess libnostr/signer/no-bump
decisions"*: declaring `nostr-homed 0.2.0` and shipping `nostrc 0.3.0-1` are
compatible, and `nostr-homed.pc` (which already emits `@PROJECT_VERSION@` = 0.2.0)
stays authoritative for the component. *(Decision D-2.)*

**P5 — Loadable modules are not versioned libraries.**
`pam_nostr.so` and `libnss_nostr.so.2` must be excluded from Debian's shlibs
machinery: `override_dh_makeshlibs: dh_makeshlibs -Xpam_nostr -Xlibnss_nostr`,
and `dh_shlibdeps` must still run against them so their own dependencies are
computed. `libnss_nostr.so.2`'s `.2` is glibc's **NSS interface version**, not a
SONAME — never bump it. Fedora: no `-devel`, no `Provides: libnss_nostr.so.2` filter
needed, but add `%global __provides_exclude ^libnss_nostr\\.so.*$` for hygiene.

**P6 — `.pc` naming.** `libnostr.pc.in` is installed as `nostr.pc` (module name
`nostr`), which is what `gnome/nostr-homed/CMakeLists.txt:56` consumes. Keep it,
but ship a `libnostr.pc` symlink for discoverability. `nostr_json.pc` uses an
underscore; if the library is renamed to `libnostr-json` (recommended), ship
`nostr-json.pc` and keep `nostr_json.pc` as a symlink for one release.

**P7 — Symbols files.** Start with `debian/*.shlibs` (coarse). Introduce
`debian/libnostr1.symbols` only after the first ABI-stable release; the churn rate
in `libnostr/include/` today would make a symbols file a constant merge conflict.

---

## 6. Install layout + distro integration that packaging must respect

These are already established in-tree and are **constraints, not choices**.

### 6.1 NSS module path (already correct — do not "fix")
`gnome/nostr-homed/CMakeLists.txt:228` installs `nss_nostr` to
`${CMAKE_INSTALL_LIBDIR}` with an explicit `message(WARNING)` when the resolved
path is not one glibc searches. glibc `dlopen`s `libnss_nostr.so.2` by bare name
from the standard runtime path only:

- Debian/Ubuntu: `/usr/lib/<triplet>` → `debian/rules` **must** pass
  `-DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=/usr/lib/$(DEB_HOST_MULTIARCH)`
  (the existing `apps/gnostr-signer/packaging/debian/rules` already does exactly this).
- Fedora: `%cmake` gives `/usr/lib64`. Fine as-is.
- **Never** `/usr/local/lib`, **never** `.../security/`.

### 6.2 PAM module path
`NH_PAM_MODULE_DIR` defaults to `${CMAKE_INSTALL_LIBDIR}/security` and is a cache
variable, so both distros work without patching. Debian:
`/usr/lib/<triplet>/security/pam_nostr.so`. Fedora: `/usr/lib64/security/pam_nostr.so`.

### 6.3 PAM activation — the biggest policy gap

`gnome/nostr-homed/packaging/pam/gdm-password.sample` is a **complete replacement**
for Debian's `gdm-password` conffile, owned by `gdm3`. A package must never
overwrite another package's conffile. Both distros have the right mechanism:

- **Debian: `pam-auth-update`.** Ship `/usr/share/pam-configs/nostr` declaring
  `Name`, `Default: no`, `Priority`, and `Auth:`/`Account:` blocks carrying exactly
  the semantics the sample documents —
  `[success=end new_authtok_reqd=ok user_unknown=ignore authinfo_unavail=ignore default=die]`
  — then `pam-auth-update --package` in `postinst` and
  `pam-auth-update --package --remove nostr` in `prerm`. This composes into
  `common-auth`, which `gdm-password` already `@include`s, so **no GDM file is
  touched at all**. `Default: no` preserves the "activation is an explicit admin
  act" posture and gives a clean rollback (the D2/D9 acceptance criterion
  *"activation refuses unknown modified PAM layouts; rollback order is frozen"*).
- **Fedora: `authselect`.** Ship a vendor feature under
  `/usr/share/authselect/vendor/nostr/` and document
  `authselect enable-feature with-nostr`. Do **not** edit `/etc/pam.d/*` from `%post`.
- Keep `packaging/pam/nostr-login.sample` as a real shipped file at
  `/etc/pam.d/nostr-login` (a service **we** own) — it is the `pamtester` proof
  service and is safe to ship as a conffile.
- `packaging/pam/gdm-password.sample` stays in `/usr/share/doc/libpam-nostr/examples/`
  as documentation only.

### 6.4 `nostr-homectl` and the seeder — currently unshippable
- `nostr-homectl` is built **only** under `NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING`
  and links `nostr_homed_common` (GLib, curl, FUSE-adjacent). The maintainer wants it
  as a headless identity-authority CLI. **Required work:** split `nostr-homectl` out
  of the roaming block, or add a `NOSTR_HOMED_ENABLE_CTL` flag producing a GLib-free
  identity/admin CLI on top of `nostr_identity_core`.
- `nh-seed-authority` is built only when `NOSTR_HOMED_BUILD_TESTS=ON` and has **no
  install rule**, yet `docs/INSTALLED_LOGIN.md` §2 makes it the mandatory
  provisioning step. **Required work:** promote it to an installed
  `/usr/sbin/nostr-homed-seed` under `AUTH_INSTALL`, independent of `BUILD_TESTS`.

Without both, `nostr-login` installs a broker that no admin can seed.

### 6.5 systemd
`systemd/nostr-authd.service.in` is installed to `pkg_get_variable(systemd systemdsystemunitdir)`
→ `/usr/lib/systemd/system`. Hardened: `RuntimeDirectory=nostr-auth` with
`RuntimeDirectoryMode=0711`, `StateDirectory=nostr-auth` at `0700`,
`ProtectSystem=strict`, `RestrictAddressFamilies=AF_UNIX`, `MemoryDenyWriteExecute=yes`.

Packaging consequences:
- **Do not ship `/var/lib/nostr-auth` or `/run/nostr-auth` as packaged directories** —
  systemd creates them with the correct modes. Shipping them risks wrong ownership.
- `ExecStart` is `configure_file`-substituted and its **argument count differs by
  build flavour** (2-arg AUTH-only vs 4-arg with SMB). A single source package builds
  once, so the shipped unit will be the SMB 4-arg form. `nostr-smb-acquire` lives in
  a *separate* package (`nostr-homed-smb`) that an admin may not install. The 4-arg
  daemon tolerates this (it warns and ignores the user socket) — acceptable, but
  **decide explicitly** (D-5): ship the 4-arg unit always, or ship a drop-in
  `/usr/lib/systemd/system/nostr-authd.service.d/10-smb.conf` from `nostr-homed-smb`
  that adds the SMB arguments, keeping the base unit 2-arg.
- Debian: `dh_installsystemd --no-start --no-enable` (an auth broker must not
  auto-start before it is seeded). Fedora: `%systemd_post`/`%preun`/`%postun`.
- Socket activation is listed as remaining D9 work and is **out of scope** for the
  first packages.

### 6.6 Configuration files
| File | Owner package | Type |
|---|---|---|
| `/etc/nss_nostr.conf` | `libnss-nostr` | conffile / `%config(noreplace)` |
| `/etc/nostr-auth/auth.conf` | `nostr-authd` | conffile / `%config(noreplace)` |
| `/etc/pam.d/nostr-login` | `libpam-nostr` | conffile |
| `/usr/share/pam-configs/nostr` | `libpam-nostr` | **not** a conffile (pam-auth-update input) |
| `/usr/share/nostr-homed/{auth,nss}.conf.sample` | respective packages | docs |

⚠️ Inconsistency to resolve: `config/` contains **both** `nss.conf.sample` and
`nss_nostr.conf.sample`, but CMake installs only `nss.conf.sample`, while the module
reads `/etc/nss_nostr.conf`. Pick one before packaging.

### 6.7 nsswitch.conf activation
`docs/INSTALLED_LOGIN.md` §3 uses `sed -i` on `/etc/nsswitch.conf`. That is fine for a
lab but is a policy question for a package:
- Debian has no `dh_installnss`; `libnss-*` packages either prompt via debconf
  (`libnss-ldap`) or document the edit (`libnss-systemd` uses a postinst helper).
- Fedora delegates entirely to `authselect`.

**Recommendation:** do **not** edit `nsswitch.conf` from maintainer scripts in v1.
Ship `nostr-homectl activate-nss` / documentation, and revisit debconf later.
*(Decision D-7.)*

### 6.8 `BUILD_SHARED_LIBS` linkage
`nostrc-ecrx` (commit `abee4510`, "registration-seam bridges for shared build
linkage") removed the layering violation where `nostr-gobject` referenced
`gnostr_signer_*` symbols defined in `apps/gnostr`. Packaging depends on
`-DBUILD_SHARED_LIBS=ON` succeeding for the **whole tree**; add this as a CI gate
before phase 2 (§8).

---

## 7. Build-system recommendation

### 7.1 What exists

- `apps/gnostr-signer/packaging/debian/` — a **complete, working** debhelper 13 dir:
  `control` (2 binary packages), `rules` (`dh $@ --buildsystem=cmake`, correct
  `CMAKE_INSTALL_LIBDIR=/usr/lib/$(DEB_HOST_MULTIARCH)`, `dh_installsystemd --user`,
  `dh_shlibdeps --ignore-missing-info`), per-package `.install`, `postinst`/`postrm`/
  `prerm`, `triggers`, `manpages`, `copyright`, `watch`, `source/format`,
  `upstream/metadata`. This is good Debian work and is the right skeleton.
- `apps/gnostr-signer/packaging/rpm/gnostr-signer.spec` — a real Fedora spec with
  `pkgconfig()`-style `BuildRequires`, a `daemon` subpackage, `%cmake`/`%cmake_build`/
  `%cmake_install`, `systemd-rpm-macros`. Its `%prep` is already
  `%autosetup -n nostrc-%{version}` — i.e. **it already builds from the monorepo
  tarball**, which is exactly the model §2.1 option A proposes.
- `packaging/archlinux/PKGBUILD` — `pkgbase=gnostr`, `pkgname=('gnostr' 'gnostr-signer')`:
  a split-package build from one source. Proof the split model works.
- `packaging/homebrew/`, `apps/gnostr-signer/packaging/{macos,windows,flatpak,appimage}`,
  `apps/gnostr-signer/daemon/packaging/{aur,homebrew,tar}` — non-distro channels.
- **No CPack anywhere in the tree.**

### 7.2 Recommendation

> **Generalise the existing `gnostr-signer` debian/ and spec into a single
> top-level `packaging/debian/` and `packaging/rpm/nostrc.spec`. Do not introduce
> CPack for distro packages. Do not invent a new scheme.**

Rationale:

1. **Native `debian/` + `.spec` are required, not optional.** CPack's DEB/RPM
   generators cannot express: `pam-auth-update` integration, `Multi-Arch: same`,
   `dh_makeshlibs` exclusions for NSS/PAM modules, shlibs/symbols, triggers,
   `%config(noreplace)`, `%systemd_post`, conffile handling, or Debian's
   `${shlibs:Depends}` substvars. Everything in §5–§6 is outside CPack's model.
2. **The per-app `debian/` dirs are a fiction that should be retired.**
   `apps/gnostr-signer/packaging/debian/control` declares `Source: gnostr-signer`
   with its own changelog, yet `rules` configures the *root* CMake project. Two
   such dirs (gnostr-signer plus a future gnostr) would race over the same build.
   Collapse to one source package; **keep the per-binary `.install`/`.postinst`
   content verbatim** as the file lists for the `gnostr-signer` /
   `gnostr-signer-daemon` binary packages.
3. **Keep the existing conventions** that are already right: debhelper-compat 13,
   `dh --buildsystem=cmake`, Ninja, `hardening=+all`, `Rules-Requires-Root: no`,
   `pkgconfig()`-style Fedora `BuildRequires`, `%cmake` macros.
4. **Where to put it.** Debian's own tooling expects `debian/` at the source root.
   Options: (a) a top-level `debian/` in-tree; (b) keep `packaging/debian/` and have
   `debian/` be a symlink or a `gbp` `--git-debian-branch` overlay; (c) a separate
   `pkg/` branch. Recommend **(a) top-level `debian/`** for the first cut — it is
   what `sbuild`/`dpkg-buildpackage`/`gbp` expect with zero glue — and keep
   `packaging/rpm/nostrc.spec` next to the existing spec. *(Decision D-8.)*
5. **CPack keeps a narrow, legitimate role:** `CPack TGZ` for the release tarball and
   the existing macOS/Windows bundling. Not for `.deb`/`.rpm`.
6. **One build, many flavours.** A single `debian/rules` `override_dh_auto_configure`
   must turn **everything** on, because the split happens at `.install`-glob time:

   ```
   -DCMAKE_INSTALL_PREFIX=/usr
   -DCMAKE_INSTALL_LIBDIR=/usr/lib/$(DEB_HOST_MULTIARCH)
   -DBUILD_SHARED_LIBS=ON
   -DENABLE_NOSTR_HOMED=ON
     -DNOSTR_HOMED_ENABLE_AUTH_CORE=ON -DNOSTR_HOMED_ENABLE_IDENTITY_CORE=ON
     -DNOSTR_HOMED_ENABLE_NSS=ON -DNOSTR_HOMED_ENABLE_AUTH_RUNTIME=ON
     -DNOSTR_HOMED_ENABLE_PAM=ON -DNOSTR_HOMED_ENABLE_SMB=ON
     -DNOSTR_HOMED_ENABLE_DOMAIN_CONFIG=ON -DNOSTR_HOMED_ENABLE_AUTH_INSTALL=ON
   -DBUILD_NOSTR_GOBJECT=ON -DBUILD_NOSTR_GTK=ON -DNOSTR_ENABLE_GI=ON
   -DBUILD_APPS=ON -DBUILD_LIBHANAMI=ON -DBUILD_LIBMARMOT=ON
   -DSIGNET_ENABLE=ON -DBUILD_RELAYD=ON -DBUILD_TESTING=OFF
   ```

   For the **phase-1 headless-only** build (§8), use exactly the flag set already
   proven in `docs/INSTALLED_LOGIN.md` §1 (`BUILD_NOSTR_GTK=OFF`, `BUILD_APPS=OFF`,
   `BUILD_LIBHANAMI=OFF`, `SIGNET_ENABLE=OFF`, `WITH_NOSTRDB=OFF`, …) — a build
   profile, not a second source package.

7. **Meson parity.** D9's acceptance criteria require Meson and CMake to expose
   identical features. Packaging should drive **CMake only** (it is the complete
   build); Meson parity remains a separate D9 obligation and must not become a
   packaging fork.

---

## 8. Phased rollout

### Pre-phase — blockers to clear before any `.deb` is built

| # | Blocker | Where | Owner |
|---|---|---|---|
| B1 | Add `VERSION`/`SOVERSION` to every installed library via `apply_versioning()` | `cmake/VersionHelpers.cmake` + per-lib | D |
| B2 | Rename `libgo` artifact (`liblibgo.a` → `libnostrgo`) and resolve the gccgo `libgo` name collision | `libgo/CMakeLists.txt`, `libgo.pc.in` | D |
| B3 | `BUILD_SHARED_LIBS=ON` must build the whole tree green (follow-on to `nostrc-ecrx`) | tree-wide | D |
| B4 | Install `nh-seed-authority` as `/usr/sbin/nostr-homed-seed` outside `BUILD_TESTS` | `gnome/nostr-homed/CMakeLists.txt` | D |
| B5 | Decouple `nostr-homectl` from `EXPERIMENTAL_ROAMING`/GLib | `gnome/nostr-homed/` | A/D |
| B6 | Author `/usr/share/pam-configs/nostr` profile matching the `gdm-password.sample` semantics | `gnome/nostr-homed/packaging/pam/` | B/D |
| B7 | Resolve `nss.conf.sample` vs `nss_nostr.conf.sample`; ship a real `/etc/nss_nostr.conf` | `gnome/nostr-homed/config/` | A |
| B8 | Confirm **nsync** availability in Debian/Ubuntu archives; if absent, vendor or convert to an embedded build | root, `libgo`, `libjson`, `relayd` | D |
| B9 | Give `signet`/`relayd` `GNUInstallDirs` destinations (currently relative `bin`) | `signet/`, `apps/relayd/` | D |
| B10 | Add a systemd **system** unit for `nostr-relayd` (none exists) | `apps/relayd/` | D |
| B11 | **Pin `NOSTR_WITH_GLIB` explicitly in every packaging build.** Today it is `ON` by default *and* conditional on `GLIB_FOUND`, so the produced `libnostr` ABI and its `nostr.pc` `Requires:` differ by build host — non-reproducible and silently GLib-coupled | `libnostr/CMakeLists.txt:19,85,96,189,387` | D |

### Phase 1 — headless login stack (maintainer priority)

Cut, for **amd64** first: `libnss-nostr`, `libpam-nostr`, `nostr-authd`,
`nostr-homectl`, `nostr-homed-smb`, `nostr-homed-domain`, metapackage `nostr-login`.
Plus the minimum core runtime they link (`libnostr1`, `libnostr-go0`,
`libnostr-json1`) and their `-dev` counterparts.

Exit criteria (extending D9's):
- `sbuild` on Debian 13 / Ubuntu 24.04 **amd64** produces all packages; `lintian -EI`
  clean of errors.
- `mock` on Fedora 41 amd64 produces the `nss-nostr` / `pam-nostr` / `nostr-authd`
  set; `rpmlint` clean of errors.
- On a clean amd64 VM with **no** GTK/GNOME installed: `apt install ./nostr-login*.deb`
  succeeds; `getent passwd n_alice` resolves through the packaged NSS module at
  `/usr/lib/x86_64-linux-gnu/libnss_nostr.so.2`; `pamtester nostr-login n_alice
  authenticate` passes via the packaged PAM module; `systemctl start nostr-authd`
  activates with the packaged unit. (This is the arm64 proof recorded in
  `nostrc-rb0e.10` notes, re-run on amd64 from packages instead of `cmake --install`.)
- `pam-auth-update --package` enables and `--remove` cleanly reverts; `/etc/pam.d/gdm-password`
  is byte-identical before and after.
- **Dependency-purity gate:** `apt-rdepends nostr-login` contains no
  `gtk|adwaita|gvfs|gnome-|libsecret|libpeas|webkit|gstreamer|fuse` package. Wire
  this into CI. Whether `libglib2.0-0` is permitted in that closure is D-14.

### Phase 2 — core shared libraries + `-dev`
`libnostr1`/`-dev`, `libnostr-go0`/`-dev`, `libnostr-json1`/`-dev`, `libmarmot0`/`-dev`,
`libhanami0`/`-dev`. Depends on B1/B2/B3. Adds `.pc` correctness checks
(`pkg-config --cflags --libs nostr` from a clean chroot with only `-dev` installed).

### Phase 3 — GObject + introspection
`libnostr-gobject-1.0-0`, `gir1.2-gnostr-1.0`, `libnostr-gtk-1.0-0`,
`gir1.2-gnostrgtk-1.0`, `libmarmot-gobject-1.0-0` + Fedora equivalents. Requires
`NOSTR_ENABLE_GI=ON` and the per-lib GIR blocks to run in the buildd (they need the
shared wrapper libs, which requires B3).

### Phase 4 — desktop applications
`gnostr`, `gnostr-signer`, `gnostr-signer-daemon`, `gnostr-desktop` meta. Reuses the
existing `apps/gnostr-signer/packaging/debian/*.install`, `*.postinst`, `*.triggers`,
`*.manpages` verbatim under the unified source package. Retires the per-app
`debian/control` and `debian/changelog`.

### Phase 5 — servers and experimental
`nostr-relayd`, `nostr-signet`, `blossom-cache`, `nostrfs`.

### amd64 preparation notes
- Multiarch triplet `x86_64-linux-gnu`; NSS module lands at
  `/usr/lib/x86_64-linux-gnu/libnss_nostr.so.2`, PAM at `.../security/pam_nostr.so`.
  Everything in `debian/*.install` must use `usr/lib/*/…` globs or
  `${DEB_HOST_MULTIARCH}` substitution — **never** hardcode `aarch64-linux-gnu`.
- Fedora amd64: `%{_libdir}` = `/usr/lib64` (vs `/usr/lib` on some arches) — use the
  macro, never a literal.
- The existing proof runs were Ubuntu 24.04 **arm64**. Re-run the full
  `docs/INSTALLED_LOGIN.md` matrix on amd64 and record it as the D9 evidence artifact.
- CI: `sbuild`/`pbuilder` chroots for Debian 13 + Ubuntu 24.04 amd64; `mock` for
  Fedora 41/42 amd64. Add a `BUILD_SHARED_LIBS=ON` job (B3 gate) and the
  dependency-purity job.

---

## 9. Recommended package list (summary table)

| # | Debian | Fedora | Contents | Headless | Phase |
|---|---|---|---|---|---|
| 1 | `libnostr1` / `libnostr-dev` | `libnostr` / `-devel` | core protocol lib | ✅ | 2 |
| 2 | `libnostr-go0` / `libnostr-go-dev` | `libnostr-go` / `-devel` | libgo concurrency (renamed) | ✅ | 2 |
| 3 | `libnostr-json1` / `libnostr-json-dev` | `libnostr-json` / `-devel` | jansson interop | ✅ | 2 |
| 4 | `libmarmot0` / `libmarmot-dev` | `libmarmot` / `-devel` | MLS + Nostr | ✅ | 2 |
| 5 | `libhanami0` / `libhanami-dev` | `libhanami` / `-devel` | Blossom + libgit2 | ✅ | 2 |
| 6 | **`libnss-nostr`** | **`nss-nostr`** | `libnss_nostr.so.2` + `/etc/nss_nostr.conf` | ✅ | **1** |
| 7 | **`libpam-nostr`** | **`pam-nostr`** | `pam_nostr.so` + `/usr/share/pam-configs/nostr` | ✅ | **1** |
| 8 | **`nostr-authd`** | **`nostr-authd`** | broker sbin + systemd unit + `auth.conf` | ✅ | **1** |
| 9 | **`nostr-homectl`** | **`nostr-homectl`** | identity CLI + `nostr-homed-seed` | ✅ | **1** |
| 10 | **`nostr-homed-smb`** | **`nostr-homed-smb`** | `nostr-smb-acquire`, `nostr-smb-mount` | ✅ | **1** |
| 11 | `nostr-homed-domain` | `nostr-homed-domain` | winbind samples + validator (arch: all) | ✅ | 1 |
| 12 | **`nostr-login`** *(meta)* | **`nostr-login`** *(meta)* | 6+7+8+9 (+10 Recommends) | ✅ | **1** |
| 13 | `nostr-relayd` | `nostr-relayd` | relay server + unit | ✅ | 5 |
| 14 | `nostr-signet` | `nostr-signet` | `signetd`, `signetctl`, git credential helper | ✅ | 5 |
| 15 | `blossom-cache` | `blossom-cache` | local Blossom cache server (GLib, no GTK) | ✅ | 5 |
| 16 | `libnostr-gobject-1.0-0` / `-dev` | `nostr-gobject` / `-devel` | GObject bindings | ❌ | 3 |
| 17 | `gir1.2-gnostr-1.0` | *(in `nostr-gobject`)* | `GNostr-1.0.typelib` | ❌ | 3 |
| 18 | `libnostr-gtk-1.0-0` / `-dev` | `nostr-gtk` / `-devel` | GTK4/libadwaita widgets | ❌ | 3 |
| 19 | `gir1.2-gnostrgtk-1.0` | *(in `nostr-gtk`)* | `GNostrGtk-1.0.typelib` | ❌ | 3 |
| 20 | `libmarmot-gobject-1.0-0` / `-dev` / `gir1.2-marmot-1.0` | `marmot-gobject` / `-devel` | Marmot GObject bindings | (GLib only) | 3 |
| 21 | `gnostr` | `gnostr` | GTK4 Nostr client | ❌ | 4 |
| 22 | `gnostr-signer` | `gnostr-signer` | signer GUI | ❌ | 4 |
| 23 | `gnostr-signer-daemon` | `gnostr-signer-daemon` | D-Bus/UDS signing daemon (user unit) | ❌ | 4 |
| 24 | `nostrfs` | `nostrfs` | FUSE roaming home (experimental) | ❌ | 5 |
| 25 | **`gnostr-desktop`** *(meta)* | **`gnostr-desktop`** *(meta)* | 21+22+23 | ❌ | 4 |

---

## 10. Top open decisions for the maintainer

| ID | Decision | Options | Recommendation |
|---|---|---|---|
| **D-1** | **nsync availability.** `libgo`/`libnostr`/`libjson`/`relayd` all `find_library(nsync REQUIRED)`. The existing `debian/control` build-depends on `libnsync-dev`. Is that package actually in Debian/Ubuntu? Fedora ships `nsync-devel`. | (a) confirm it exists; (b) vendor nsync into `third_party/`; (c) replace with pthreads/futex primitives | **Verify first.** If absent from Debian, this blocks *every* package — resolve before phase 1. |
| **D-2** | **Version scheme.** Per-component versions (`libnostr` 1.0.0, `libgo` 0.1.1, `nostr-homed` 0.2.0) vs one distro source version. | (a) monorepo `Version:` + per-lib SONAMEs; (b) split sources so each keeps its own version | **(a).** Keeps `VERSION_MANIFEST.md` authoritative upstream; SONAMEs carry ABI. |
| **D-3** | **`libgo` name collision** with gccgo's `libgo` in Debian, plus the `liblibgo.a` artifact bug. | (a) rename lib to `libnostrgo` + pkg `libnostr-go0`; (b) keep name, accept collision risk | **(a).** Rename now, before anything ships. |
| **D-4** | **`nostr_json` naming.** Underscore in SONAME and `.pc`. | (a) rename to `libnostr-json` + `nostr-json.pc` (symlink old); (b) keep | **(a)**, with a compat symlink for one release. |
| **D-5** | **`nostr-authd` unit argument form.** SMB build emits the 4-arg `ExecStart`, but `nostr-homed-smb` is a separate package. | (a) always ship 4-arg (daemon tolerates it); (b) base unit 2-arg + a drop-in from `nostr-homed-smb` | **(b)** is cleaner; **(a)** is cheaper. Maintainer's call. |
| **D-6** | **NIP libraries as packages.** ~50 targets, mixed OBJECT/static, no ABI story. | (a) fold into `libnostr1`; (b) one `libnostr-nips0`; (c) per-NIP packages | **(a).** Revisit only for an external consumer. |
| **D-7** | **`nsswitch.conf` activation.** | (a) document only; (b) debconf prompt; (c) postinst `sed` | **(a)** for v1; Fedora uses `authselect` regardless. |
| **D-8** | **Where `debian/` lives.** | (a) top-level `debian/`; (b) `packaging/debian/` + gbp overlay; (c) separate packaging branch | **(a)** for the first cut. |
| **D-9** | **Retire the per-app `debian/` dirs?** `apps/gnostr-signer/packaging/debian/` declares its own source package but configures the root CMake project. | (a) collapse into one source package, keep the `.install`/maintainer scripts; (b) keep both | **(a).** Two `debian/control` files racing over one build tree is untenable. |
| **D-10** | **Build profiles now or later.** Headless-only rebuilds currently need the full GTK4 build-dep set. | (a) later (after phase 1); (b) now | **(a).** Runtime independence — the actual requirement — is already satisfied. |
| **D-11** | **`nostr-homectl` scope.** Roaming CLI vs headless identity-authority CLI. | (a) split into a GLib-free `NOSTR_HOMED_ENABLE_CTL`; (b) ship it in `nostrfs` and give `nostr-login` a different admin tool | **(a).** The maintainer explicitly wants it headless. |
| **D-12** | **Does `gnostr-desktop` depend on `nostr-login`?** | (a) no dependency, both co-installable; (b) `Recommends` | **(a).** A GNOME workstation should not silently install a PAM/NSS stack. |
| **D-13** | **Upstream targets.** Debian/Fedora archive submission vs PPA/COPR. | (a) PPA + COPR first; (b) go straight to NEW/Fedora review | **(a).** Several §8 blockers (nsync, symbols files, `watch` file accuracy) are hard archive gates. |
| **D-14** | **Does `libnostr1` carry GLib?** `NOSTR_WITH_GLIB=ON` is the default and links GLib PUBLIC, putting `glib-2.0`/`gobject-2.0` in `nostr.pc` `Requires:`. | (a) pin `ON` everywhere — one `libnostr1`, accepts `libglib2.0-0` in the headless closure; (b) pin `OFF` everywhere — smallest server footprint, but the GObject layers then need their own GLib bridge; (c) two ABIs (`libnostr1` + `libnostr-glib1`) | **(a)** unless the maintainer wants a strictly GLib-free server profile. (c) doubles the ABI surface for little gain. Whichever is chosen, **pin it** (B11) — the current auto-detection is non-reproducible. |

---

## Appendix A — Evidence index

| Claim | Source |
|---|---|
| NSS module → multiarch libdir, not `security/`; warns on bad prefix | `gnome/nostr-homed/CMakeLists.txt:210–240` |
| PAM module dir is an overridable cache var | `gnome/nostr-homed/CMakeLists.txt:531–533` |
| `AUTH_INSTALL` requires AUTH_RUNTIME+PAM+NSS (FATAL_ERROR) | `gnome/nostr-homed/CMakeLists.txt:39–45` |
| Headless components have no GLib dependency | GLib `pkg_check_modules` appears only in the `EXPERIMENTAL_ROAMING` block, `…CMakeLists.txt:348` |
| `nh-seed-authority` is test-gated and never installed | `…CMakeLists.txt:293–295` |
| `nostr-homectl` is roaming-gated | `…CMakeLists.txt:389–392` |
| Unit hardening, `RuntimeDirectoryMode=0711` | `gnome/nostr-homed/systemd/nostr-authd.service.in` |
| Install inventory of the login runtime | `gnome/nostr-homed/docs/INSTALLED_LOGIN.md` §1 |
| `gdm-password.sample` replaces a `gdm3`-owned conffile | `gnome/nostr-homed/packaging/pam/gdm-password.sample` |
| `libnostr`/`libgo`/`nostr_json`/`hanami` have no `SOVERSION` | `grep -n SOVERSION` across `*/CMakeLists.txt` |
| `liblibgo.a` double prefix | built artifacts in `_build/libgo/`, `build/libgo/` |
| `nostr_json` is hardcoded `SHARED` | `libjson/CMakeLists.txt:27` |
| `NOSTR_WITH_GLIB` defaults ON; GLib linked PUBLIC; lands in `nostr.pc` `Requires:` | `libnostr/CMakeLists.txt:19, 85, 96, 189–191, 387–391` |
| `nostr_nip46_core` (linked by the headless broker) is GLib-free; the GLib layer is a separate `nostr_nip46_glib` target | `nips/nip46/CMakeLists.txt:13, 26–35` |
| Existing correct multiarch `CMAKE_INSTALL_LIBDIR` in `debian/rules` | `apps/gnostr-signer/packaging/debian/rules` |
| Existing spec already builds from the monorepo tarball | `apps/gnostr-signer/packaging/rpm/gnostr-signer.spec` (`%autosetup -n nostrc-%{version}`) |
| Arch split-package precedent | `packaging/archlinux/PKGBUILD` (`pkgbase=gnostr`, two `pkgname`s) |
| No CPack anywhere | `grep -rn CPack` returns nothing outside `third_party` |
| `BUILD_SHARED_LIBS` layering fix | beads `nostrc-ecrx`, commit `abee4510` |
