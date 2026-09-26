# nostr-share — Share to Nostr

`nostr-share` publishes whatever GNOME hands it as the right Nostr event.
It is a small CLI plus a libadwaita dialog, registered through
`org.nostr.Share.desktop` so it shows up in **every application's Open
With** menu (xdg-desktop-portal has no share portal; Open With and
drag-and-drop are GNOME's real entry points). A Files context-menu entry
is tracked separately (nostrc-xlf3).

Bead: nostrc-1xak.

## For users

Right-click a photo in Files, a page in a browser's *Open With*, a
Markdown file, a git checkout… → **Open With → Share to Nostr**. The
dialog shows:

- a thumbnail of each file (the *cleaned* bytes that will be uploaded),
- **Publish as**: the event kind, with a sensible default (see below),
- **Mention / group**: an optional public mention or NIP-29 group,
- the text or caption, editable,
- **Privacy**: what metadata was removed from each photo,
- **Destination**: which relays and Blossom servers will be used,
- **Show event JSON**: exactly what your signer will be asked to sign.

Nothing is uploaded or published until you press **Publish**, and every
signature goes through your `org.nostr.Signer` (gnostr-signer), which may
ask for approval.

### Command line

```
nostr-share [--kind N] [--to npub|host'group] [--title T]
            [--dry-run] [--no-ui] [--keep-metadata]
            FILE… | -t TEXT | URL…
```

| option | meaning |
|---|---|
| `-t, --text TEXT` | text to share; repeatable; `-t -` reads stdin. With files it is the caption |
| `-k, --kind N` | override the kind (only kinds that make sense for the input are accepted) |
| `--to WHO` | `npub1…`/hex → a **public** `p`-tag mention (+ `nostr:npub…` in the content; the CLI says so on stderr). `host'group-id` → NIP-29 `h` tag, published **only** to `wss://host`; notes become kind-9 group messages |
| `--title T` | title for a long-form article |
| `-n, --dry-run` | build and sign, print the signed event(s); upload, publish and stage nothing |
| `--no-ui` | never open the dialog (it opens by default when a display is available) |
| `--keep-metadata` | allow uploading media whose metadata could not be stripped |

Exit status: `0` ok, `1` usage, `2` bad input / metadata refusal,
`3` no signer, `4` upload / publish / nostr-dav failure. The signed
events go to stdout, one JSON object per line; progress goes to stderr.

```sh
nostr-share --dry-run -t "hello"                 # prints a signed kind-1 event
nostr-share --no-ui https://example.com/article  # kind 1 + r tag
nostr-share --no-ui -t "sunset" ~/Pictures/a.jpg # Blossom + kind 1 with imeta
nostr-share --no-ui --kind 1063 report.pdf        # Blossom + NIP-94
nostr-share --no-ui ~/src/myproject              # NIP-34 repo announcement
nostr-share --no-ui meetup.ics                   # handed to nostr-dav
```

### Kind mapping

| input | default | `--kind` alternatives |
|---|---|---|
| text (`-t`, `text/plain`), any length | **1** note, `r` tag per http(s) URL | 30023 |
| `text/markdown`, `*.md` | **30023** article: `d` = slug of the title, `title`, `published_at` | 1 |
| URL argument, `text/uri-list` | **1** note, URL in content, `r` tag | — |
| `image/*`, `video/*`, `audio/*` | Blossom upload → **1** note, blob URLs in content, one NIP-92 `imeta` per file (`url`, `m`, `x`, `size`, `dim`) | 1063 (one NIP-94 event per file) |
| `application/pdf`, anything else | Blossom upload → **1063** (`url`, `m`, `x`, `ox`, `size`, `dim`?) | 1 (+`imeta`) |
| directory with a git repository | **30617** (NIP-34, built with `nips/nip34`): `d`, `name`, `description`, `clone`, `web`, `relays`, `["r", <root commit>, "euc"]` | — |
| `text/calendar`, `*.ics` | PUT to nostr-dav `calendars/nostr/<UID>.ics`; nostr-dav publishes (NIP-52). One event per file | — |
| `text/vcard`, `*.vcf` | PUT to nostr-dav `contacts/nostr/<UID>.vcf`; nostr-dav publishes. One contact per file | — |
| any other directory | refused | — |

Notes:

- Plain text stays a kind-1 note however long it is: a 30023 article is
  addressable (`d` tag) and a re-share would *replace* it, which is only
  what you want when you asked for an article (`--kind 30023`) or shared
  a Markdown file.
- With `--to host'group`, kind-1 notes (text and media) are published as
  NIP-29 **kind 9** group messages — what group clients render. Articles,
  1063 and 30617 keep their kind and gain the `h` tag.
- `nostr:` references (NIP-27) are left untouched in the content and get
  no tags from us; `r` tags are only added for http(s) links the user
  wrote, never for our own Blossom URLs.
- Several media files make **one** kind-1 note with one `imeta` each.
- The article `d` tag is the slug of the title, so re-sharing the same
  article replaces it rather than duplicating it.
- Git clone URLs have credentials stripped; local paths and `file://`
  remotes are never published. A repository with no public remote is
  refused.

### Privacy

- **Image metadata is stripped before upload** by a small container-level
  stripper in `src/ns-strip.c` (GExiv2 is not a dependency anywhere in
  this tree). It never re-encodes pixels:
  - JPEG: drops APP1 (Exif, XMP), APP3–APP13/APP15 (IPTC/Photoshop), APP2
    MPF and COM segments and everything after EOI (MPF secondary images);
    keeps JFIF, ICC profile and Adobe APP14.
  - PNG: drops `eXIf`, `tEXt`, `zTXt`, `iTXt`, `tIME`.
  - WebP: drops `EXIF`/`XMP ` chunks and clears the VP8X flags.
  - GIF: drops comment and non-loop application extensions (XMP).
- **Other media** (HEIC/AVIF/TIFF, all video and audio) cannot be cleaned
  by nostr-share. The CLI refuses to upload them without
  `--keep-metadata`; the dialog shows a banner and keeps **Publish**
  disabled until you flip *Upload anyway, with metadata*. Phone videos in
  particular often carry GPS coordinates.
- A file whose container is malformed is refused outright — we never
  upload something we could not fully walk.
- PDFs and other documents are uploaded byte-for-byte (their author /
  producer fields are not touched).
- The event JSON is shown before signing. If a Blossom server answers with
  a URL different from the predicted `https://server/<sha256>.<ext>`, the
  dialog updates the JSON and asks you to press Publish again.
- `--to npub…` is a **public** mention, not a private message.

### Where events go

nostr-share asks your signer for your pubkey, then:

1. **Write relays**: your NIP-65 kind-10002 relay list (write/unmarked
   entries), fetched from the session relay and `home_relays`; events
   whose id or signature do not verify are ignored. Without a kind 10002
   it uses `home_relays` from the config.
2. **Session relay**: when `$XDG_RUNTIME_DIR/nostr/relay.sock` exists it
   also gets a copy (WebSocket over the Unix socket), so local apps see
   the event immediately.
3. **Success** means at least one *write relay* accepted the event. The
   session relay's ACK alone does not count: it does not federate writes
   upstream yet, so an event only it holds never leaves this machine.
   `upstream_mode` in the config changes this (`session_relay_or_direct`,
   `session_relay_only`, `direct_only`); `session_relay_only` warns that
   nothing leaves the machine.
4. **NIP-29 groups** (`--to host'group`) go to the group relay only — never
   to your public write relays or the session relay.
5. **Blossom servers**: your BUD-03 kind-10063 list, else
   `blossom_servers` from the config; `https://` only. Uploads are
   BUD-02 `PUT /upload` with a kind-24242 auth event signed by your
   signer and the file's real `Content-Type`.

### Configuration

`~/.config/nostr-share/nostr-share.conf` (or `$NOSTR_SHARE_CONFIG`); see
`nostr-share.conf.example`. Keys: `home_relays`, `blossom_servers`,
`upstream_mode`, `max_upload_mib` (default 100), `ok_wait_sec`
(default 15), `dav_url`.

## For app authors

You do not need any Nostr code to offer "Share to Nostr":

- **Launch it with files or URLs** — it is a normal desktop app:
  ```c
  g_autoptr(GDesktopAppInfo) share = g_desktop_app_info_new("org.nostr.Share.desktop");
  GList *files = g_list_append(NULL, file);          /* GFile* */
  g_app_info_launch(G_APP_INFO(share), files, NULL, NULL);
  ```
  or `gio launch /usr/share/applications/org.nostr.Share.desktop FILE`.
  `Exec=nostr-share %U`, so `file://`, GVfs URIs and `http(s)://` URLs
  all work.
- **Text**: write it to a temporary `.txt`/`.md` file and pass that, or
  run `nostr-share -t "…"` (add `--title` for an article).
- **Pick the kind** with `--kind` when your app knows better (e.g. a
  document viewer sharing a PDF as a kind-1 post: `--kind 1`).
- **Headless / scripted**: `nostr-share --no-ui …` prints the signed
  events to stdout; `--dry-run` shows them without publishing. Check the
  exit status (above).
- **Mime types** it registers: `text/plain`, `text/uri-list`,
  `text/markdown`, `image/*`, `video/*`, `audio/*`, `application/pdf`,
  `inode/directory`, `text/calendar`, `text/vcard`. If your app uses
  `GtkAppChooser`/Open With for those, it is already listed.

## Building

```sh
cmake -B build -DENABLE_NOSTR_SHARE=ON -DBUILD_LIBHANAMI=ON
cmake --build build --target nostr-share
ctest --test-dir build -R nostr-share
```

Requires GLib ≥ 2.70, json-glib ≥ 1.6, libsoup-3, libnostr-publish,
libhanami (Blossom), `nips/nip34`, `nips/nip19`. The dialog needs gtk4 ≥
4.10 and libadwaita ≥ 1.4 and is skipped (CLI only) when they are
missing (`-DNOSTR_SHARE_UI=OFF` forces that).

Tests (no network; relays are libnostr-publish fixture transports and
the signer is an in-process libnostr key):

- `test_kind` — the kind-mapping table and `--kind` validation
- `test_event` — URL → `r` extraction, `imeta` / 1063 / 30023 tags, `--to`
- `test_strip` — JPEG/PNG/WebP/GIF metadata removal and dimensions
- `test_share` — end to end: publish through fixtures, verified kind
  10002 lookup (forged events ignored), session-relay success rule,
  group isolation, media `imeta`/1063, metadata refusal, article,
  nostr-dav hand-off, NIP-34 from a real `git init`.
