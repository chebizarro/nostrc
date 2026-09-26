# GVfs backends for Nostr — `blossom://` and `nostr://<npub>/files/`

**Status:** Design. **No implementation in this document.**
**Bead:** `nostrc-jaxi` (child of epic `nostrc-prqu` "Nostr as a first-class GNOME primitive"). Work items W0–W10 (§13) are bead-ready; none is filed yet.
**Reads:** `docs/investigations/gnome-gtk-nostr-planned-not-implemented-2026-09-26.md` (Cluster 4, §3 item 3), `docs/designs/nostrfs-porthome-overlay.md` (D1, D9, D11, §8, §14), `gnome/nostr-share/README.md`, `gnome/nostr-seal/README.md`, `gnome/libnostr-publish/include/nostr-publish/*.h`, `libhanami/include/hanami/{hanami-blossom-client,hanami-bud02-auth,hanami-server-capability}.h`, `apps/blossom-cache/README.md`, `gnome/nostr-dispatcher/README.md`, `apps/gnostr-signer/data/dbus/org.nostr.Signer.xml`, gvfs `daemon/{gvfsbackend.h,mount.c,meson.build}` and `NEWS` at master (2026-09-26).
**Date:** 2026-09-26 (revised the same day after an Oracle critique pass; see §17)
**Branch:** `design/gvfs-nostr-backends` (base `28e82491`)

> Decision numbering (**D1…D23**) is local to this document. The porthome overlay
> design's decisions are written **PHO-D1**, **PHO-D9**, etc.; the home-from-relay
> design's as **HFR-…**.

---

## 0. Executive summary

The repo has two FUSE trees and neither is "relays as a filesystem": both mount
*portable home* (§1). The bead asks for the GNOME-native way to put Nostr and Blossom
content in **Files**: two **GVfs backends** that run inside the user's existing
`gvfsd`, appear in the Files sidebar, need no `/dev/fuse`, no root, no systemd unit,
get a POSIX view under `$XDG_RUNTIME_DIR/gvfs/` from `gvfs-fuse` for free, and are
reachable from Flatpak apps that have gvfs access (or, for the rest, through the
document portal over that gvfs-fuse path).

| Backend | URI | Namespace | Writes |
| --- | --- | --- | --- |
| `gvfsd-blossom` | `blossom://<server>/` (own blobs, writable) · `blossom://<npub>@<server>/` (any pubkey's blobs, read-only unless it is the signer's identity) · `blossom:///` (virtual root: one **shortcut** per server in the user's kind-10063 list, BUD-03) | a pubkey's blobs on one BUD-02 server (`GET /list/<pubkey>`), named `<sha256>.<ext>` | **upload** (`PUT /upload`, kind-24242 auth via `org.nostr.Signer`) and **delete** (`DELETE /<sha256>`). Rename is refused (`G_IO_ERROR_PERMISSION_DENIED`, D4 explains why not `NOT_SUPPORTED`); no directories. |
| `gvfsd-nostr` | `nostr://<npub>/` | `profile.json` (kind 0), `relays.json` (kind 10002), `files/` = the user's **NIP-94 kind-1063 events** plus **NIP-92 `imeta`** attachments of their kind-1 notes, deduplicated by sha256 | **read-only for anyone else**; for the **current identity**, dropping a file uploads it to their kind-10063 servers and publishes a kind 1063 through `libnostr-publish`; deleting publishes a kind 5. |

Both backends:

- **hold no key material.** Every signature is `org.nostr.Signer.SignEvent` (nip55l);
  the daemon never sees an nsec. `NIP44DeriveConversationKey` exists on the signer
  now but is irrelevant here — nothing in either namespace is encrypted *by us*
  (`.nsealed` files are opaque blobs, §9);
- **verify everything they show**: event id + Schnorr signature for every relay
  event, sha256 for every blob before it is admitted to the cache (§5.3 states the
  one bounded window where bytes are served before the final hash check);
- **speak HTTPS only** to Blossom servers (loopback `http://` only behind an explicit
  setting, for `blossom-cache` and test fixtures);
- share one **content-addressed read-through cache** at `$XDG_CACHE_HOME/gvfs-nostr/`
  with LRU eviction (§5.4), and one private static library `libgvfsnostr-common`
  (§10) built on **libsoup 3** (already in `gvfsd-http`/`gvfsd-dav`'s closure),
  **libnostr-publish** (transports, NIP-65, publisher) and **libnostr** (verification,
  NIP-19). `libhanami` is *not* linked into the daemons (it drags libcurl + libgit2 +
  SQLite and its Blossom API is whole-buffer/synchronous); its pure BUD-02 validator
  is used by the test fixture server (§12).

The honest caveat, stated up front: **gvfs installs no headers for `GVfsBackend`**
(verified against `daemon/meson.build`: `libgvfsdaemon` is a `shared_library`
installed to `${libdir}/gvfs/`, no `install_headers`). Out-of-tree backends build
against a pinned gvfs source checkout and link the distro's private
`libgvfsdaemon.so`/`libgvfscommon.so` (Debian: package `gvfs-libs`); the package
must pin the **exact** gvfs version it was built against (D17). The end state is an
upstream MR to GNOME/gvfs (W10); until then this is the same position `gvfs-mtp` and
the OneDrive backend occupied before they were merged.

---

## 1. Ground truth — what exists today

Read 2026-09-26 on `master` (`28e82491`).

| Area | Where | Reality |
| --- | --- | --- |
| Legacy FUSE | `gnome/nostr-homed/src/fs/nostrfs.c` (954 lines) | FUSE 3 high-level ops (`nfs_getattr … nfs_read`, L537–L881) over a flat `nh_manifest` from the NSS cache; unencrypted CAS at `/var/cache/nostrfs/<uid>/<cid>`; writeback through a libgo actor + upload worker pool (`upload_req`, L48); NIP-98 kind-27235 auth (retired by HFR-D7); publishes kind 30081; hard-coded `wss://nos.lol,wss://nostr.wine` fallback (L273); `EXPERIMENTAL_ROAMING`-gated; CAS miss returns the literal `CID:<hex>` as content. Mounts **a home**, not relay content. |
| Phase-4 FUSE | `gnome/nostr-homed/src/porthome-fuse/nostr-home-fuse.c` (893 lines) | "EXPERIMENTAL AND UNREVIEWED" (L7). Read-only overlay of `snapshot.json`; every mutator `EROFS` (`rofs()`, L516–L542); `Type=notify` unit, `fuse.lock`, exit codes 3–6, no `allow_other`. Content = encrypted porthome chunks decrypted with `home_key`. Design `docs/designs/nostrfs-porthome-overlay.md`, bead `nostrc-1u55`. Mounts **a home**, not relay content. Its cache (`nh_syncd_cache`) is keyed by the sha of *sealed* chunks — not reusable for public blobs. |
| Share | `gnome/nostr-share/` (bead `nostrc-1xak`, closed) | `ns_blossom_upload()` (libhanami client, kind-24242 via signer, `https://` only, descriptor-hash check); `ns_net_fetch_replaceable()` (session relay socket → relays, **sig-verified**), `ns_resolve_targets()` (write relays + a session-relay copy, `session_included`), `ns_resolve_blossom_servers()` (kind 10063 → config fallback), `ns_tags_add_file_metadata()` (1063: `url,m,x,ox,size,dim?,alt?`), `ns_imeta_tag_new()`, `ns_blossom_blob_url()` (mime→ext), `ns_strip_metadata()` (JPEG/PNG/WebP/GIF), config `~/.config/nostr-share/nostr-share.conf` (`home_relays`, `blossom_servers`, `upstream_mode`, `max_upload_mib`=100, `ok_wait_sec`=15). |
| Seal | `gnome/nostr-seal/` (bead `nostrc-da9c`, closed) | `.nsealed` v1, MIME `application/vnd.nostr.sealed`, magic `NSEALED\x01`; `nostr_seal_core` static lib exports `nseal_header_read_fd()` / `nseal_header_stanza_recipient()` (recipient pubkeys are in clear). README "Not yet": `--publish` (upload + kind 1063 p-tagging recipients). |
| Publish lib | `gnome/libnostr-publish/` (bead `nostrc-tmsc`, closed; Debian `libnostr-publish0`) | `NostrPublishTransport` (libsoup 3 WebSocket; **Unix-socket variant** for the session relay; in-process **fixture** transport for tests), `NostrPublisher` (fan-out, OK aggregation, quorum, deadline via `tick()`; answers NIP-42 on publisher-owned transports with its signer), `NostrPublishSigner` (D-Bus `SignEvent`, **synchronous**, 30 s), `nostr_publish_nip65_relays()`, `nostr_publish_policy_select_targets()` (never mixes the session relay with write relays — nostr-share does not use it and mixes). No REQ/EOSE *query* helper — nostr-share and nostr-dispatcher each carry their own (`ns_net_fetch_replaceable`, `nd_fetch_event_async`). |
| Blossom lib | `libhanami/` (Debian `libhanami0`) | libcurl client `hanami_blossom_{get,head,upload,delete,list,mirror,upload_batch}` (whole-buffer, sync); pure `hanami_bud02_{create_auth_event,create_auth_header,parse_auth_header,validate_auth_event}`; per-server capability cache (`server_tag_ok`: blossom.band rejects the `server` tag; batch default omits it; `raw_random_ok`: body-sniffing servers answer 415). Links libgit2/SQLite too (`hanami-odb-backend.h`). |
| Local cache app | `apps/blossom-cache/` | Standalone libsoup 3 Blossom proxy on `127.0.0.1:24242` (local-blossom-cache spec): BUD-01/02/10, range requests, LRU, `?xs=`/`?as=` hints, `?cursor=`/`?limit=` listing (its own extension — BUD-02 specifies `since`/`until`), **no auth validation**. Not socket-activated; not started by anything. |
| Dispatcher | `gnome/nostr-dispatcher/` (bead `nostrc-1v65`, closed) | The **only** desktop entry claiming `x-scheme-handler/nostr` and `web+nostr` (NIP-21 opaque URIs `nostr:nevent1…`). `nd_fetch_event_async`: session relay (1.5 s) → relay hints (4 s budget). |
| Session relay | `apps/relayd/src/session/relayd_session_main.c` | `$XDG_RUNTIME_DIR/nostr/relay.sock`, 0600, SO_PEERCRED. Open bugs: cache-less mode never sends EOSE for a plain REQ (`nostrc-prqu.14`); no upstream federation (`nostrc-7d96`) — an event only it holds never leaves the machine. |
| Signer | `apps/gnostr-signer/data/dbus/org.nostr.Signer.xml` | `GetPublicKey() → npub`, `SignEvent(json, current_user, app_id) → signed json`, `GetRelays() → relays.conf`, `ApprovalRequested` signal (approval UI is the signer's). D-Bus-activatable (`org.nostr.Signer.service`). |
| gvfs (verified in tree) | `daemon/gvfsbackend.h`, `daemon/mount.c`, `daemon/meson.build`, `NEWS` | Vfunc pairs `do_`/`try_` for mount, unmount, open_for_read/read/seek/close, create/replace/append_to/edit/write/truncate/close_write, query_info, query_fs_info, enumerate, set_display_name, delete, trash, make_directory, make_symlink, copy, move, **push/pull with `GFileProgressCallback`**, set_attribute, create_dir_monitor/create_file_monitor. Helpers `g_vfs_backend_set_{display_name,stable_name,icon_name,symbolic_icon_name,user_visible,default_location,mount_spec,block_requests}`, `g_vfs_backend_{handle,get}_readonly_lockdown`, `g_vfs_backend_add_auto_info`. `.mount` keys: `Type, Exec, DBusName, AutoMount, Scheme, SchemeAliases, DefaultPort, HostnameIsInetAddress, MountPerClient`; lookup dir overridable with **`GVFS_MOUNTABLE_DIR`** / **`GVFS_MOUNTABLE_EXTENSION`** (`mount.c:466–470`). Backends install to `gvfs_libexecdir`, `.mount` files to `gvfs_mounts_dir`, `libgvfsdaemon`/`libgvfscommon` to `gvfs_pkglibdir` (Debian package `gvfs-libs`); **no headers installed**. OneDrive backend landed 1.53.90 → shipped in **1.54 = GNOME 46**; 1.56 = GNOME 47; 1.58 = GNOME 49; `g_vfs_backend_set_autounmount` is 1.61.90+; Google backend deprecated 1.59.1. `gvfsd-smb` reads GSettings `org.gnome.system.smb` (precedent for D22). |
| GIO dispatch (verified in `gappinfo.c`) | `g_app_info_launch_default_for_uri()` | Resolves `g_app_info_get_default_for_uri_scheme()` **first**; only when no scheme handler exists does it build a `GFile` and consult gvfs. `g_file_move()` falls back to copy + delete when the backend's `move` returns `G_IO_ERROR_NOT_SUPPORTED` (unless `G_FILE_COPY_NO_FALLBACK_FOR_MOVE`). Both facts drive D4 and D5. |
| Packaging | `debian/control`, `packaging/rpm/nostr-login.spec`, `docs/designs/packaging-plan-debian-fedora.md` | Per-component binary packages (`nostr-share`, `nostr-seal`, `nostr-home-fuse` with `Depends: libhanami0 (= ${binary:Version}) … fuse3`); RPM `%package -n <name>` stanzas; dependency-purity gate (headless closure must not match `gtk|adwaita|gvfs|gnome-|…`). `debian/control:204` already `Suggests: gvfs-backends` for `nostr-smb-mount --mode gvfs`. |
| Acceptance | `docs/ACCEPTANCE_MATRIX.md:28` | Row "GNOME client — GNOME Files/GVfs — **Unavailable**". This design is the work that fills it. |

Three consequences shape everything below:

1. **The namespace is relay/Blossom content, not a home.** Neither FUSE tree's
   manifest, cache, key handoff or unit applies. The only reusable pieces are
   libraries: libnostr-publish, libnostr, nostr-share's pure tag/URL/strip helpers.
2. **gvfsd already owns the lifecycle problems the FUSE trees fought** — single
   instance per mount spec, session teardown, unmount while busy, status to the
   desktop, no `st_dev`/xdev trap, no `allow_other` question (§2).
3. **The daemon is asynchronous by construction.** `try_*` vfuncs run on the
   backend's main context; libnostr-publish transports and the publisher are
   main-context objects; the signer's sync `SignEvent` wrapper must never be called
   from a `try_*` handler, and CPU-bound work must leave the main context (§7.1).

---

## 2. D1 — Why GVfs, not a third FUSE tree

### 2.1 The three shapes

**(a) A third FUSE daemon** (`nostr-relay-fuse`) mounting `~/Nostr/<npub>/…`.
**(b) An in-process GIO VFS module** implementing `GFile` for the schemes inside every
application (a `GIOModule` in `${libdir}/gio/modules/`).
**(c) gvfsd backends** (`gvfsd-blossom`, `gvfsd-nostr`) spawned by the user's `gvfsd`
from `.mount` files.

| Property | (a) FUSE | (b) GIO module | (c) gvfsd backend |
| --- | --- | --- | --- |
| Appears in Files sidebar / "Other Locations" / Connect to Server | only as a local folder | no (no `GMount`) | **yes**, as a user-visible mount |
| Flatpak apps see it | only with `--filesystem=` on the mountpoint | **no** (module is not in the sandbox runtime) | apps with `--talk-name=org.gtk.vfs.*` + `--filesystem=xdg-run/gvfsd` (common in GNOME apps) browse it natively; every other app receives files through the document portal over gvfs-fuse |
| Needs `/dev/fuse`, `fusermount3`, a mountpoint in `$HOME` | yes — see PHO §5 (unit ordering, `fuse.lock`, teardown) and PHO-D11 (`st_dev` trap, GDM) | no | **no**; POSIX view is `gvfs-fuse`'s job |
| Per-process credentials / approval UX | own D-Bus plumbing | every app talks to the signer itself | one daemon per mount, `GMountOperation` for questions |
| Streaming, seeks, progress, cancellation | own thread pool (legacy `nostrfs` built a libgo actor + upload pool for this) | per-app | `GVfsJob*` + `GCancellable` + `push`/`pull` progress, given |
| Directory change notification | `fuse_invalidate_path` gymnastics (PHO §7.3, 512-path rule) | none | `GVfsMonitor` → `GFileMonitor` in every client |
| Kernel page cache / `mmap` | yes | n/a | via gvfs-fuse only |
| Lifetime | systemd unit, `Type=notify`, exit codes 3–6 | app lifetime | gvfsd session; auto-spawn on first use, gone at logout |
| Shipping precedent | two experimental-gated trees, neither default-on | none in GNOME | every network location in Files |

### 2.2 Recommendation: **(c)**

(a) re-imports every problem the two existing trees solved twice: `nostrfs.c`
needed an actor, an upload pool, a quota sweeper and a warn-throttle
(`nostrfs.c:127–186`); `nostr-home-fuse.c` needed a lock file, a notify socket,
inotify reload, a status writer and a 40-line header explaining exit codes and
test env overrides. Those are costs of *being a FUSE daemon*, not of the feature.
gvfsd absorbs all of them. A FUSE mount is also invisible to Files' sidebar
model and to Flatpak apps, which is the opposite of what the bead asks for.

(b) is the wrong process boundary: every application would open its own relay
sockets and Blossom connections, each would need the signer, and sandboxed apps
would silently lack the scheme. GIO's own `GDaemonVfs` exists precisely to avoid
this.

(c) is the idiomatic answer and the only one whose result is *the same thing* as
`smb://`, `sftp://`, `google-drive://` and `onedrive://` from the user's point of
view. Its one real cost — gvfs has no public backend ABI — is priced in D17 and is
the same cost every backend paid before it was upstreamed.

### 2.3 What the existing trees keep

Nothing in `gnome/nostr-homed/` changes. The porthome overlay's PHO-D11 already
states that gvfs mounts under `$XDG_RUNTIME_DIR/gvfs` "do not clash" with
`$HOME/Portable`; the two products coexist. The porthome blob cache is *not* shared
(different address space, different key material); the GVfs cache is a plaintext
CAS for public blobs (§5.4).

---

## 3. Namespaces

### 3.1 `blossom://` — D2, D3, D4, D23

**URI grammar and mount specs.** The `.mount` file registers `Scheme=blossom`; gvfs's
default URI mapper turns the URI into a mount spec:

| URI | Mount spec | Meaning |
| --- | --- | --- |
| `blossom://blossom.example.com/` | `type=blossom host=blossom.example.com` | one server, the **signer's current identity**; writable |
| `blossom://blossom.example.com:8443/` | `… port=8443` | non-default port (still HTTPS) |
| `blossom://npub1…@blossom.example.com/` | `… user=npub1…` | one server, an **explicit pubkey**; read-only unless it equals the signer's identity |
| `blossom:///` | `type=blossom` | **"My Blossom servers"** — a virtual root whose entries are shortcuts to the host form (D23) |

Empty-authority URIs are ordinary gvfs (`network:///`, `smb:///`, `recent:///` all
use the default mapper). `blossom:///` is what the Files bookmark points at (§8.1).

**D2 — listed pubkey vs. signer identity.** The pubkey whose blobs are listed comes
from the mount spec's `user=` when present (the `blossom://<npub>@<server>/` form,
foreign browsing), else from `GetPublicKey` at mount time (the `sftp://host/` vs
`sftp://user@host/` convention). A mount whose listed pubkey differs from the current
signer identity is read-only: `access::can-write=FALSE`, pushes and deletes fail
`G_IO_ERROR_PERMISSION_DENIED`. The comparison is redone on every write, because the
signer's identity can change underneath a mounted tree; switching the *writable*
identity means unmount + remount, since mounts are keyed by spec. The display name
shows the npub prefix ("Blossom — blossom.example.com (npub1abc…)").

**D3 — names are content addresses.** Every entry in a `blossom://` folder is
`<sha256>.<ext>`, `<ext>` derived from the BUD-02 descriptor `type` (the same
mime→extension table as `ns_blossom_blob_url()`), no extension when the type is
unknown. This is the server's own canonical BUD-01 URL basename, it is stable across
servers, and it makes the CAS semantics honest: **rename is meaningless**, and the
name of an uploaded file is decided by its bytes, not by the name it was dropped
with (Ops table). Friendly names live in `nostr://…/files/` (§3.2), where events
carry them. The host form is the canonical mount; there is exactly one mount per
(server, pubkey), so there are never two views of the same listing.

**D23 — `blossom:///` is a root of shortcuts, not a tree.** Its entries are
`G_FILE_TYPE_SHORTCUT` with `standard::target-uri=blossom://<host[:port]>/`, one per
server in the current identity's kind-10063 list (BUD-03, list order; identical
hosts collapsed), exactly the shape `network:///` uses for `smb://server/` entries.
Files follows `target-uri` on activation, so the user lands in the real per-server
mount; the virtual root never enumerates blobs, never accepts pushes
(`G_IO_ERROR_NOT_SUPPORTED` "choose a server"), and holds no cache of its own. This
removes the duplicate-mount problem a nested tree would have (two mount specs, two
sidebar entries, two monitors for one server) and, more importantly, the data-loss
hazard of a cross-mount move between two views of the same server (see the `move`
row in the Ops table). Without a signer the root lists the servers of the last
successful mount from the index, read-only.

**Listing.** `GET /list/<pubkey>` (BUD-02), paginated with `?since`/`?until` until a
page returns fewer than the page size (cap 10 000 blobs per server; beyond that the
listing is truncated and the folder gets `nostr::truncated=true`). Some servers
require auth for `/list`: on `401`, retry once with a kind-24242 `t=list` event.
Descriptors without a 64-hex `sha256` are dropped. The listing is cached in the
index (§5.4) with a 60 s TTL; a directory monitor re-lists every 30 s while it
exists, passing `?since=<newest uploaded seen>` so a refresh is one small page, and
emits `CREATED`/`DELETED` diffs (deletions are detected on the TTL re-list).

**Attributes** (`query_info`, `enumerate`): `standard::{name,display-name,type=REGULAR,
size,content-type,icon,symbolic-icon}`, `time::modified` = `uploaded`,
`access::can-read=TRUE`, `access::can-write` = identity match (there is no in-place
write, but Files uses it to enable paste), `access::can-delete` = identity match,
`access::can-rename=FALSE`, `access::can-trash=FALSE`, `id::file=<sha256>`,
`etag::value=<sha256>`, plus the `nostr::` namespace: `nostr::sha256`, `nostr::url`,
`nostr::server`, `nostr::pubkey`, `nostr::uploaded`, `nostr::verified` (§5.3). The
folder: `standard::type=DIRECTORY`, `nostr::server`, `nostr::truncated`.
`query_fs_info`: `filesystem::type=blossom`, `filesystem::remote=TRUE`,
`filesystem::readonly` = !identity match, `filesystem::use-preview=IF_ALWAYS` (§6.3),
`filesystem::size`/`free` unset.

**Ops table (D4).**

| GIO operation | Backend vfunc | Behaviour |
| --- | --- | --- |
| list / stat | `try_enumerate`, `try_query_info` | from the index; network only on TTL expiry |
| read / seek | `try_open_for_read`, `try_read`, `try_seek_on_read`, `try_query_info_on_read`, `try_close_read` | read-through cache (§5.4); seeks map to HTTP `Range` (§5.5) |
| download to local | `try_pull` | cache fill → local copy; `GFileProgressCallback` from bytes received |
| copy *into* the mount | `try_push` | **upload**: sha256 the local file (in a `GTask` thread, §7.1), BUD-06 `HEAD /upload` precheck if the server supports it, `PUT /upload` with `Authorization: Nostr <b64 kind-24242>` (`t=upload`, `x=<sha>`, `expiration=now+300`, no `server` tag by default — blossom.band rejects it, per libhanami's capability notes), `Content-Type` = `g_content_type_guess()` of the local file, progress from libsoup's `wrote-body-data`. **The destination name is ignored**: the new entry is `<sha>.<ext>`, a `CREATED` monitor event for that name follows immediately, and the requested destination path does not exist afterwards — callers that need the result (nostr-nautilus, nostr-share per W9) must take it from the monitor event or by `nostr::sha256`, never by re-querying the name they asked for. Descriptor `sha256` ≠ ours ⇒ `G_IO_ERROR_FAILED` "server stored different bytes". A `415` from a body-sniffing server (blossom.band, blossom.primal.net — the `nostrc-bpum` findings) is reported verbatim with the hint to give the file a proper extension; per-server Content-Type negotiation and libhanami's PNG shim are deliberately out of scope. |
| create / save from an app | `try_create`, `try_replace`, `try_write`, `try_close_write` | **upload via spool**: bytes go to `$XDG_CACHE_HOME/gvfs-nostr/spool/<rand>` (size-capped as they arrive), `close_write` hashes and uploads exactly as `push`. `replace` on an existing `<sha>.<ext>` is also just an upload (the old blob is untouched — content addresses cannot be overwritten). |
| delete | `try_delete` | `DELETE /<sha256>` with `t=delete`, `x=<sha>`; `DELETED` monitor event. |
| trash | `try_trash` | `G_IO_ERROR_NOT_SUPPORTED` → Files offers "Delete Permanently" (precedent: sftp) |
| **move / rename** | `try_move`, `try_set_display_name` | **`G_IO_ERROR_PERMISSION_DENIED`** "blobs are content-addressed; renaming is a no-op" — *not* `NOT_SUPPORTED`. `g_file_move()` treats `NOT_SUPPORTED` from the backend as "fall back to copy + delete": on this mount that is a pull, a push that lands on the identical `<sha>.<ext>`, and then **a delete of that same blob** — a slow, silent data loss. GIO calls the backend's `move` only for same-mount pairs and performs the copy+delete fallback itself for cross-mount moves; because D23 guarantees one mount per server, a cross-mount move is always between two *different* servers, where re-upload + delete is exactly the right semantics. `set_display_name` has no fallback, so `NOT_SUPPORTED` would also be safe there, but the same error keeps the message consistent. |
| mkdir / symlink / append / truncate / set_attribute | `try_make_directory`, `try_make_symlink`, `try_append_to`, `try_truncate`, `try_set_attribute` | `G_IO_ERROR_NOT_SUPPORTED`, message "Blossom stores files by content hash" (the GIO analogue of `EROFS` for a namespace that *is* writable but not *that* way) |
| copy | `try_copy` | not implemented → GIO falls back to pull + push (for two Blossom servers that is the right thing; BUD-04 `PUT /mirror` is W-later) |
| `blossom:///` root | `try_enumerate` | shortcuts only (D23); every mutation ⇒ `G_IO_ERROR_NOT_SUPPORTED` |

**Size limits.** `max-upload-mib` (GSettings, default **100**, same as nostr-share)
enforced at spool time and before `push`; server `413` ⇒ `G_IO_ERROR_MESSAGE_TOO_LARGE`,
`507`/quota ⇒ `G_IO_ERROR_NO_SPACE`. Upload `Content-Length` is always sent (no
chunked encoding) so servers can reject early.

### 3.2 `nostr://<npub>/` — D5, D6, D7, D8, D9

**URI grammar.** Host = `npub1…` (bech32, NIP-19) or 64-hex pubkey. Anything else
(`user@host`, NIP-05 names, `nprofile1…` with relay hints) fails `try_mount` with
`G_IO_ERROR_INVALID_ARGUMENT` ("use nostr://<npub>/"); NIP-05 and nprofile are
W-later (§11). No port, no path outside the tree below.

**D5 — scheme routing, and why the scheme stays `nostr`.** NIP-21 URIs are *opaque*
(`nostr:nevent1…`, no authority) and belong to `nostr-dispatcher`, the only
`.desktop` that may claim `x-scheme-handler/nostr` (`nostrc-2thp`). Two facts decide
the routing:

- **Launching a URI never reaches gvfs.** `g_app_info_launch_default_for_uri()`
  resolves the scheme handler *first* and consults `GFile`/gvfs only when no handler
  exists. So every `nostr:` URI a browser, terminal (`gio open`) or app launches —
  opaque *or* authority-bearing — enters the dispatcher. The dispatcher's rule (W9)
  is: **a `nostr://` URI with an authority is a location**: it launches the default
  handler for `inode/directory` (Files) with that URI via `g_app_info_launch_uris()`,
  and **must not** call `g_app_info_launch_default_for_uri()` or spawn `gio open`
  on it — both re-enter the scheme handler, i.e. the dispatcher itself. Opaque
  `nostr:nevent1…` / `nostr:npub1…` keep today's kind-based routing, unchanged.
- **Browsing a URI never reaches the dispatcher.** Files' location bar, "Connect to
  Server", bookmarks and `g_file_new_for_uri()` construct a `GFile` directly; with a
  `Scheme=nostr` `.mount` installed, GIO maps `nostr://npub1…/files/` to the spec
  `type=nostr host=npub1…` and spawns `gvfsd-nostr`. An opaque `nostr:nevent1…`
  typed into Files maps to a host-less spec; `try_mount` fails it immediately with
  `G_IO_ERROR_INVALID_ARGUMENT` ("nostr: links open with the Nostr dispatcher") — a
  defensive fast-fail, since it errors in Files today too.

The alternative, a private scheme (`npub://`), avoids sharing the scheme but breaks
the one URI a human would guess and still needs the dispatcher rule for links.
Decision: **`nostr://`**, with the rule above; Q1 asks the maintainer to confirm.

**D6 — root listing, read-only always.**

| Entry | Source | Content | Attributes |
| --- | --- | --- | --- |
| `profile.json` | kind 0 (newest, verified) | the event's `content` parsed and pretty-printed if it is a JSON object, else the raw string | `content-type=application/json`, `time::modified=created_at`, `nostr::event-id`, `nostr::kind=0` |
| `relays.json` | kind 10002 | `{"relays":[{"url":"wss://…","read":true,"write":true},…]}` from `nostr_publish_nip65_relays()` (read + write sets merged, order preserved) | as above, `nostr::kind=10002` |
| `files/` | D7 | directory | `nostr::relays` (JSON array actually queried), `nostr::relays-source`, `nostr::index-age` (seconds) |

Neither JSON file is writable, even for the current identity (`G_IO_ERROR_READ_ONLY`);
editing a profile from Files is not a goal (Q10). Missing kind 0 / 10002 ⇒ the file
is absent, not empty.

**D7 — what is fetched, and what becomes a `files/` entry.** One query helper call
per mount (D11) with the filter set
`{"authors":[hex],"kinds":[0,10002,10063,1063,1,5]}` — kind 0 for `profile.json`,
10002 for `relays.json` and outbox discovery (D10), 10063 for the Blossom fallback
of §5.2 and for self-uploads (D9), 1063/1/5 for the listing. Kinds 0/10002/10063 are
latest-per-kind replaceables; 1063 and 1 are paginated by `until`:

| Source | Filter | Fields |
| --- | --- | --- |
| kind **1063** (NIP-94) | `{"kinds":[1063],"authors":[hex]}`, paginated down to `max-file-events` (default 5 000) | `url`, `m`, `x`, `ox`, `size`, `dim`, `alt`, `summary`, `blurhash`, `thumb`, `fallback` tags; `content` = description |
| kind **1** with `imeta` (NIP-92) | `{"kinds":[1],"authors":[hex],"limit":N}` — `imeta` is not an indexed tag, so the newest `imeta-scan-limit` notes (default **1 000**) are scanned; refreshes use `since` = newest seen | each `imeta` tag: `url`, `m`, `x`, `size`, `dim`, `alt`, `blurhash`, `fallback` |
| kind **5** | `{"kinds":[5],"authors":[hex]}` | hides referenced 1063 / kind-1 events (relays SHOULD already drop them per NIP-09; the session relay and our index may not) |

An entry **requires** a 64-hex `x` (sha256) and an `https://` `url`. This is a
deliberate v1 restriction, not spec conformance: NIP-94 requires only `url`, so
spec-valid events without `x` are **hidden**, because without a hash none of the
integrity story of §5–§6 can apply (Q11 asks whether to list them as
`nostr::verified=false` entries later). `m` missing ⇒ sniffed on first read (§5.6).
`size` missing ⇒ `standard::size` unset until the first HEAD.

**D8 — dedup and naming.** The key of an entry is its sha256. The same sha in several
events is one file; the newest event supplies the metadata and `nostr::event-id`;
`nostr::event-ids` carries all of them and `nostr::sources` says which kinds
(`1063`, `1`, or both — the delete rule in D9 depends on it). Display name, first
rule that applies:

1. the event's `["name", <text>]` tag if present — NIP-94 defines no filename tag
   (checked against `docs/nips/94.md`); this is our documented extension, emitted by
   our own publish path (D9) and proposed for `nostr-share` (W9). Q4;
2. the last path segment of `url` when it is *not* just the hash (e.g. NIP-96 hosts
   keep names);
3. `<sha256[0:12]>.<ext>` with `<ext>` from `m`.

Then sanitise: NFC-normalise; strip `/`, `\0`, control characters and Unicode
bidi/format controls; refuse `.`/`..`; a leading `.` is replaced by `_`; truncate to
200 bytes before the extension. Collisions (same sanitised name, different sha) get
`~<sha256[0:6]>` before the extension, deterministically (sorted by `created_at`
desc, then id). Names are recomputed on every refresh, so they are stable as long as
the event set is; a rename on the relay side is a `DELETED`+`CREATED` pair on the
monitor.

**Attributes** as in §3.1 plus `nostr::event-id`, `nostr::event-ids`, `nostr::sources`,
`nostr::kind`, `nostr::pubkey`, `nostr::alt`, `nostr::dim`, `nostr::blurhash`,
`nostr::thumb`, `nostr::fallback` (JSON array). `access::can-write`, `can-delete` and
`filesystem::readonly` depend on **self**: the mount is *self* iff the host pubkey
equals the signer's `GetPublicKey` at mount time (re-checked on every write, as in
D2).

**D9 — writes for self.** Only `nostr://<self>/files/`:

| Operation | Behaviour |
| --- | --- |
| `push` / `create`+`write`+`close_write` | 1. sha256 (local file or spool; `GTask` thread). 2. Refuse media with embedded metadata (D19). 3. Upload to the current identity's kind-10063 servers (fetched with D7, refreshed with the index), first server that accepts wins, `https://` only; no 10063 ⇒ GSettings `blossom-servers`; none ⇒ `G_IO_ERROR_NOT_SUPPORTED` "no Blossom servers: set them in Nostr Settings" (never a hard-coded server). 4. Build kind **1063**: `url`, `m`, `x`, `ox`(=`x`: we upload exactly the bytes we hashed and Blossom does not transform them), `size`, `dim` (from `ns_image_dimensions` logic), `name` (sanitised original filename), `alt` (= name); for `.nsealed` inputs add one `p` tag per recipient from `nseal_header_read_fd()` (only when built with `ENABLE_NOSTR_SEAL`); content `""`. 5. Sign via the async signer wrapper (§7.1). 6. Publish with `nostr_publisher_publish_signed()`, targets = own NIP-65 write relays plus a session-relay copy, mirroring `ns_resolve_targets()` (not `nostr_publish_policy_select_targets()`, which never mixes the two), `NostrPublishPolicy.ok_wait_sec = 15` (the publisher default is 120), quorum 1, and nostr-share's success rule enforced through the relay callback: success = ≥ 1 **direct** write relay ACK unless `upstream-mode` is `session-relay-only`, which logs a warning and succeeds on the session ACK (nothing leaves the machine until `nostrc-7d96`). 7. Drop the entry into the index and emit `CREATED` without waiting for a relay round-trip. If 6 fails after 3 succeeded the job fails and the blob stays on the server; a retry re-uses it (`HEAD /<sha>` before `PUT`). |
| `delete` | Allowed only when `nostr::sources` is exactly `1063`: publish kind **5** (`e` = every 1063 id of the entry, `k`=`1063`; kind-5 deletion is advisory per NIP-09 — relays SHOULD honour it — and affects only same-pubkey events, satisfied by construction), drop the entry from the index and emit `DELETED` on the publish verdict, then `DELETE /<sha>` on each server that hosts it **only if** no other known event of this identity references the sha (Q8) — the kind 5 never removes blob bytes by itself. **Mixed-source guard:** if the sha is also referenced by one of the user's own kind-1 notes, delete is refused with `G_IO_ERROR_PERMISSION_DENIED` "also attached to note `<id>`; delete the note in a Nostr client first" — otherwise the entry would silently resurrect from the `imeta` source on the next refresh. `imeta`-only entries ⇒ the same error. |
| move / rename | `G_IO_ERROR_PERMISSION_DENIED` (same reason as §3.1: `NOT_SUPPORTED` would make GIO re-upload the same bytes under a new 1063 and then kind-5 the entry — including the new event). Rename-as-republish is W-later. |
| mkdir / symlink / trash / set_attribute | `G_IO_ERROR_NOT_SUPPORTED` |
| any write on a foreign mount | `G_IO_ERROR_READ_ONLY` |

`g_vfs_backend_handle_readonly_lockdown()` is consulted on every write path in both
backends, as upstream backends do.

---

## 4. Relay access — D10, D11

**D10 — relay resolution order.** For a mount of pubkey *P*:

1. the **session relay** at `$XDG_RUNTIME_DIR/nostr/relay.sock` (libnostr-publish
   Unix-socket transport), deadline **1.5 s** (the dispatcher uses the same budget;
   see the `nostrc-prqu.14` gate under D11);
2. *P*'s **NIP-65 write relays** (outbox model: to read someone's events you go to
   their *write* relays — the bead text says "read relays", which is the inbox set
   and is the wrong side for this; corrected here). *P*'s kind 10002 is looked up
   on (1), then on the mounting user's own relays: signer `GetRelays()`
   (`relays.conf`), else the user's own kind-10002 read+write set from (1), else
   GSettings `discovery-relays` (default **empty** — no hard-coded public relays,
   per the repo's slop rules). Up to **4** relays are connected concurrently
   (list order); `ws://` is accepted for loopback only.
3. no 10002 found ⇒ query the user's own relays with `nostr::relays-source=fallback`
   on `files/`; nothing at all ⇒ `try_mount` fails `G_IO_ERROR_HOST_NOT_FOUND`
   "no relays known for npub1…".

**D11 — protocol shape.** Queries are real `REQ` subscriptions, `EOSE`-terminated,
never polled: enumerate opens the D7 filters on every connected relay, returns
when every relay sent `EOSE` or its deadline (`relay-timeout-ms`, default 8 000)
passed, then **keeps the subscription open** while a directory monitor exists so
new events arrive as `CREATED` monitor events; subscriptions close on unmount or
when the last monitor is released (plus a 60 s grace). `CLOSED` (with its
machine-readable prefix) or a connection failure marks the relay unreachable for
that query; `NOTICE` is informational and only logged. The REQ/EOSE helper is added
to **libnostr-publish** (W2a) because it is otherwise the third copy after
`ns_net_fetch_replaceable` and `nd_fetch`; migrating those two onto it is a separate
bead, not part of this design.

**Hard dependency — `nostrc-prqu.14`.** The session relay in cache-less mode never
sends `EOSE` for a plain `REQ`. Against it, every query runs to its full deadline
and a kept-open subscription can never tell "stored events delivered" from "still
streaming". The fix **must land before W2b**. Interim fallback, for the session
socket **only** (never for remote relays): after ≥ 1 `EVENT`, 500 ms of idle is
treated as end-of-stored-events; the fallback is deleted with the fix.

**NIP-42.** Publisher-owned publish transports answer `AUTH` for free
(`NostrPublisher` signs with its signer). The v1 *query* helper declines challenges:
a relay that requires `AUTH` for `REQ` is unreachable for reads (§7.1 explains why —
the transport's auth callback is synchronous).

`NostrPublisher` needs `nostr_publisher_tick()`; the backend arms a 5 s `GSource`
while requests are in flight.

---

## 5. Blob access, verification, cache, offline

### 5.1 HTTP stack — D12

**libsoup 3** directly, async, one `SoupSession` per backend process (user agent
`gvfs-nostr/<version>`, default `GTlsDatabase`, no `accept-invalid-cert` path,
`SOUP_SESSION` timeout 60 s idle). libhanami is not linked (§0); the kind-24242
event builder is ~40 lines of json-glib (`gn-bud02.c` in the shared core, §9),
unit-tested against `hanami_bud02_validate_auth_event()` in a test that *does* link
libhanami, and against the fixture server (§12). BUD-01/02/06 endpoints used: `GET
/<sha>[.ext]` (+`Range`), `HEAD /<sha>`, `GET /list/<pubkey>`, `PUT /upload`, `DELETE
/<sha>`, `HEAD /upload` (BUD-06, optional).

### 5.2 Which URL is fetched — D13

- `blossom://`: always `<server>/<sha>[.ext]` on the mounted server.
- `nostr://…/files/`: the event's `url`. If it is **canonical** (`https://<host>/<sha>[.<ext>]`,
  basename hash equal to `x`) **and a size is declared** (event `size` tag or the
  response's `Content-Length`; if both exist and disagree, the URL is treated as
  non-canonical), it is streamed to the reader while the download runs (§5.3). If it
  is **not** canonical (NIP-96 hosts, arbitrary CDNs) or has no declared size, the
  blob is **downloaded completely and verified before the first byte is served** —
  the server has not committed to the hash, so the stream cannot be trusted early.
  `fallback` URLs are tried in order after `url` fails (network error, 4xx, 5xx,
  hash mismatch); then the entry's other events' URLs; then each of the author's
  kind-10063 servers (from D7) by `<server>/<sha>` (BUD-01: the hash is the address).
- `http://` is refused unless the host is loopback **and** `allow-insecure-loopback`
  is set (fixtures, `blossom-cache`).

### 5.3 Streaming and verification — D14

Reads are served from the cache file *while it fills*: the download writes
`blobs/tmp/<sha>.<rand>`, hashing incrementally; `try_read` for an offset beyond the
filled length waits (job stays pending, cancellable) until the bytes arrive or the
download fails. On EOF the hash is compared: **match** ⇒ `rename()` into
`blobs/<aa>/<sha>` (0600), `nostr::verified=true`; **mismatch** ⇒ the temp file is
unlinked, every open handle on it fails its next `read` with `G_IO_ERROR_FAILED`
"content hash mismatch (server returned different bytes)", the (url, sha) pair is
negatively cached for 10 min and the next `fallback` is tried on the next open.
Bytes are never served beyond the declared `size`: a server that keeps sending is
cut off at `size` and the blob is rejected. Until the rename the file carries
`nostr::verified=false`. The residual window — a canonical-URL server lying until
EOF — is bounded, detected, and confined to the case where the server itself
committed to the hash in the URL; it is the same posture as `gvfsd-http` and every
Blossom client, and it is why non-canonical or size-less URLs are fully verified
first (D13).

### 5.4 Cache — D15

`$XDG_CACHE_HOME/gvfs-nostr/` (0700), shared by both backends (same sha keyspace,
several processes):

| Path | Content | Policy |
| --- | --- | --- |
| `blobs/<aa>/<sha256>` | verified blobs, 0600 | **LRU by mtime**: every open `utimensat()`s the file. Explicit touch is immune to `noatime`/`relatime`, needs no shared index between the daemon processes, and is the trick porthome's `nh_syncd_cache` already uses; the cost is that mtime stops meaning "download time" (that lives in the index). Eviction runs on insert when the total exceeds `cache-max-mib` (default **2048**), down to 90 %, oldest first, skipping blobs with open handles in *this* process and any blob younger than 60 s. Blobs larger than `cache-max-blob-mib` (default **256**) are not admitted: they are streamed (§5.5). `flock(blobs/.lock)` around insert/evict; reads lock-free. |
| `blobs/tmp/` | in-flight downloads | unlinked on completion/failure; entries older than the daemon's own start time are swept at startup (crash leftovers) |
| `index/blossom/<host>[_<port>]/<pubkey>.json`, `index/nostr/<pubkey>.json` | last listing + attributes + relay set + `fetched_at` | TTL 60 s for enumerate; kept indefinitely for offline (§5.7); rewritten atomically; garbage-collected against `blobs/` on the first eviction pass |
| `spool/` | uploads in progress from `create`/`write` | unlinked on `close_write`; stale entries swept at startup like `blobs/tmp/` |

Nothing in the cache is secret (it is public content), but it *is* private to the
user (0700/0600) because it reveals what they looked at. Purge is `rm -rf`; a
"Clear cache" button belongs to Nostr Settings (W9).

### 5.5 Seeks and large blobs

A cached blob is filled sequentially from 0. `seek` before the filled offset is
served from the file; beyond it, the read waits (bounded by throughput, cancellable).
Un-admitted large blobs (> `cache-max-blob-mib`) and every `seek` on them issue an
independent `Range: bytes=<off>-` request, exactly as `gvfsd-http` does, verified
only per-response (no whole-blob hash is possible without a full read; the file
stays `nostr::verified=false`). Video preview in Files therefore works for a 2 GiB
file without caching it.

### 5.6 Content types

`standard::content-type` = the BUD-02 descriptor `type` / event `m` / HEAD
`Content-Type`, unless missing or `application/octet-stream`, in which case the
first 4 KiB of the blob are sniffed with `g_content_type_guess(name, data)` on
first read and stored in the index. Uploads send the local file's guessed type.
`.nsealed` resolves to `application/vnd.nostr.sealed` through the shared-mime XML
that the `nostr-seal` package installs (`nostr-seal.xml`); without it, it is
`application/octet-stream` and still round-trips as a blob.

### 5.7 Offline — D16

`GNetworkMonitor` gates every network attempt. When offline (or when every relay /
server fails):

| Operation | Behaviour |
| --- | --- |
| mount | succeeds **if** an index exists for the spec (`nostr::index-age` set on the root, display name suffixed " (offline)"); otherwise `G_IO_ERROR_NOT_CONNECTED` "offline and never mounted before" |
| enumerate / query_info | from the index |
| read of a cached blob | works |
| read of an uncached blob | `G_IO_ERROR_NOT_CONNECTED` within 1 s — never a hang (PHO-D9's argument applies verbatim: a blocking miss stalls the thumbnailer, then Files, then unmount) |
| any write | `G_IO_ERROR_NOT_CONNECTED` |

Coming back online does nothing by itself; the next enumerate after TTL refreshes.

---

## 6. Security — D18, D19

### 6.1 Threat model

| Threat | Control |
| --- | --- |
| Hostile relay forges or substitutes events | every event: id recomputed, Schnorr signature verified (libnostr, as `ns_net_fetch_replaceable` does), `kind` and `pubkey` must match the filter; replaceables: newest `created_at`, ties by lexically lowest id (NIP-01) |
| Relay floods (huge events, unbounded lists) | per-event cap 64 KiB; per-mount caps `max-file-events` 5 000 / `imeta-scan-limit` 1 000 / 10 000 blobs per server; oversize ⇒ dropped and counted in `nostr::truncated` |
| Server returns wrong bytes | sha256 before cache admission (§5.3); non-canonical or size-less URLs verified before the first byte (D13); `size` ceiling |
| **Bytes served before the sha is verified** | bounded to canonical URLs with a declared size (D13); `nostr::verified=false` until the rename; mismatch unlinks the file and fails every open handle (D14); range-served large blobs stay unverified and say so |
| Server/event lies about content type | the type is advisory for icons and app choice only; sniffing for octet-stream; nothing is ever executed; `.desktop`-like files are not special (GIO marks gvfs files non-trusted) |
| **Path traversal** via event/descriptor data | names sanitised (D8); cache paths are derived from the validated `[0-9a-f]{64}` sha only, never from names or URLs; cache dir created 0700, files opened `O_NOFOLLOW|O_CREAT|O_EXCL` |
| TLS | GLib default validation; HTTPS only; loopback `http://` behind `allow-insecure-loopback` |
| Key material | **none in gvfsd.** Only `GetPublicKey`/`SignEvent` over D-Bus; no `mlock`, nothing to scrub. Kind-24242 events carry `expiration=now+300` and one `x` tag (no batch tokens) |
| Signer ACL semantics | app_id `org.nostr.Files` (D18). A remembered "allow" in the signer means *any* application writing to a mounted tree gets uploads/1063s signed without a prompt — that is the nature of a filesystem, and it is documented in the signer prompt text ("Files (GVfs) wants to upload photo.jpg (2.1 MiB) to blossom.example.com") |
| Privacy | fetching reveals the user's IP to Blossom servers and relays, as any client does; `?as=` author hints are never sent; the index reveals browsing history ⇒ 0700 cache; `gio mount -u` does not purge |
| Multi-user | gvfs mounts are per-user by construction (`$XDG_RUNTIME_DIR/gvfs` is 0700); there is no `allow_other` question |
| Lockdown | `org.gnome.desktop.lockdown` read-only lockdown honoured via `g_vfs_backend_handle_readonly_lockdown()` |

### 6.2 D18 — authentication and `GMountOperation`

There are **no passwords** anywhere in these backends, so `ask_password` is never
called. The only interactive authentication is the **signer's own approval UI**
(`ApprovalRequested` → gnostr-signer dialog), which fires on `SignEvent`. The mount
operation is used for exactly one question, via `g_mount_source_ask_question()`:
mounting `nostr://<npub>/` where `<npub>` is a locally known identity **but the
signer is unreachable** — "Your Nostr signer is not available. Mount read-only?"
[Mount read-only] [Cancel]. Read-only mounts never touch the signer.

Unmount cannot prompt from the backend (`GMountSource` questions are a mount-time
mechanism): `try_unmount` returns `G_IO_ERROR_BUSY` while uploads are in flight
(Files shows its generic busy dialog) and `G_MOUNT_UNMOUNT_FORCE` aborts them —
spool files are unlinked; a partially received `PUT` is the server's to discard
(BUD-02 uploads are atomic on the server).

`blossom://` without a reachable signer and without `user=` cannot list (no pubkey)
and fails `G_IO_ERROR_PERMISSION_DENIED` "no Nostr signer available"; with an
explicit `user=` it mounts read-only (D2).

### 6.3 Bandwidth policy: thumbnails and indexers

`filesystem::use-preview = G_FILESYSTEM_PREVIEW_TYPE_IF_ALWAYS`: Files thumbnails
these mounts only when the user set "Show thumbnails: Always" — the default
"Local files only" never pulls a whole folder of images over the network.
Tracker/localsearch does not index `$XDG_RUNTIME_DIR/gvfs` by default; nothing to
do. This answers PHO §15 item 4 for this design.

### 6.4 D19 — media metadata on upload

nostr-share refuses to upload photos whose EXIF/XMP/IPTC it cannot strip, and
strips them by default. A filesystem cannot strip: the sha of what the user copied
must be the sha of what is stored, or `g_file_copy` semantics break. Decision:
**both backends refuse `image/*` uploads whose container carries metadata segments**
(detector = "`ns_strip_metadata()` would change the bytes", factored from
`gnome/nostr-share/src/ns-strip.c` into the shared core, §9), and refuse
HEIC/AVIF/TIFF/`video/*`/`audio/*` outright (uninspectable), with
`G_IO_ERROR_PERMISSION_DENIED` "photo contains location/camera metadata — use *Share
to Nostr* to publish a cleaned copy, or enable *Upload media with metadata* in Nostr
Settings". GSettings `allow-media-metadata` (default **false**) lifts it.
`application/vnd.nostr.sealed` and everything non-media pass. Q3.

---

## 7. Process shape and mount lifecycle — D20

### 7.1 Async everywhere

Every vfunc — including `push`/`pull` — is implemented as `try_*` (returns `TRUE`,
completes the `GVfsJob` later on the daemon's main context), like `gvfsd-http` and
`gvfsd-dav`; no `do_*` thread pool. Consequences:

- **everything long-lived lives on the main context**: the `SoupSession`, the
  libnostr-publish transports and `NostrPublisher` (documented as single-context
  objects), the async signer `GDBusProxy`, the 5 s publisher tick and the 30 s
  monitor re-list `GSource`s. No marshalling is needed because no vfunc runs
  elsewhere;
- **CPU-bound work leaves the main context**: sha256 of a local file for `push`,
  hashing the spool on `close_write`, the D19 metadata detector and content sniffing
  run in `g_task_run_in_thread()`; results return through the `GTask` callback on the
  main context. Hashing a 100 MiB file on the main loop would stall every other job
  of the mount;
- the signer is called through a thin **async** wrapper
  (`gn_signer_sign_event_async`, `gn_signer_get_public_key_async` over
  `g_dbus_proxy_call()`), *not* `nostr_publish_signer_sign_event_json()`, which
  blocks for up to 30 s of approval UI; the signed JSON then goes to
  `nostr_publisher_publish_signed()`. Cancelling a job while the signer prompt is
  open cancels only the client-side wait — the signer may still complete the
  signature; the orphaned signed event is simply never published, and because the
  blob was already uploaded the next push re-`HEAD`s before re-`PUT`ting (§3.1),
  so nothing is stranded;
- NIP-42 `AUTH` on *query* transports is not answered in v1:
  `NostrPublishTransportAuthCallback` is synchronous and would need the sync signer
  on the main loop. Filed as a libnostr-publish follow-up (async auth callback) in
  W2a's notes. Publish transports get it from `NostrPublisher` (§4);
- `GCancellable` from each job aborts HTTP messages, `GTask`s and waiting reads.

### 7.2 `.mount` files

```ini
# ${datadir}/gvfs/mounts/blossom.mount
[Mount]
Type=blossom
Exec=${libexecdir}/gvfsd-blossom
AutoMount=false
Scheme=blossom
MountPerClient=false
```

```ini
# ${datadir}/gvfs/mounts/nostr.mount
[Mount]
Type=nostr
Exec=${libexecdir}/gvfsd-nostr
AutoMount=false
Scheme=nostr
MountPerClient=false
```

Deliberately absent keys: **`DBusName`** — gvfsd then spawns `Exec` once per mount
spec through the spawner protocol implemented by gvfs's own `daemon-main.c` (which we
reuse, §10.1); `DBusName` is for backends that serve from a pre-owned well-known
name, which is not this shape. **`DefaultPort`** and **`HostnameIsInetAddress`** —
neither scheme has a numeric default port to strip or IP-literal hosts.
`MountPerClient=false` gives one daemon per mount spec serving every client, which
the shared cache and single `SoupSession` (D12) assume. Tests point
`GVFS_MOUNTABLE_DIR` at the build tree (§12).

### 7.3 `try_mount`

1. Parse the mount spec; validate `host` (hostname/npub/hex), `port`, `user`.
   Host-less `nostr` spec ⇒ `G_IO_ERROR_INVALID_ARGUMENT` (D5).
2. `g_vfs_backend_set_block_requests(TRUE)`.
3. Identity: `GetPublicKey` (async, 10 s) — `blossom://` unless `user=`; `nostr://`
   to decide *self* (failure ⇒ read-only, with the D18 question when host is a
   locally known identity).
4. First index: `blossom://` — `GET /list` (or the 10063 list for `blossom:///`);
   `nostr://` — D10 resolution + the D7 query; offline ⇒ index file (D16).
5. `g_vfs_backend_set_display_name("Blossom — host (npub1abc…)" | "My Blossom servers"
   | "Nostr — <display name from kind 0 or npub1abc…>")`,
   `set_stable_name("blossom:host=…[,user=…]" | "blossom:" | "nostr:host=<npub>")`
   (this becomes the gvfs-fuse directory name), `set_icon_name("folder-remote")`,
   `set_symbolic_icon_name("folder-remote-symbolic")`, `set_user_visible(TRUE)`,
   `set_default_location("/")` (`/files/` for `nostr://`), `set_mount_spec` unchanged.
6. `g_vfs_job_succeeded`, unblock requests.

Mount errors: `HOST_NOT_FOUND` (no relays / DNS), `NOT_CONNECTED` (offline, no
index), `PERMISSION_DENIED` (no signer for `blossom://` without `user=`),
`INVALID_ARGUMENT`, `FAILED` with the server's `X-Reason` for HTTP 5xx.

### 7.4 Running and unmount

State per process: the index, open handles, active monitors, live REQs, in-flight
uploads, a 5 s publisher tick while publishing, a 30 s monitor re-list timer while
monitors exist. `try_unmount`: `G_IO_ERROR_BUSY` when uploads are in flight unless
forced (§6.2); close REQs; flush the index; `g_vfs_job_succeeded`. The process exits
when gvfsd drops it; at session end gvfsd takes every backend with it. The cache is
persistent and needs no teardown. `g_vfs_backend_set_autounmount` (1.61.90+) is not
used; the build targets 1.54+ (D17).

### 7.5 gvfs-fuse and Flatpak

`gvfs-fuse` exposes every user-visible mount at
`$XDG_RUNTIME_DIR/gvfs/blossom:host=blossom.example.com/…` and
`…/gvfs/nostr:host=npub1…/files/…`, so mpv, LibreOffice and terminals read and
write these paths with plain POSIX I/O — the "POSIX view for free" of the bead.
Backends run in the **host** `gvfsd`; the signer is on the host session bus.

The honest Flatpak story for v1: a sandboxed app browses `blossom://` natively only
if its manifest grants `--talk-name=org.gtk.vfs.*` and `--filesystem=xdg-run/gvfsd`
(many GNOME apps do; it is not a default permission). Every other sandboxed app
receives files from these mounts the way it receives any file — through the
document portal, when Files, the file chooser or `xdg-open` hands it a gvfs-fuse
path. No portal work is in v1; a portal-mediated share path is follow-up work.
The backends never read caller paths, so they need no sandbox awareness.

---

## 8. Desktop integration

### 8.1 Files

- **Sidebar / Other Locations**: user-visible mounts appear once mounted; "Connect
  to Server" accepts `blossom://…` and `nostr://…` (GIO validates the scheme against
  the installed `.mount` files).
- **Bookmarks (D21)**: the discoverable entry points are two GTK bookmarks,
  `blossom:/// My Blossom servers` and `nostr://<npub>/files/ My Nostr files`,
  written to `~/.config/gtk-3.0/bookmarks` and `~/.config/gtk-4.0/bookmarks` by
  the **Nostr Settings → Files** page (`nostrc-janr` already lists a Files page)
  behind a toggle, never by the package. A gvfs *volume monitor* (the mechanism that
  shows unmounted Google Drive accounts) is another D-Bus service implementing
  `org.gtk.Private.RemoteVolumeMonitor`; it is W-later, not v1.
- **Drag-and-drop / copy**: standard `g_file_copy` → `push`/`pull` with progress
  in Files' operations popover. Callers must not expect the dropped name to exist
  afterwards on `blossom://` (D4).
- **Open**: double-click goes through the gvfs-fuse path for non-GIO apps
  (`.nsealed` → `nostr-seal-gtk`, §9).

### 8.2 `nostr-nautilus` (`nostrc-xlf3`, in progress)

"Upload to Blossom…" becomes `g_file_copy(file, blossom://<server>/…)` with the
server chosen from the same 10063 list — no second upload code path; the extension
takes the resulting `<sha>.<ext>` from the `CREATED` monitor event. "Share to
Nostr…" on a file inside `blossom://`/`nostr://` passes the GVfs URI to
`nostr-share` unchanged (its README already accepts GVfs URIs).

### 8.3 Dispatcher and Settings

- `nostr-dispatcher` (W9): a `nostr://` URI *with authority* is a location — launch
  the `inode/directory` default handler (Files) with the URI via
  `g_app_info_launch_uris()`; never `g_app_info_launch_default_for_uri()` / `gio
  open`, which would re-enter the dispatcher (D5). Opaque `nostr:` links unchanged.
- `org.nostr.Settings` (W9): Files page owns `org.nostr.gvfs` keys (§10.3), the
  bookmark toggle, "Clear cache", and the media-metadata switch.

---

## 9. Relation to `nostr-share` and `nostr-seal`

**Division of labour.** `nostr-share` is the *publishing* front door: dialog,
kind choice, captions, mentions, metadata stripping, previews of the event JSON.
The backends are the *filesystem* view of what was published. They share the
protocol (same servers, same kind-24242 shape, same 1063 tags, same success rule)
and a small pure core (below), but not the upload/publish code paths; the only
mutations the backends perform are the two a filesystem can express (put bytes,
remove bytes).

| Flow | What happens |
| --- | --- |
| Share a photo with nostr-share | upload + kind 1 with `imeta` (or 1063). It appears in `blossom://<server>/` on the next enumerate (≤ 60 s, ≤ 30 s under a monitor) and in `nostr://<self>/files/` as an `imeta`-sourced (undeletable from Files) or 1063-sourced entry. |
| Drop `report.pdf` into `nostr://<self>/files/` | upload + kind 1063 with `name`, no dialog, no stripping (D19 refuses metadata-bearing photos). This *is* the "`--publish`" nostr-seal's README defers, and the "Upload to Blossom" of `nostrc-xlf3`, expressed as a copy. |
| Share a file that is already in `blossom://` | nostr-share reads the GIO attributes `nostr::sha256` / `nostr::url` (via `g_file_query_info("nostr::*")`, not `getxattr(2)`) and skips the re-upload (W9: ~30 lines in `ns-share.c`; BUD-04 `mirror` if the target server differs is W-later). |
| Seal then publish | `nostr-seal encrypt --to npub1… report.pdf` → drop `report.pdf.nsealed` into `nostr://<self>/files/` → 1063 with `m=application/vnd.nostr.sealed`, `p` tags for every recipient (read from the header, which lists them in clear by design). Recipients see it in `nostr://<sender>/files/`, double-click → `nostr-seal-gtk` via gvfs-fuse → decrypt through `NIP44DeriveConversationKey` on *their* signer. The backends never decrypt anything and never call that method. |
| Sealed blob on `blossom://` | just a blob; sha-named; content type from the shared-mime magic (`NSEALED\x01`) when `nostr-seal` is installed. |

**Shared pure core — where it lives.** A private static library `ns-core` at
`gnome/nostr-share/core/` (built whenever `ENABLE_NOSTR_SHARE` *or*
`ENABLE_GVFS_NOSTR` is on; no install, no SONAME): mime↔extension
(`ns_blossom_blob_url` core), 1063/`imeta` tag *parsing* (the inverse of
`ns_tags_add_file_metadata` / `ns_imeta_tag_new`), the metadata *detector* (§6.4,
from `ns-strip.c`), the D8 name sanitiser, and the kind-24242 builder. Linked by
`nostr-share`, `libgvfsnostr-common` and the test fixture. It is not
libnostr-publish material (publish policy is a different scope) and it is not
duplicated. Moving nostr-share's own code onto it is behaviour-preserving and lands
with W1/W4; Q12 lets the maintainer pick a different home.

---

## 10. Build, packaging, configuration — D17, D22

### 10.1 Tree

```
gnome/gvfs-nostr/
  CMakeLists.txt                 option ENABLE_GVFS_NOSTR (OFF); GVFS_SOURCE_DIR / GVFS_VERSION
  common/                        libgvfsnostr-common (STATIC, private); links ns-core (§9)
    gn-blobcache.[ch]            §5.4 CAS + LRU + flock + startup sweep
    gn-http.[ch]                 libsoup session, range fetch, streaming verify (§5.3/5.5)
    gn-signer.[ch]               async GetPublicKey / SignEvent (§7.1)
    gn-relay.[ch]                D10 resolution, 4-relay set, live REQs (thin over W2a)
    gn-events.[ch]               verify + parse kind 0/10002/10063/1063/imeta/5
    gn-index.[ch]                index files, TTL, offline
    gn-settings.[ch]             org.nostr.gvfs
  gvfsbackendblossom.[ch]        mirrors gvfs/daemon/gvfsbackend*.c naming for the upstream MR
  gvfsbackendnostr.[ch]
  daemon-main-glue.c             includes gvfs's daemon-main.c with DEFAULT_BACKEND_TYPE/BACKEND_TYPES
  data/blossom.mount.in  data/nostr.mount.in  data/org.nostr.gvfs.gschema.xml  data/nostr-gvfs.7.md
  tests/                         §12
gnome/nostr-share/core/          ns-core (STATIC, private): mime↔ext, tag parsing, metadata
                                 detector, name sanitiser, kind-24242 builder (§9)
```

### 10.2 D17 — building against gvfs's private ABI

gvfs installs the daemon library but no headers, and the library's ABI can change
between any two releases. Options:

- **(a) Out-of-tree, pinned source.** `GVFS_SOURCE_DIR` (packagers) or
  `FetchContent` of `https://gitlab.gnome.org/GNOME/gvfs.git` at tag
  `GVFS_VERSION` (developers) supplies `daemon/*.h`, `common/*.h` and
  `daemon/daemon-main.c`; a minimal generated `config.h` satisfies their includes.
  Link `-L${libdir}/gvfs -lgvfsdaemon -lgvfscommon -Wl,-rpath,${libdir}/gvfs`.
  **The package pins gvfs exactly** — there is no public ABI to pin a range
  against, and gvfs's own binary packages pin each other exactly, which is the only
  precedent: Debian `Depends: gvfs-libs (= ${gvfs:Version})` (the private libs
  live in `gvfs-libs`, not `gvfs`), `gvfs:Version` a substvar captured from the
  build-time `gvfs-libs`; RPM `Requires: gvfs%{?_isa} = %{gvfs_version}` with
  `%{gvfs_version}` evaluated at build. The package becomes uninstallable when the
  distro bumps gvfs until it is rebuilt: CI needs a rebuild-on-gvfs-bump trigger
  (the same job as the ABI smoke, §12 q). Rejected variants: shipping a gvfs fork
  (security-update burden), `dlopen` + symbol probing (fails late, in the user's
  session).
- **(b) Upstream first.** Correct end state, ≥ 1 release cycle before users have it.
- **(c) Distro patch to the `gvfs` source package.** Nobody maintains that.

Decision: **(a) now, (b) as W10**; sources are laid out and named so the MR is a
copy plus a `meson.build` snippet. The version floor is **1.54** (GNOME 46, the
OneDrive-era `GVfsBackend`): supported distros are Ubuntu 24.04 (1.54) and Debian 13
(1.57) onwards; Debian 12 (gvfs 1.50) is unsupported and the package README says
so. Linking the MIT daemons against gvfs's LGPL-2.1+ libraries is compatible;
`debian/copyright` notes it. Q2 asks the maintainer to accept the exact-version
pin; it is the honest cost of shipping before upstream.

### 10.3 Configuration — D22

GSettings schema `org.nostr.gvfs` (path `/org/nostr/gvfs/`), read by both backends
(in-tree precedent: `gvfsd-smb` reads `org.gnome.system.smb`):

| Key | Default | § |
| --- | --- | --- |
| `max-upload-mib` | 100 | 3.1 |
| `cache-max-mib` / `cache-max-blob-mib` | 2048 / 256 | 5.4 |
| `max-file-events` / `imeta-scan-limit` | 5000 / 1000 | 3.2 |
| `relay-timeout-ms` | 8000 | 4 |
| `discovery-relays` | `[]` | 4 |
| `blossom-servers` | `[]` (fallback when no kind 10063) | 3.2 D9 |
| `upstream-mode` | `session-relay-or-direct` | 3.2 D9 |
| `allow-insecure-loopback` | false | 5.2 |
| `allow-media-metadata` | false | 6.4 |

`nostr-share` keeps its own conf file today; Q6 asks whether `nostrc-janr` should
make `org.nostr.gvfs` (or an `org.nostr.*` umbrella) the single source for
`upstream-mode`/`blossom-servers`. No `.conf` file, no env overrides except the test
seams (§12).

### 10.4 Packages

| | Debian | RPM |
| --- | --- | --- |
| Name | `nostr-gvfs-backends` (Section: gnome) | `%package -n nostr-gvfs-backends` |
| Depends | `${shlibs:Depends}` (libsoup-3.0-0, libjson-glib, libglib), `libnostr1 (= ${binary:Version})`, `libnostr-publish0 (= ${binary:Version})`, **`gvfs-libs (= ${gvfs:Version})`**, `gvfs`, `dconf-gsettings-backend \| gsettings-backend` | `libnostr`, `libnostr-publish` `= %{version}-%{release}`, **`gvfs%{?_isa} = %{gvfs_version}`** |
| Recommends | `gvfs-fuse`, `gnostr-signer-daemon`, `nostrc-session-relay` | `Recommends:` same |
| Suggests | `nostr-share`, `nostr-seal`, `nostr-settings` | — |
| Files | `/usr/libexec/gvfsd-blossom`, `/usr/libexec/gvfsd-nostr`, `/usr/share/gvfs/mounts/{blossom,nostr}.mount`, `/usr/share/glib-2.0/schemas/org.nostr.gvfs.gschema.xml`, `/usr/share/man/man7/nostr-gvfs.7.gz`, `README` (supported gvfs floor, rebuild note) | same |
| Triggers | `glib-compile-schemas` (dh_installgsettings) | `%transfiletriggerin` glib2 schemas |

Nothing in the headless closure depends on this package; the dependency-purity gate
(`packaging-plan-debian-fedora.md` §8) is unaffected and the `gvfs` match in its
regex is exactly why the backends are a separate package. `ENABLE_GVFS_NOSTR=OFF`
must stay `-Werror` clean (feature-OFF CI job, precedent `nostrc-6quj`).

---

## 11. Non-goals for v1 and follow-ups

| Punted | v1 behaviour | Follow-up |
| --- | --- | --- |
| NIP-05 hosts (`nostr://alice@example.com/`), `nprofile1…` hosts | `INVALID_ARGUMENT` | W-later: resolve `.well-known/nostr.json` / relay hints → npub, mount spec rewritten |
| `nostr:///` "me" shortcut | not a valid mount | bookmarks use the explicit npub |
| Editing `profile.json` / `relays.json` | `READ_ONLY` | Q10 |
| Rename in `nostr://<self>/files/` (republish 1063 with new `name`) | `PERMISSION_DENIED` | W-later |
| NIP-94 events without `x` | hidden | Q11 |
| BUD-04 mirror to all 10063 servers, BUD-05 media variants, BUD-09 reports | first-accepting server only; `thumb`/`image` tags exposed as attributes, not fetched | W-later |
| NIP-96 upload | read-only consumption of NIP-96-hosted 1063s works (non-canonical URL path, D13) | none planned |
| NIP-42 AUTH on query relays | auth-requiring relays are unreachable for reads | libnostr-publish async auth callback |
| Volume monitor (unmounted entries in the sidebar) | bookmarks (D21) | W-later |
| `blossom-cache` as the shared local CAS | private cache (D15) | Q7 |
| Per-server Content-Type negotiation / PNG shim for body-sniffing servers | `415` surfaced with a hint | none planned (D12) |
| Thumbnails from `thumb` tags, blurhash placeholders | attributes only | Files has no hook for remote-provided thumbnails today |
| Folders / albums inside `files/` | flat | no NIP defines them |
| Flatpak apps without gvfs talk-names browsing the mounts | portal-delivered files only | portal-mediated share path |
| Other people's *private* files, DMs, MLS | out of scope | Groundhog (`nostrc-qp24`) |
| Windows/macOS | GVfs is GNOME | — |

---

## 12. Testing strategy

**Where the existing pieces fit.** `apps/blossom-cache` is a spec-complete BUD-01/02
server **without auth validation** — ideal for the "unauthenticated local server"
and range/streaming scenarios, wrong for auth tests. `libhanami` supplies
`hanami_bud02_validate_auth_event()` and the capability findings (server-tag
rejection, 415 on random bytes), so the **fixture Blossom server links libhanami**
to validate what the backends sign, while the backends themselves do not link it.
`libnostr-publish`'s fixture transport drives relay unit tests in-process;
end-to-end runs the real session relay on a temp socket.

| Layer | Harness | Covers |
| --- | --- | --- |
| **Unit** (GTest, no network, no gvfsd) | `test_names` (D8 precedence, sanitise, collisions, hostile names: `..`, NUL, 300-byte, RTL override, leading dot), `test_events` (1063/imeta/kind-5 parsing, missing `x`/`url`, bad sig dropped, replaceable tie-break, `nostr::sources`), `test_bud02` (event shape validated by libhanami; header base64; expiration), `test_blobcache` (admission cap, LRU by mtime, flock, sha mismatch leaves nothing, startup sweep), `test_http` (canonical-URL + declared-size rule, `size` ceiling, range mapping), `test_media` (detector agrees with `ns_strip_metadata` on nostr-share's `test_strip` corpus), `test_error_map` (HTTP → GIO table) | `ns-core` and the common lib |
| **Component** (backend cores without gvfsd) | `GnBlossomCore` / `GnNostrCore` are job-free objects the vfuncs wrap; tests drive them with the fixture Blossom server and fixture transports, using `GMainLoop` | listing, upload, delete, mixed-source delete refusal, 1063 publish verdicts, offline index, monitor diffs |
| **Fixtures** (W7a, before W3) | `tests/fixtures/blossom-fixture` (libsoup 3, ~400 lines): BUD-01/02/06, `/list` with optional auth requirement and `since`/`until`, fault injection over a control endpoint (`wrong-bytes`, `slow`, `413`, `415`, `401`, `507`), records every request and `Authorization` header; `tests/fixtures/seed-relay.sh`: starts `nostr-session-relayd` on a temp `$XDG_RUNTIME_DIR/nostr/relay.sock` and publishes signed fixture events with an in-process libnostr key (until `nostrc-prqu.14` lands it runs the relay in cache mode, where EOSE is sent); signer: `nostr-signer-daemon` on a private bus with an ACL allow-list, as `test-nseal-signer.c` does | shared by component and e2e |
| **End-to-end** (W7b; skip-if-absent: needs installed `gvfsd`, `gio`; runs as user, no root) | `GTestDBus` private session; `GVFS_MOUNTABLE_DIR=$build/gnome/gvfs-nostr/data`; spawn the distro's `${libexecdir}/gvfsd` on that bus; `GIO_USE_VFS=gvfs`; `allow-insecure-loopback` on via `GSETTINGS_BACKEND=memory`; then `gio mount/list/info/copy/remove/monitor` | scenarios below |

End-to-end scenarios (`tests/e2e/run_gvfs_nostr_*.sh`):

| # | Scenario | Asserts |
| --- | --- | --- |
| a | `gio mount blossom://127.0.0.1:P/` → `gio list` | names `<sha>.<ext>`, sizes, `nostr::sha256`; exactly one `/list` request |
| b | `gio cat` uncached then cached | one GET; second read zero GETs; blob in `blobs/aa/…` 0600; `nostr::verified=true` |
| c | wrong-bytes fault | read fails `G_IO_ERROR_FAILED`; nothing in `blobs/`; second server (fallback) serves |
| d | `gio copy photo.jpg blossom://…/` with progress | fixture saw `Authorization: Nostr` validated by libhanami, `t=upload`, `x` matches; `gio list` shows `<sha>.jpg`; monitor got `CREATED`; `gio info blossom://…/photo.jpg` is `NOT_FOUND` (documented) |
| e | oversize / `413` / `415` / `507` | `MESSAGE_TOO_LARGE` / `FAILED` with hint / `NO_SPACE`; no spool leftovers |
| f | `gio remove` | `DELETE` with `t=delete`; `DELETED` event |
| g | `gio mkdir`, trash, `gio rename` | `NOT_SUPPORTED`, `NOT_SUPPORTED`, `PERMISSION_DENIED`; server untouched |
| h | metadata-bearing JPEG drop | refused with the D19 message; with `allow-media-metadata` it uploads |
| i | `gio mount nostr://<seeded npub>/` (foreign) | `profile.json`, `relays.json`, `files/` names per D8 incl. one collision suffix and one imeta-sourced entry; `filesystem::readonly=TRUE`; write ⇒ `READ_ONLY` |
| j | self mount + drop `report.pdf` | fixture got the upload; relay got a verified kind 1063 with `name`, `x`, `ox`, `size`; `files/` shows it; publish verdict required a direct relay ACK |
| k | kind 5 on the relay | entry disappears on next enumerate / `DELETED` under a monitor |
| l | `.nsealed` drop (when `ENABLE_NOSTR_SEAL`) | 1063 carries the recipients' `p` tags; `standard::content-type=application/vnd.nostr.sealed` |
| m | offline (`GIO_USE_NETWORK_MONITOR=base` + fixture down) | mount from index; cached read ok; uncached read `NOT_CONNECTED` < 1 s; `gio mount -u` immediate |
| n | opaque `nostr:nevent1…` | `gio info nostr:nevent1…` fails `INVALID_ARGUMENT` fast; `gio open nostr:nevent1…` and `gio open nostr://<npub>/` both reach the dispatcher (stub handler on the private bus), which opens Files for the latter without re-entering itself |
| o | gvfs-fuse (when `gvfs-fuse` present) | `cat $XDG_RUNTIME_DIR/gvfs/blossom:host=…/<sha>.jpg` matches |
| p | unmount with upload in flight | `BUSY` without force; force aborts; spool empty |
| q | ABI smoke on gvfs 1.54 and newest | (a)+(i) pass in both containers; the same job is the rebuild-on-gvfs-bump trigger |
| r | same-mount `gio move` on `blossom://` | `PERMISSION_DENIED`; **zero** PUTs and zero DELETEs on the fixture |
| s | mixed-source delete in `nostr://<self>/files/` | refused; no kind 5 published |
| t | session relay without EOSE (only until `nostrc-prqu.14` lands) | listing completes via the 500 ms idle fallback within the 1.5 s budget |
| u | `blossom:///` | entries are `SHORTCUT` with `standard::target-uri`; `gio copy` into the root ⇒ `NOT_SUPPORTED` |

**Lab acceptance (maintainer, manual)** on the amd64 GNOME VM `gnome-dev`: mount
`blossom://blossom.sharegap.net/` and `nostr://<own npub>/` against
`relay.sharegap.net`; drag a PDF in from Files; open a peer's `.nsealed`; record
first-fetch time of a ~50 MiB blob; flip `docs/ACCEPTANCE_MATRIX.md:28` from
"Unavailable" to the run reference.

---

## 13. Work items

Sizes: **S** ≤ 2 engineer-days, **M** 3–5, **L** 6–10. Titles are bead-ready
(`bd create --type=feature --priority=2 …`; parent `nostrc-jaxi`).

| # | Title | Size | Depends on | Deliverable |
| --- | --- | --- | --- | --- |
| **W0** | `gvfs-nostr: build seam against pinned gvfs source; empty gvfsd-blossom/gvfsd-nostr that mount and unmount` | M | — | `ENABLE_GVFS_NOSTR`, `GVFS_SOURCE_DIR`/`GVFS_VERSION`, `config.h` shim, `daemon-main` glue, both backends registering a mount with `try_mount`/`try_unmount`/empty `try_enumerate`, `.mount` files, GSettings schema, `GVFS_MOUNTABLE_DIR` e2e harness skeleton, feature-OFF `-Werror` job, ABI smoke on 1.54 + newest (§12 q) |
| **W1** | `ns-core + gvfs-nostr common: content-addressed blob cache, streaming sha-verified HTTP fetch, range reads, LRU eviction, name sanitiser, mime↔ext` | M | W0 | `gnome/nostr-share/core/` skeleton (mime↔ext, sanitiser moved from nostr-share, behaviour-preserving), `gn-blobcache`, `gn-http`, `test_blobcache`, `test_http`, `test_names` |
| **W2a** | `libnostr-publish: REQ-until-EOSE query helper with signature verification (NostrPublishQuery)` | M | — | one-shot and live subscriptions over `NostrPublishTransport`, per-relay deadlines, verified events, fixture-transport tests. Scope stops at the helper: migrating `ns_net_fetch_replaceable` / `nd_fetch` is a separate bead. Files the async NIP-42 callback follow-up. |
| **W2b** | `gvfs-nostr common: relay resolution (session socket → NIP-65 write relays), event parsing, async signer wrapper, kind-24242 builder` | M | W2a, **`nostrc-prqu.14`** (or ships the session-socket idle fallback and a bead to remove it) | `gn-relay`, `gn-events`, `gn-signer`, `ns-core` kind-24242 builder (+ libhanami-validated test), `test_events`, `test_bud02` |
| **W7a** | `gvfs-nostr fixtures: libsoup3 Blossom fixture server (libhanami-validated auth, since/until, fault injection), seeded session relay, private-bus signer` | M | W0 | §12 fixtures; **before W3** |
| **W3** | `gvfsd-blossom: read path — /list index, enumerate/query_info, open/read/seek/pull, monitors with since watermark, blossom:/// shortcut root` | M | W1, W2b, W7a | §3.1 read half, D23, §5, §7.3; e2e a–c, u |
| **W4** | `gvfsd-blossom: write path — push/create/write/close_write uploads with progress, BUD-06 precheck, size limits, delete, move → PERMISSION_DENIED, media-metadata refusal` | L | W3 | §3.1 write half, `ns-core` metadata detector (from `ns-strip.c`), error map incl. 415, `readonly_lockdown`; e2e d–h, p, r |
| **W5** | `gvfsd-nostr: read path — profile.json/relays.json, files/ from kind 1063 + imeta + kind 5, naming/dedup/sources, foreign read-only, offline index` | L | W1, W2b, W7a | §3.2 D6–D8, D16; e2e i, k, m, n |
| **W6** | `gvfsd-nostr: self write — upload to kind-10063 servers + publish kind 1063 via NostrPublisher; delete → kind 5 with mixed-source guard (+ blob delete when unreferenced); move refused; .nsealed p-tags` | M | W4, W5 | §3.2 D9; e2e j, l, s |
| **W7b** | `gvfs-nostr e2e: GTestDBus + private gvfsd harness, scenario scripts a–u, dispatcher stub` | L | W7a, W6 | §12 e2e table |
| **W8** | `packaging: nostr-gvfs-backends (Debian + RPM) with exact gvfs-libs/gvfs pin, rebuild-on-bump CI, schema triggers, man page, README floor note, lab acceptance + ACCEPTANCE_MATRIX row` | M | W6, W7b | §10.4; `debian/control` + `.install` + `copyright`, spec `%package`, `nostr-gvfs.7`, lab run recorded |
| **W9** | three small integrations: (i) `nostr-dispatcher: open nostr:// URIs with an authority in Files via the inode/directory handler (never launch_default_for_uri)`; (ii) `org.nostr.Settings Files page: org.nostr.gvfs keys, sidebar bookmarks toggle, clear cache`; (iii) `nostr-share: skip re-upload for files carrying nostr::sha256/url; emit name tag on kind 1063` | S each | W5 / `nostrc-janr` / W6 | §8.3, §9 |
| **W10** | `upstream: MR to GNOME/gvfs adding backends blossom and nostr` | M (+ review latency) | W8 accepted | meson snippet, gvfs coding style pass, CI; until merged the pinned build stays |

Critical path: W0 → W1/W2/W7a → W3 → W4 → W6 → W7b → W8 (≈ 7–9 weeks of one
engineer; W5 runs in parallel with W3/W4). W2b is gated on `nostrc-prqu.14`.

---

## 14. Decisions

| # | Decision | Options | Recommendation |
| --- | --- | --- | --- |
| **D1** | Delivery shape | (a) third FUSE daemon; (b) in-process GIO module; (c) gvfsd backends | **(c).** Sidebar, gvfs-fuse, no `/dev/fuse`, lifecycle owned by gvfsd; (a) repeats both FUSE trees' plumbing; (b) is the wrong process boundary. §2 |
| **D2** | `blossom://` listed pubkey | (a) always in the mount spec; (b) `user=` when given, else the signer's identity; read-only unless they match | **(b)**, the `sftp://` convention; remount to switch the writable identity. §3.1 |
| **D3** | `blossom://` names | (a) remembered original names in a sidecar; (b) `<sha256>.<ext>` | **(b).** It is a CAS; names come from events in `nostr://`; one mount per (server, pubkey). §3.1 |
| **D4** | Refused mutations | `READ_ONLY`; `NOT_SUPPORTED`; `PERMISSION_DENIED` | **`PERMISSION_DENIED` for move/rename** (a `NOT_SUPPORTED` makes `g_file_move()` copy+delete the same blob); `NOT_SUPPORTED` for mkdir/symlink/append/truncate/trash; `READ_ONLY` only on foreign `nostr://` mounts. §3.1, §3.2 |
| **D5** | Scheme and routing | (a) `nostr://` + dispatcher opens authority URIs in Files via the `inode/directory` handler; (b) `npub://` | **(a)**; launching never reaches gvfs, browsing never reaches the dispatcher; Q1. §3.2 |
| **D6** | Root of `nostr://<npub>/` | `profile.json`, `relays.json`, `files/`, all read-only | as listed; no `servers.json` in v1. §3.2 |
| **D7** | Fetch set and `files/` sources | 1063 only; 1063 + kind-1 `imeta`; + kind 5 | one query for kinds `0,10002,10063,1063,1,5`; entries from **1063 + imeta (capped scan) + kind 5**, requiring `x` and `https` `url` (spec-valid `x`-less 1063s hidden, Q11). §3.2 |
| **D8** | Naming and dedup | by event; by sha | **by sha**; name = `name` tag → non-hash URL basename → `<sha12>.<ext>`; sanitised; `~<sha6>` on collision; `nostr::sources` recorded. §3.2 |
| **D9** | Self writes | (a) none; (b) upload + 1063, delete = kind 5 (+ blob delete if unreferenced), refused for anything a note references | **(b)**, with the mixed-source guard. §3.2 |
| **D10** | Relay order | session socket → target's NIP-65 **write** relays (discovered via session → own relays → `discovery-relays`), cap 4, no hard-coded relays | as stated; corrects the bead's "read relays". §4 |
| **D11** | Query shape | polling; REQ/EOSE one-shot; REQ kept live under monitors | **live REQ under monitors**, EOSE-terminated, helper in libnostr-publish; **gated on `nostrc-prqu.14`** with a session-socket-only idle fallback until then. §4 |
| **D12** | HTTP stack | libhanami (curl, sync); libsoup 3 | **libsoup 3**; libhanami only in the fixture; no Content-Type negotiation/shim. §5.1 |
| **D13** | Which URL | event `url` blindly; canonical-first with full verification for non-canonical; fallbacks; author's servers | **canonical + declared size streams; everything else verified before serve; then `fallback`, other events' URLs, 10063 servers**. §5.2 |
| **D14** | Serve-while-downloading | (a) full download then serve; (b) stream with EOF check and handle poisoning | **(b)** under the D13 preconditions, `nostr::verified` visible. §5.3 |
| **D15** | Cache | per-backend; shared CAS in `$XDG_CACHE_HOME/gvfs-nostr`; `blossom-cache` daemon | **shared private CAS**, LRU by mtime, 2 GiB / 256 MiB admission, startup sweep; `blossom-cache` is Q7. §5.4 |
| **D16** | Offline | fail mount; mount from index with fast `NOT_CONNECTED` on misses | **index-backed mount, never hang**. §5.7 |
| **D17** | gvfs ABI | (a) out-of-tree pinned source + **exact** `gvfs-libs`/`gvfs` package pin + rebuild-on-bump CI; (b) upstream first; (c) distro patch | **(a) now, (b) as W10**; floor 1.54 (Ubuntu 24.04 / Debian 13+). §10.2 |
| **D18** | Auth UX | passwords via `GMountOperation`; signer approval only + one mount-time `ask_question` | **signer only**; app_id `org.nostr.Files`; unmount is `BUSY`/force, no prompt. §6.2 |
| **D19** | Media metadata on upload | strip in the backend; allow; refuse unless setting | **refuse unless `allow-media-metadata`** (a filesystem must store what it was given). §6.4 |
| **D20** | Process model | `do_*` threads; `try_*` async | **`try_*` async on the main context** for every vfunc incl. push/pull; CPU-bound work in `GTask` threads; async signer wrapper; NIP-42 only on publish transports. §7.1 |
| **D21** | Sidebar discoverability | volume monitor; bookmarks written by Settings | **bookmarks** in v1. §8.1 |
| **D22** | Configuration | conf file; env; GSettings `org.nostr.gvfs` | **GSettings** (precedent `org.gnome.system.smb`); Q6 on unifying with nostr-share. §10.3 |
| **D23** | `blossom:///` shape | nested tree of per-server folders; root of `G_FILE_TYPE_SHORTCUT` entries with `target-uri` | **shortcuts** (the `network:///` shape): one real mount per server, no duplicate views, no same-server cross-mount move hazard. §3.1 |

---

## 15. Open questions for the maintainer

1. **Scheme (D5).** Keep `nostr://` for the GVfs namespace (the dispatcher opens
   authority URIs in Files; opaque NIP-21 URIs are untouched), or use a distinct
   scheme (`npub://`) and avoid sharing the scheme at the cost of the guessable URI?
2. **gvfs pin (D17).** Accept shipping `nostr-gvfs-backends` with an *exact*
   `gvfs-libs` / `gvfs` version dependency (rebuilt on every gvfs update, CI-driven)
   until the upstream MR lands? The alternative is not shipping before upstream.
3. **Media metadata (D19).** Refuse metadata-bearing photos/videos on drag-drop by
   default (consistent with nostr-share's CLI), or accept them silently because it
   is a filesystem? The default decides whether "copy a phone video into
   `nostr://me/files/`" works out of the box.
4. **`name` tag (D8).** Emit `["name", <filename>]` on our kind 1063 (and have
   nostr-share do the same, W9), or keep names URL-only? It is an undocumented
   extension; other clients ignore it.
5. **Signer ACL.** app_id `org.nostr.Files` for both daemons, with the consequence
   that a remembered "allow" makes uploads from *any* app writing into a mount
   prompt-free. Alternatively never allow "remember" for this app_id (signer-side
   change).
6. **Configuration ownership (D22).** Should `nostrc-janr` make one `org.nostr.*`
   GSettings tree the source of `upstream-mode`, `blossom-servers`, relays for
   nostr-share, nostr-seal and these backends, retiring `nostr-share.conf`?
7. **`blossom-cache` (D15).** Keep the private CAS, or promote `apps/blossom-cache`
   to a socket-activated user service and make it the single local Blossom cache
   for gnostr *and* the backends (then the backends fetch through
   `http://127.0.0.1:24242/<sha>?xs=…`)?
8. **Blob deletion (D9).** On `rm` in `nostr://me/files/`: kind 5 only, or also
   `DELETE /<sha>` on the servers when nothing else of mine references the sha?
   The latter is destructive across every client; the former leaves orphans.
9. **`blossom:///` shortcut set (D23).** Only the current kind-10063 servers, or
   also servers that ever appeared in my 1063/imeta URLs (a "previously used"
   section)?
10. **Editing `profile.json`/`relays.json`.** Confirm read-only for v1 (a save from a
    text editor would republish kind 0 / 10002 — powerful and easy to get wrong).
11. **`x`-less NIP-94 events (D7).** Hidden in v1. Should a later version list them
    as `nostr::verified=false` entries (readable, never cached), or stay hidden?
12. **Home of the shared core (§9).** `gnome/nostr-share/core/` as a private static
    `ns-core` (keeps ownership with the code it came from), or a new top-level
    `gnome/libnostr-media/`?

---

## 16. References

- `docs/investigations/gnome-gtk-nostr-planned-not-implemented-2026-09-26.md` —
  Cluster 4 (FUSE), Cluster 5 (share-to), §"Work items filed" (`nostrc-jaxi`).
- `docs/designs/nostrfs-porthome-overlay.md` — PHO-D1 (why FUSE-as-`$HOME` was
  rejected), PHO-D9 (bounded offline), PHO-D11/§8 (`st_dev`, gvfs coexistence),
  §3 (`nh_syncd_cache` LRU precedent), §14 (test-plan shape), §15 item 4 (indexer
  policy).
- `docs/designs/home-from-relay.md` §3.2 (FUSE hazards), §7.2 (packaging rows).
- `docs/designs/packaging-plan-debian-fedora.md` §8 (dependency-purity gate).
- `gnome/nostr-share/README.md`, `src/{ns-blossom,ns-net,ns-event,ns-strip}.h`.
- `gnome/nostr-seal/README.md`, `include/nostr-seal.h`.
- `gnome/libnostr-publish/include/nostr-publish/*.h`.
- `libhanami/include/hanami/{hanami-blossom-client,hanami-bud02-auth,hanami-server-capability}.h`.
- `apps/blossom-cache/README.md`; `gnome/nostr-dispatcher/README.md`, `src/nd-fetch.h`.
- `apps/gnostr-signer/data/dbus/org.nostr.Signer.xml`.
- gvfs master (2026-09-26): `daemon/gvfsbackend.h`, `daemon/mount.c:466–495`,
  `daemon/meson.build`, `NEWS` (1.53.90 OneDrive; 1.59.1 Google deprecated;
  1.61.90 auto-unmount).
- GLib `gio/gappinfo.c` (`g_app_info_launch_default_for_uri`: scheme handler before
  `GFile`), `gio/gfile.c` (`g_file_move`: copy+delete fallback on `NOT_SUPPORTED`).
- Specs: NIP-01, NIP-09, NIP-19, NIP-21, NIP-65, NIP-92 (`docs/nips/92.md`), NIP-94
  (`docs/nips/94.md`), BUD-01/02/03/04/06.
- Beads: `nostrc-jaxi`, `nostrc-prqu`, `nostrc-1xak`, `nostrc-da9c`, `nostrc-tmsc`,
  `nostrc-1v65`, `nostrc-2thp`, `nostrc-xlf3`, `nostrc-janr`, `nostrc-7d96`,
  `nostrc-prqu.14`, `nostrc-bpum`, `nostrc-6quj`, `nostrc-1u55`.

---

## 17. Revision log

- **2026-09-26 (Oracle critique pass, plan mode).** Folded in: scheme-handler-first
  dispatch and the recursion-safe dispatcher rule (D5, W9); `PERMISSION_DENIED` for
  move/rename because `g_file_move()`'s `NOT_SUPPORTED` fallback would delete the
  blob (D4, D9); `blossom:///` as a shortcut root (D23); `user=` semantics (D2);
  mixed-source delete guard and kind-5 advisory note (D9); explicit fetch set incl.
  kinds 0/10002/10063 (D7); `nostrc-prqu.14` gate with a session-socket-only idle
  fallback (D11); `x`-less 1063s hidden deliberately (D7, Q11); stream-while-fill
  preconditions and threat row (D13/D14, §6.1); unmount without prompts (D18);
  `gvfs-libs` exact pin, distro floor, LGPL note, rebuild trigger (D17); Flatpak
  honesty (§2, §7.5); `DBusName`/`DefaultPort` deliberately absent (§7.2); NOTICE
  non-terminal and the NIP-42 publish/query split (§4, §7.1); `since` watermark
  (§3.1); startup sweeps (§5.4); 415 handling (§3.1); `ns-core` home (§9, Q12);
  W7 split so fixtures precede W3; e2e r–u. Rejected from the critique: "`try_push`
  runs on job threads" (it does not — `try_*` vfuncs run on the main context, which
  is the point of D20; the real hazard, CPU-bound hashing on the main loop, is now
  in §7.1) and adding `DBusName=` to the `.mount` files (it would bypass the
  per-mount spawner protocol the backends rely on).
