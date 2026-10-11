# Zapstore (NIP-82) integration for the Linux desktop — design draft

**Status:** owner decisions Q1–Q7 recorded 2026-10-10 (§12); ready for review. **Tracking:** beads epic
`nostrc-k4p14` (first task: `nostrc-k4p14.2`, `nips/nip82`).

**Goal.** Let a GNOME or Ubuntu user find, install and update software that is
published on Nostr as NIP-82 events (the format Zapstore uses), through the software tools their distribution already gives them **and**
through our own branded store app, **Capuchin** (§4.3), with integrity and
publisher identity verified end to end. Secondarily, let nostrc publish its own
apps (Groundhog, Grotto, gnostr) and "ported" third-party apps the same way.

**Non-goals.** Replacing App Center or GNOME Software as the system's
package manager; Android/macOS/Windows assets; building a public mirror or a
centralised index.

## 1. Background and findings

### 1.1 NIP-82 as specified

Draft spec: <https://github.com/franzaps/nips/blob/applications/82.md>.

| Kind | Type | Purpose | Key tags |
|---|---|---|---|
| 32267 | addressable | Application | `d` app id (reverse-DNS recommended), `name`, `summary`, `icon`, `image`*, `t`*, `url`, `repository`, `a` (NIP-34 repo), `f`* platforms, `license` (SPDX); content = markdown description |
| 30063 | addressable | Release | `i` app id, `version`, `d` = `<app-id>@<version>`, `c` channel (`main`/`beta`/`nightly`/`dev`/custom), `e`* asset ids with relay hints; `created_at` MUST be the release date; content = markdown notes |
| 3063 | regular | Asset (derived from 1063) | `i`, `m` MIME, `x` SHA-256, `version`, `url`, `size`, `f`*, `min_platform_version`, `target_platform_version`, `supported_nip`*, `variant`, `commit`, `min_allowed_version`; Android-only `version_code`, `apk_certificate_hash` |

Appendix C lists the supported Linux MIME types as AppImage, Flatpak and ELF
executables (which SHOULD be static), and states that **generic archives
(`application/gzip`, `application/zip`) and dependency-based packages (`.deb`,
`.rpm`) are NOT supported**. Appendix D defines version ordering (SemVer 2.0.0,
then a segment-wise fallback). Appendix E allows `p`/`zap` attribution tags
when the signer is not the maintainer.

### 1.2 NIP-82 as deployed (relay.zapstore.dev, sampled 2026-10-10)

- **17 apps** carry `f=linux-x86_64`; all but Armada and nSite Deck are CLI
  tools (ngit, nak, jq, pandoc, fzf, yt-dlp, zola, zsp, phoenixd, …).
- **120 Linux assets**: 102 `application/gzip`, 5 `.flatpak`, 5 `.deb`,
  5 `.AppImage`, 3 `application/x-executable`. Only **buzz.armada.app** ships
  desktop packages (flatpak + deb + AppImage per release).
- The deployed format **diverges from the spec**:
  - `.deb` and gzip assets are published and accepted despite Appendix C.
  - Tags not in the spec are in use: `filename`, `executable` (path inside an
    archive), `variant`, `h`, and assets carry an `a` back-pointer to their
    32267.
  - Many events (e.g. `xq`, `jq`) are signed by the **Zapstore indexer key**
    `78ce6faa…307d`, which republishes upstream GitHub releases; only some
    apps (e.g. Armada, `781a1527…dae5`) are developer-signed.
  - Release `e` hints point at other relays (`wss://relay.ngit.dev`), so assets
    are not guaranteed to be on the catalog relay.
  - The relay also serves a legacy format (`pkg/events/legacy`: 32267 + 30063
    + kind 1063 file metadata).
- The relay rejects low-specificity filters ("filters are too vague"): a
  filter needs a score ≥ 3 (ids 10, search 3, authors 2, tags 2, kinds 1), so
  `kinds` alone is refused; `kinds + #f`, `kinds + authors` or `kinds + search`
  are accepted.

### 1.3 Desktop store landscape

- **GNOME Software** (Fedora, Debian GNOME, openSUSE, Arch, …) loads plugins
  from `${libdir}/gnome-software/plugins-<API>`; `main` (51.x) is API **23**.
  Headers and a `gnome-software.pc` are installed for out-of-tree plugins, but
  the ABI changes between releases with no stability promise.
- **Ubuntu App Center** (default since 23.10) is a Flutter front end over
  snapd and PackageKit/AppStream with **no plugin mechanism**. In 26.04 it can
  manage installed debs; it is unconfirmed whether it lists *uninstalled* apps
  from third-party apt repositories with DEP-11 metadata (spike S1, §9).
- **Snap** cannot carry a third-party catalog: App Center browses only the
  Snap Store; side-loaded snaps need `--dangerous` and get no updates.

### 1.4 What nostrc already has

- `libnostr/include/nostr-kinds.h`: `NOSTR_KIND_FILE_METADATA 1063` and
  `NOSTR_KIND_RELEASE_ARTIFACT_SETS 30063` (old name); **no 32267 / 3063**.
- `nips/nipb0` — Blossom protocol layer (URL builders, kind 24242 auth,
  descriptor parsing, no transport): the template for a protocol-only module.
- `nostr-gobject` — `GNostrPool` / `GNostrSubscription` (signal-driven
  `event` / `eose` / `closed`, multi-relay subscribe, NIP-42 auth handler,
  cache-query hook) and nostrdb storage.
- `gnome/libnostr-publish` — signing (Grotto / D-Bus signer) and checked
  publishing through the session relay.
- `gnome/nostr-dispatcher` — NIP-89 "no handler installed" banner that opens
  `appstream://<app-id>`; Groundhog's update handoff also targets `appstream:`.
- `debian/` unified source package, `packaging/rpm`, Arch, Nix, Flatpak
  recipes, and `docs/systemd-hardening.md`.
- **No NIP-82 implementation exists** in `nips/` or anywhere else.

## 2. Decisions

1. **Generic NIP-82, Zapstore as the default catalog.** Code and package names
   say "nostr-software" (decided, Q1); `wss://relay.zapstore.dev` is the
   default catalog relay, not a hard dependency. The user-facing store app has
   its own brand, Capuchin.
2. **One core, several front ends.** A protocol module (`nips/nip82`) and a
   GLib core library (`libnostr-software`) hold all parsing, trust, sync,
   download and verification. Front ends — the Capuchin app, the apt bridge, the
   GNOME Software plugin and a CLI — contain no protocol logic.
3. **Ubuntu goes through apt, not snap.** The apt bridge is the only route that
   reaches App Center and gives `apt upgrade` / unattended-upgrades for free.
4. **Accept what is deployed, label it.** The parser accepts deployed
   out-of-spec usage (debs, archives, extra tags) and records conformance
   flags. Published `.deb` assets are supported (decided, Q2); because they run
   maintainer scripts as root, installing one shows an explicit
   "runs setup as administrator" confirmation (§6).
5. **Never trust a relay, only a key and a hash.** Every byte installed is
   hash-checked against a `x` tag inside an event signed by a key the user's
   policy trusts.
6. **Event-driven sync.** Long-lived REQ subscriptions with an EOSE catch-up
   boundary, per the repo's Nostr protocol guidelines — no timers, no polling.

## 3. Architecture

```
            ┌─────────────────────────── front ends ────────────────────────────┐
            │ Capuchin app      nostr-software-apt   gs-plugin-nostr-software  CLI │
            │ (org.nostr.     (system service:     (GNOME Software,     (ops,  │
            │  Capuchin)         local apt repo)      Fedora/Debian)       CI)   │
            └───────────────┬──────────────────┬─────────────────────┬───────────┘
                            │                  │                     │
            ┌───────────────▼──────────────────▼─────────────────────▼───────────┐
            │ libnostr-software (GObject)                                         │
            │  NsCatalog (sync+cache) · NsTrustPolicy · NsFetcher (Blossom+hash) │
            │  NsApp / NsRelease / NsAsset models · NsAppstreamWriter (DEP-11/XML)│
            └───────┬───────────────────────┬─────────────────────┬──────────────┘
                    │                       │                     │
              nips/nip82              nostr-gobject          nips/nipb0
       (parse/build/validate,     (GNostrPool, subs,       (Blossom URLs,
        version compare,           nostrdb cache)           hash extract)
        platform/MIME matching)
```

### 3.1 `nips/nip82` — protocol module (new)

Protocol-only C, modelled on `nips/nipb0` (OBJECT library, public headers
under `include/nostr/nip82/`, CTest suite). No I/O.

- **Types:** `NostrNip82App`, `NostrNip82Release`, `NostrNip82Asset` (owned
  strings, `_free()`), plus `NostrNip82Conformance` flags
  (`OUT_OF_SPEC_MIME`, `UNKNOWN_TAG`, `LEGACY_1063`, `MISSING_RECOMMENDED`).
- **Parse:** from `NostrEvent*`; enforce required tags and duplicates the way
  the reference relay does (`pkg/events/*.go`), then classify conformance.
  Accept legacy kind 1063 assets read-only.
- **Validate links:** release `i` == app `d`; asset `i` == release `i`;
  `d` == `<i>@<version>`; asset ids in release `e` tags; **same pubkey across
  app, release and asset** unless an Appendix E `p` attribution says otherwise
  (reported, not silently accepted).
- **Version ordering:** Appendix D comparator, and a mapping to dpkg version
  syntax (`1.0.0-alpha` → `1.0.0~alpha`) so apt ordering agrees with NIP-82
  ordering; property tests check both orders agree.
- **Platform / MIME:** host platform id (`linux-x86_64`, `linux-aarch64`, …),
  `f`-tag matching, MIME → install method enum (`FLATPAK_BUNDLE`, `APPIMAGE`,
  `ELF`, `DEB`†, `ARCHIVE`†; † = out of spec).
- **Build:** event builders for 32267/30063/3063 so nostrc can publish its own
  releases (signed through `libnostr-publish`).
- **Filters:** helpers that produce relay-acceptable filters (always kinds +
  authors/tags/search), so front ends cannot emit "too vague" REQs.
- **Kind constants** in `libnostr/include/nostr-kinds.h`:
  `NOSTR_KIND_SOFTWARE_APPLICATION 32267`, `NOSTR_KIND_SOFTWARE_RELEASE 30063`
  (keep `NOSTR_KIND_RELEASE_ARTIFACT_SETS` as a deprecated alias),
  `NOSTR_KIND_SOFTWARE_ASSET 3063`.
- **Fixtures:** real signed events captured from relay.zapstore.dev (Armada
  app/release/assets, an indexer-signed CLI tool, a legacy 1063 asset) stored
  verbatim — like `third_party/nostrdb/testdata`, they cannot be edited without
  breaking signatures — plus synthetic negative cases.

### 3.2 `libnostr-software` — GLib core (new, `gnome/nostr-software/`)

- **NsCatalog** — owns a `GNostrPool` and a nostrdb cache directory.
  - *Trusted sync:* one subscription `{kinds:[32267,30063,3063], authors:[trusted…]}`
    plus `{kinds:[5], authors:[trusted…]}` for NIP-09 yanks; catch-up to EOSE,
    then stays live. Reconnect with backoff re-issues subscriptions with
    `since` = last seen `created_at` − skew.
  - *Discovery:* on demand, `{kinds:[32267], #f:[<host platform>]}` and NIP-50
    `search` for keyword search; results are untrusted until policy says
    otherwise.
  - *Relays:* default catalog relay, publisher NIP-65 relays, and `e`/`a`
    relay hints; NIP-11 probe for search support; dedupe by event id;
    addressable events resolved newest-`created_at`-wins per (kind, pubkey, d).
  - Emits `app-changed`, `release-added`, `app-yanked` GObject signals.
- **NsTrustPolicy** — decides, per publisher and asset type, *visible*,
  *installable*, *auto-update*:
    - `developer`: user-listed npubs (config file / D-Bus), plus maintainers
    vouched for by a trusted project's maintainer set (§7).
  - `porter`: keys that republish someone else's software — the Zapstore
    indexer key and the nostrc project key. **Trusted by default** (decided,
    Q3) and labelled "Republished by Zapstore" / "Ported by nostrc" in every
    UI. Users can untrust either.
  - `unknown`: discoverable in search with a warning, not installable without
    adding the key.
  - Root-executing assets (`.deb`) are installable from `developer` and
    `porter` keys, but the first install of each package name shows the
    administrator-setup confirmation; a per-publisher "don't ask again" grant
    (`allow-root`) is recorded. Auto-update of root-executing packages happens
    only for packages already confirmed once — including debs republished by
    porter keys (decided, Q7).
  - Rollback guard: never offer a version ordered below the installed one;
    honour `min_allowed_version`; channel opt-in for non-`main`.
  - Optional later: web-of-trust signal from the user's follow list (Grotto
    identity) — advisory only, never sufficient for `allow-root`.
- **NsFetcher** — libsoup 3 download with streaming SHA-256; tries the `url`
  tag, then the same hash on the publisher's BUD-03 server list (kind 10063)
  via `nostr_blossom_url_download()`; enforces `size` and a configurable
  ceiling; writes to a temp file and renames only after the hash matches.
- **NsAppstreamWriter** — maps NIP-82 to AppStream (§5) as DEP-11 YAML (apt)
  and AppStream catalog XML (GNOME Software / `swcatalog`).
- **NsInstaller** — a set of `NsBackend` implementations (interface modelled
  on COSMIC Store's backend trait, §4.3): Flatpak bundle install via libflatpak into the
  configured installation (below), AppImage install + desktop integration
  under `~/.local/share/nostr-software/`, static ELF into `~/.local/bin`.
  Deb installation goes through PackageKit (`InstallFiles`) or the apt bridge,
  never `dpkg` directly.
- **Flatpak scope is configurable** (decided, Q4). GSettings
  `org.nostr.Software flatpak-installation` = `user` (default) | `system` |
  `ask`. `system` uses libflatpak's system installation and the Flatpak
  system helper's polkit actions; `ask` shows the choice per install. The
  GNOME Software plugin instead follows GNOME Software's own
  `org.gnome.software install-bundles-system-wide` key, so it behaves like
  every other bundle there.

### 3.3 `nostr-software` CLI

`list`, `search`, `show`, `fetch --verify`, `trust add|remove|list`,
`export-appstream`. Used for operations, CI smoke tests and as the reference
consumer of the library before any store integration lands.

## 4. Front ends

### 4.1 `nostr-software-apt` — Ubuntu/Debian bridge (system service)

A systemd system service running as `_nostr-software`, hardened per
`docs/systemd-hardening.md`, that keeps a live `NsCatalog` subscription and
maintains a local signed apt repository:

```
/var/lib/nostr-software/apt/
  dists/nostr/{InRelease,Release}
  dists/nostr/main/binary-amd64/Packages(.xz)
  dists/nostr/main/dep11/{Components-amd64.yml.gz,icons-*.tar.gz}
  pool/…
/etc/apt/sources.list.d/nostr-software.sources   (Signed-By: local key)
/etc/apt/preferences.d/nostr-software             (pin priority 100)
```

- **Repository key.** Generated on first start; private key readable only by
  the service user. The signature attests "this machine's policy approved
  it", nothing more — the root of trust is the Nostr publisher key plus hash.
- **Pass-through debs** (out of spec, accepted per Q2): included for
  `developer` and `porter` publishers once the package has been confirmed
  (§6). Rejected if the package `Package:` name collides with a
  package in the distro archive, or if it declares `Replaces`/`Conflicts` on
  archive packages, unless explicitly overridden. The pin priority stops a
  Nostr package from silently replacing an archive package.
- **Wrapper debs** (spec-conformant assets): static ELF and AppImage assets
  are wrapped into generated `nostr-sw-<app-id>` packages with **no
  maintainer scripts**, installing to `/opt/nostr-software/<app-id>/` with a
  `/usr/bin` symlink (ELF) or desktop file + icon (AppImage, extracted at
  build time). gzip archives are wrapped only when the asset names an
  `executable` path and the publisher policy allows archives.
- **Flatpak assets** are skipped here (handled by GNOME Software or the CLI).
- **Updates** arrive as a new release event → repo regenerated → normal apt
  update/upgrade and unattended-upgrades pick them up. A NIP-09 deletion
  removes the version from the repo index (it does not uninstall).
- **Trust administration** through `nostr-software trust …` behind polkit
  (`org.nostr.software.manage-trust`), or drop-in files under
  `/etc/nostr-software/trust.d/`.
- **Rejected alternative:** an apt transport method (`nostr://`). Cleaner
  sources line, but it still needs signed indexes, moves network and Nostr
  parsing into apt's privileged path, and complicates DEP-11 delivery.

### 4.2 `gs-plugin-nostr-software` — GNOME Software plugin

Out-of-tree plugin against `gnome-software-dev`, built per distro release
(plugin API 23 today) and shipped as a deb/rpm, never as a Flatpak.

| GS vfunc | Behaviour |
|---|---|
| `setup_async` | open `NsCatalog`, start trusted sync |
| `refresh_metadata_async` | ensure catch-up reached EOSE; no polling |
| `list_apps_async` | keyword search (NIP-50 / cache), category via `t` tags, installed list |
| `refine_async` | fill description, icons, screenshots, releases, licence, developer name (kind 0), "verified" quirk for `developer`-tier keys |
| `install_apps_async` / `update_apps_async` / `uninstall_apps_async` | Flatpak bundle via libflatpak, AppImage via `NsInstaller` |
| `url_to_app_async` | `nostr:naddr1…` pointing at a 32267 → app page |
| `install/enable/disable/remove_repository_async` | the catalog relay appears as a repository in Software Repositories |
| `adopt_app` | claim apps whose management origin is ours |

Risks to settle in spike S2 (§9):
- **Double listing.** After `install_bundle` the stock flatpak plugin also sees
  the app; we need either a single owner (our plugin owns updates, flatpak
  plugin's copy deduplicated by AppStream id + origin) or to hand the app to
  the flatpak plugin entirely and lose Nostr-driven updates.
- **Runtimes.** Bundles without a `RuntimeRepo` cannot fetch their runtime;
  require Flathub to be configured or refuse with a clear message.
- **ABI churn.** CI builds the plugin against each supported distro's
  gnome-software version; packaging pins `gnome-software (>= X, << X+1)`.

### 4.3 Capuchin — the nostrc store app (`org.nostr.Capuchin`)

Our own branded store for Nostr-packaged software, in the role Zapstore's app
plays on Android. **Brand:** the infinite monkey theorem — enough monkeys at
typewriters eventually type the works of Shakespeare; here, enough independent
publishers produce a software catalog without a central authority. Icon: a
capuchin at a typewriter, the page in the platen showing an app grid. Tagline
candidates: "Infinite monkeys, finite trust." / "Software, typed by
everyone." Name decided (Q6).

- **Location and stack:** `gnome/capuchin/`, GTK 4 + libadwaita, C, on
  `libnostr-software`; follows Groundhog's structure (app id, metainfo,
  privacy charter, GUI tests with screenshot hooks).
- **Pages:**
  - *Explore* — featured apps (a curated NIP-51 set signed by the project
    key), new releases, categories from `t` tags.
  - *Search* — NIP-50 where the relay supports it, local cache otherwise.
  - *App page* — description, screenshots, release history and notes,
    licence, supported NIPs (`supported_nip`), available formats with
    conformance labels, and a **publisher card**: kind 0 profile, NIP-05
    check, trust-tier badge, "republished/ported by" attribution with
    Appendix E `p` credits.
  - *Installed / Updates* — one list across Flatpak, AppImage, ELF and deb,
    with a per-app channel (`main`/`beta`/…).
  - *Publishers* — trust management: add/remove npubs, porter keys,
    `allow-root` grants, followed maintainer sets (§7).
  - *Preferences* — catalog relays, Flatpak scope (`user`/`system`/`ask`),
    channels, auto-update, network/privacy settings.
- **Install paths:** NsInstaller for user-level formats; debs through
  PackageKit `InstallFiles` after the administrator-setup confirmation. When
  `nostr-software-apt` is installed, Capuchin hands debs to the apt repo instead,
  so their updates also flow through apt.
- **Nostr identity is optional.** Browsing and installing need no account.
  Signing in (Grotto or NIP-46) adds follow-graph trust hints (advisory only),
  zaps to publishers through `nostr-wallet-agent` (`org.nostr.Wallet1`,
  NIP-57, using the app event's `zap`/`p` tags), and later reactions and
  comments on releases.
- **Deep links:** registers with `nostr-dispatcher` as the NIP-89 handler for
  kinds 32267/30063, so `nostr:naddr1…` links open the app page; it is also
  the target of the dispatcher's "Install from Nostr" banner (§4.4).
- **Packaging:** deb and rpm first, since it must reach PackageKit and the
  Flatpak system helper. A Flatpak build needs `--talk-name=org.freedesktop.Flatpak`
  and PackageKit access, which Flathub reviews strictly; deferred until the
  native packages ship.
- **Self-hosting:** Capuchin is itself published as NIP-82 events (§7) and can
  update itself where it was installed from Nostr.

#### Design inspiration: COSMIC Store

System76's COSMIC Store ([user guide](https://system76.com/support/managing-apps/),
source `pop-os/cosmic-store`, Rust/libcosmic) is the closest existing model
for a multi-source Linux store. Capuchin borrows its **UX and structure, not
its code** (different toolkit and licence).

| COSMIC Store | Capuchin adaptation |
|---|---|
| Sidebar: Explore · Create · Work · Develop · Learn · Game · Relax · Socialize · Utilities · Installed apps · Updates | Same sidebar and category set; NIP-82 `t` tags and AppStream categories map onto the eight categories (unmapped → Utilities) |
| Explore sections: Editor's choice, Popular, New apps, Recently updated | *Curated by nostrc* (project-key NIP-51 set), *Popular* from public zap receipts (kind 9735) to the app's pubkey — no download telemetry, *New apps*, *Recently updated* (30063 `created_at`) |
| Per-app **source picker** listing every backend/remote ("Flathub", "Flathub (system)", "… (installed)") | Source = publisher × format × scope, e.g. "Flatpak · Armada (developer)", "Flatpak (system) · Armada", ".deb · Republished by Zapstore", "AppImage"; installed source marked "(installed)" |
| `priority()` picks the default source (flatpak-user, Flathub first) | `ns_source_priority()`: trusted developer > porter; then configured Flatpak scope > AppImage > deb > static ELF; only installable sources are offered |
| Separate flatpak-user and flatpak-system backends | Q4's `ask` mode is simply both Flatpak sources shown in the picker; `user`/`system` hide the other |
| `Backend` trait: `load_caches`, `info_caches`, `installed`, `updates`, `file_packages`, `operation` | `NsBackend` GInterface in `libnostr-software` with the same shape; implementations: flatpak-user, flatpak-system, packagekit (deb), appimage, elf |
| Update all; "N operations running (P%)" panel | Update all; operation queue showing **Download → Verify hash → Install** per item, so verification is visible, not implied |
| Uninstall dialog: Flatpak keeps data / "Permanently delete app data" | Same wording and checkbox per format (Flatpak `~/.var/app`, AppImage/ELF config dirs listed explicitly) |
| Opens when a `.deb` is downloaded | Opens for `nostr:naddr…` links and for **"Check who published this file"**: a local file's SHA-256 is looked up as a kind 3063 `#x` filter to show its publisher and trust tier |
| Manage repositories (Repoman: PPAs / Flatpak remotes tabs) | *Sources* page with two tabs: catalog relays, and trusted publishers / porter keys / `allow-root` grants |

Unlike COSMIC, Capuchin lists **only Nostr-published software**; the backend
interface would allow Flathub or distro sources later, but that is not
planned.

### 4.4 Smaller integrations

- **nostr-dispatcher:** when the NIP-89 banner finds no installed handler and
  an NIP-82 app with the same id exists in the catalog, offer "Install from
  Nostr" (opens Capuchin if installed, otherwise GNOME Software via the
  `nostr:` URL).
- **Groundhog update handoff:** treat a Nostr-installed Groundhog as its own
  channel in the `gh-update-handoff` channel metadata.

## 5. NIP-82 → AppStream mapping

| NIP-82 | AppStream |
|---|---|
| 32267 `d` | `<id>`; non-reverse-DNS ids (e.g. `jq`) become `nostr.<pubkey8>.<d>` to avoid collisions |
| asset MIME | `type`: Flatpak/AppImage/deb with icon → `desktop-application`; ELF/archive → `console-application` (with `<provides><binary>`) |
| `name`, `summary` | `<name>`, `<summary>` |
| content (markdown) | `<description>` (paragraphs/lists only; links and images dropped) |
| `icon` | remote `<icon type="remote">`; cached into DEP-11 icon tarballs for apt |
| `image`* | `<screenshots>` |
| `t`* | `<keywords>`; known values map to `<categories>` |
| `url`, `repository` | `<url type="homepage">`, `<url type="vcs-browser">` (nostr:// repos shown as text) |
| `license` | `<project_license>` |
| 30063 version / `created_at` / content | `<release version date>` + description |
| pubkey + kind 0 | `<developer id="nostr:npub…"><name>` |
| — | `<custom><value key="nostr::naddr">` for round-trips |

## 6. Security model

- **Integrity:** SHA-256 of every downloaded byte equals the `x` tag before
  anything is handed to an installer; size checked against `size` and a cap.
- **Authenticity:** app, release and asset signatures verified; publisher key
  must be trusted at the tier required for the asset type.
- **Privilege:** user-level formats (Flatpak bundle, AppImage) install without
  root (or through the Flatpak system helper when the scope is `system`).
  Deb installs run maintainer scripts as root, so the first install of each
  pass-through deb needs the administrator-setup confirmation (or a recorded
  per-publisher `allow-root` grant); wrapper debs have no scripts.
- **Porter events** (Zapstore indexer, nostrc project key) are trusted by
  default but always labelled as republished/ported, and get the same
  administrator-setup confirmation for debs as developer keys.
- **Rollback/freeze:** monotonic version rule, `min_allowed_version`, warning
  when a trusted publisher's newest release is far older than its app event.
- **Key compromise:** no key rotation in NIP-82; mitigation is fast
  revocation (remove from trust, NIP-09 yanks honoured) — tracked as an open
  spec issue.
- **Network:** relays and Blossom servers are untrusted transports; metadata
  fetches leak interest in apps to relays (documented; Tor/proxy settings
  inherited from the system).
- A Nostr protocol review (`nostr-protocol-smells`) and a security review are
  release gates for the core and both store front ends.

## 7. Publishing: maintainer keys and the project key

Decided (Q5): **nostrc's own apps are signed by their maintainers; a project
key signs "ported" apps** (third-party software nostrc repackages, the same
role Zapstore's indexer plays).

- **Maintainer keys.** Each component's release is signed by the releasing
  maintainer (Groundhog, Grotto, gnostr, Capuchin, …) through Grotto or a NIP-46
  bunker, never by a key held in CI. The release workflow builds the assets,
  uploads them to Blossom and produces unsigned 3063/30063/32267 events for
  the maintainer to sign; publishing checks every relay `OK`.
- **Maintainer set.** The project key publishes a NIP-51 set (kind 30000,
  `d=nostrc-maintainers`) listing current maintainer pubkeys, with a mapping
  of which maintainers may sign which app id. Trusting the project key lets
  clients accept nostrc apps from any listed maintainer for that app id;
  removing a maintainer from the set revokes them for new releases.
  Appendix E `p` tags credit co-maintainers.
- **Project key (porter).** Signs ported apps only, with Appendix E `p`
  attribution to the upstream author (and their `zap` split where known) and
  `repository`/`commit` tags pinning the exact upstream source. It never signs
  nostrc's own apps, so a compromise of one cannot impersonate the other. Held
  offline (hardware or an offline Grotto profile); CI never sees it.
- **Assets:** Flatpak bundle, AppImage and `.deb` (accepted per Q2) for each
  release; AppStream ids match the app's `d` tag.
- This grows the catalog the front ends depend on and doubles as the
  end-to-end test fixture.

## 8. Upstream (NIP-82 / Zapstore) items

1. Spec vs. deployment: `.deb` and archive assets are published; either the
   spec should admit them (with `executable`) or Zapstore should stop.
2. Undocumented tags in use: `filename`, `executable`, `variant`, `h`, asset
   `a` back-pointer.
3. Indexer-signed events: a standard marker distinguishing "republished by" from
   "published by" (Appendix E `p` is underused).
4. Key rotation / revocation for publishers.
5. AppStream id guidance (`d` SHOULD equal the AppStream component id on Linux).

## 9. Spikes (time-boxed, before committing to front ends)

- **S1 — App Center visibility (1–2 days).** Ubuntu 26.04 VM, local signed apt
  repo with DEP-11: does App Center list and install an *uninstalled*
  third-party desktop app? If not, the apt bridge still delivers apt updates
  and GNOME Software on Debian, but App Center discovery is lost.
- **S2 — GNOME Software ownership (2–3 days).** Fedora 4x VM: install a
  `.flatpak` bundle from a plugin; check double listing, update ownership and
  runtime resolution.
- **S3 — Relay behaviour (½ day).** Confirm NIP-50 search support, REQ limits,
  AUTH requirements and EOSE timing on relay.zapstore.dev with the exact
  filters `nips/nip82` emits.

## 10. Phasing and effort (rough)

| Phase | Deliverable | Effort |
|---|---|---|
| P0 | kind constants; `nips/nip82` with fixtures and tests | 1–1.5 wk |
| P1 | `libnostr-software` core + CLI | 2–3 wk |
| S1–S3 | spikes | ~1 wk |
| P2a | `nostr-software-apt` (pass-through + ELF/AppImage wrappers) | 2–3 wk |
| P2b | `gs-plugin-nostr-software` | 2–4 wk |
| P2c | Capuchin app: browse, search, app page, install, updates, publishers | 4–6 wk |
| P3 | maintainer/project-key publishing; dispatcher/Groundhog hooks | 1–2 wk |
| P4 | Capuchin: sign-in, zaps, follow-graph hints, release comments | 2–3 wk |

## 11. Versioning and packaging impact

- `libnostr`: new public kind constants + deprecated alias → MINOR-level
  change (libnostr has no authoritative version source yet; record the
  assessment in the bead).
- `nips/nip82` ships inside `libnostr-nips0`: new public headers → MINOR.
- New components in `VERSION_MANIFEST.md`, starting at `0.1.0`:
  `libnostr-software`, `nostr-software` (CLI), `nostr-software-apt`,
  `gs-plugin-nostr-software`, `capuchin`.
- New binary packages in `debian/`: `libnostr-software0`, `nostr-software`,
  `nostr-software-apt`, `gnome-software-plugin-nostr`, `capuchin`; RPM
  subpackages for the plugin and Capuchin; Arch/Nix recipes for Capuchin.

## 12. Owner decisions (2026-10-10)

| # | Question | Decision |
|---|---|---|
| Q1 | Naming | Generic **nostr-software** for code and packages; the store app is branded separately (Capuchin) |
| Q2 | Published `.deb` assets | **Accepted** as pass-through, with the administrator-setup confirmation (§6) |
| Q3 | Trust the Zapstore indexer by default | **Yes**, as a labelled `porter` key |
| Q4 | Flatpak user vs system | **Configurable**: `user` (default) / `system` / `ask` (§3.2) |
| Q5 | Release keys | **Per maintainer**, plus a **project key for ported apps** (§7) |
| Q6 | Store app name | **Capuchin** (`org.nostr.Capuchin`); Hamlet, Infinite Monkey, Typewriter and Folio were considered |
| Q7 | Republished (porter) debs | **Update automatically**, after the one-time confirmation on first install (§3.2) |

Design inspiration for the store app: COSMIC Store (§4.3).
