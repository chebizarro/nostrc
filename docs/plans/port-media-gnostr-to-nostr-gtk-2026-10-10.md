# Port plan: Gnostr media viewer and video player into nostr-gtk (2026-10-10)

Status: plan only (read-only investigation). Scope: `apps/gnostr/src/ui/gnostr-image-viewer.{c,h}` (1231 lines),
`apps/gnostr/src/ui/gnostr-video-player.{c,h}` (1269 lines) vs the alpha-6 reimplementation
`nostr-gtk/src/gn-media-viewer.c` (482) + `nostr-gtk/src/gn-animated-image.c` (497), and their call sites.

## 0. Current state (what alpha 6 actually did)

- `GnMediaViewer` (portable, `nostr_gtk_portable`) is a new, thinner window. Gnostr did **not** switch to it:
  `GnostrImageViewer` creates a *hidden, never-shown* `GnMediaViewer` purely as a gallery/index counter
  (`gnostr-image-viewer.c:62,166-167,906,943-944,1136,1161-1162`) and still renders everything itself. That hidden
  window is the "adapter" to delete.
- `GnostrVideoPlayer` has no portable counterpart. Its callers are in the Gnostr-coupled half of nostr-gtk:
  `nostr-gtk/src/nostr-note-card-row.c:259-266,405,525-528,3686` and `nostr-gtk/src/gnostr-profile-pane.c:3651-3653`.
- `GnostrImageViewer` callers: `apps/gnostr/src/ui/gnostr-picture-grid.c:638`,
  `nostr-gtk/src/nostr-note-card-row.c:3521-3530,3568-3589`, `nostr-gtk/src/gnostr-profile-pane.c:710-712,3687-3693`
  (the last two are in `NOSTR_GTK_GNOSTR_COUPLED_SOURCES`, `nostr-gtk/CMakeLists.txt:126-131`).
- Groundhog uses only the portable API, always with already-loaded paintables (no `load-requested` handler):
  `gnome/groundhog/src/ui/gh-message-row.c:13,535-551,624-627`, `gh-attachment-card.c:6,324-345,369,403,420-491`,
  `gh-web-content.c:139-159`. Tests: `gnome/groundhog/tests/ui/test_attachment_ui.c:40-41,1447-1449,1815-1920`,
  `tests/ui/test_conversation_view.c:40-41,1712-1777`; library: `nostr-gtk/tests/test_portable.c:196-262,338-380`.
- Portable target pins `GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_6` (`nostr-gtk/CMakeLists.txt:247-251`). This is the
  hard constraint the Gnostr code currently violates (see 1.3).

## 1. Feature / behaviour diff

### 1.1 Gnostr image viewer has, reimplementation lacks
| Feature | Gnostr ref | GnMediaViewer |
|---|---|---|
| Fit computes actual scale, never upscales; zoom label shows real % in fit mode | `gnostr-image-viewer.c:402-432` | label just "Fit" (`gn-media-viewer.c:93-95`) |
| Additive zoom steps 0.25 from the *actual* fit scale (no jump from fit to 125%) | `:444-454` | multiplicative from 1.0 (`:139-140`) |
| Pinch zoom (`GtkGestureZoom`) | `:668-680` | none |
| Key `1`/KP_1 = 100%, KP_0 = fit | `:481-494` | only `0`/`f` |
| Arrow keys pan when zoomed >1, navigate otherwise; Up/Down pan | `:511-549` | arrows always navigate (`:165-166`) |
| Click outside the image closes | `:643-666` | none |
| Loading spinner | `:216-222,846-848` | none |
| Blocked / unavailable state with message ("Remote media is blocked", "Remote images unavailable", "Remote image URL is invalid") | `:226-240,353-392` | only a bare "Load image" button (`:254-259`) |
| HTTP status / empty-body handling, off-thread decode | `:710-825` | n/a (no fetch, correct) — but no *error* state API for the host |
| Save image (Ctrl+S) | `:250-256,495-499,1019-1085` | none |
| Copy link (Ctrl+C) + toast | `:503-509,1087-1106` | none |
| URL hint for an already-loaded texture | `:961-965` | implicit via gallery URL |
| Present sizing clamps to parent, min 400x300 | `:1176-1205` | sizes in `_new()` from parent (`:358-368`), before parent is allocated in some paths |

### 1.2 Gnostr video player has, reimplementation lacks
GnMediaViewer only hosts a `GtkMediaStream` with stock `GtkMediaControls` (`gn-media-viewer.c:62-77`). Missing:
inline (in-card) widget with custom OSD controls, auto-hide (`gnostr-video-player.c:351-402`), seek/time labels
(`:221-235,267-293,404-457`), mute/volume/loop/stop (`:249-265,295-298,476-490`), keyboard space/k/f/m/Esc
(`:320-343`), fullscreen window with AdwToastOverlay hint (`:1120-1180` region), scroll-visibility auto-pause/resume
(`:620-700`), one-time media-backend probe (`:908-916`), loading/error states (`:107-219`), autoplay/loop
settings (`:827-896`), clean stream teardown order (`:739-822`).

### 1.3 What the reimplementation does better (must be kept)
- **No fetching in the widget**: `load-requested(index,url)` signal; navigation never fetches (`gn-media-viewer.c:8,457-463`);
  generation-guarded results (`set_texture_for_generation`, `:437-443`). Gnostr fetches inside the widget via a shared
  soup session (`gnostr-image-viewer.c:827-897`) and auto-fetches on `set_image_url`/`navigate` when the global
  setting allows (`:899-929,1158-1173`).
- **Bounded input**: gallery capped at 256 (`:409`); zoom clamp 0.1-10 (`:469`); `GnAnimatedImage` checks logical
  screen size before decode, caps frames (1000) and decoded bytes (192 MiB), never uses gdk-pixbuf
  (`gn-animated-image.h:6-20`). Gnostr calls `gdk_texture_new_from_bytes` on arbitrary bytes with **no size or
  dimension bound** (`gnostr-image-viewer.c:716`); on GTK this falls back to gdk-pixbuf loaders for non PNG/JPEG/TIFF.
- **Animated GIF** playback tied to mapping (`gn_animated_image_attach`) — Gnostr viewer shows only the first frame.
- **Video pause on unmap/navigate/close** (`gn-media-viewer.c:200-213,232-238`).
- **i18n** through its own `nostr-gtk` domain (`gn-portable-i18n-private.h`, `:3,250`); Gnostr strings partly
  untranslated (`"Save image (Ctrl+S)"` `:254`, `"Save Image"` `:1033`, `"Link copied"` `:1104`, `"%u / %u"` `:980`,
  `"Remote media loading is disabled"` video `:996`).
- **a11y**: every icon button has an accessible label (`osd_button`, `:245-255`); Gnostr icon buttons have tooltips only.
- **GTK 4.6 floor**: Gnostr uses `gtk_picture_set_content_fit` (4.8; `gnostr-image-viewer.c:204,412,425`,
  `gnostr-video-player.c:935`) and `GtkFileDialog` (4.10; `:1032-1061`). These cannot move as-is into
  `nostr_gtk_portable` (MAX_ALLOWED 4.6). Use `gtk_picture_set_keep_aspect_ratio` (4.0) and an injected save path.
- **No app singletons**: Gnostr calls `gnostr_main_window_show_toast` (`:1104`), `gnostr_is_remote_media_allowed`
  (`:349`, video `:995`), `gnostr_get_shared_soup_session` (`:858`), `g_settings_new("org.gnostr.Client")`
  (video `:887`), includes `gnostr-main-window.h`, `gnostr-avatar-cache.h`, `../util/utils.h` (`:9-11`, video `:10`).
- Gnostr video `set_uri` passes the remote URL to `g_file_new_for_uri()` (`gnostr-video-player.c:1038`), i.e. the
  media backend (GStreamer/gvfs) does its own HTTP, bypassing any app proxy/Tor/consent control. Must not be ported.

### 1.4 Known Gnostr bugs not to carry over
- `on_save_clicked` with no texture opens the dialog then saves nothing (`:1047-1050`, `:1075-1083`).
- `set_texture` clears `cancellable` instead of cancelling, and no request is cancellable at all ("no cancellable on
  shared session", `:885`); late results are only dropped by weak-ref, not by gallery generation (a slow image N can
  overwrite image N+1 after navigation).
- Saved file always PNG-re-encoded regardless of chosen extension (keep re-encode — it strips metadata — but name the
  suggested file `.png`).

## 2. Gnostr dependencies to replace with injected interfaces

The library keeps the *signal* model (Groundhog's contract) and adds a small optional source interface so Gnostr
does not have to reimplement plumbing at each call site.

```c
/* gn-media-source.h (portable, new) */
typedef enum { GN_MEDIA_POLICY_ALLOW, GN_MEDIA_POLICY_ASK, GN_MEDIA_POLICY_BLOCKED } GnMediaPolicy;
G_DECLARE_INTERFACE(GnMediaSource, gn_media_source, GN, MEDIA_SOURCE, GObject)
struct _GnMediaSourceInterface {
  GTypeInterface parent;
  /* Never called from bind/construct/navigate unless policy == ALLOW; ASK -> only after the user's Load action. */
  GnMediaPolicy (*get_policy)(GnMediaSource *self, const char *url, GnMediaKind kind);
  void          (*fetch_async)(GnMediaSource *self, const char *url, GnMediaKind kind, gsize max_bytes,
                               GCancellable *c, GAsyncReadyCallback cb, gpointer data);
  GBytes       *(*fetch_finish)(GnMediaSource *self, GAsyncResult *res, char **out_mime, GError **error);
};
```
- `gn_media_viewer_set_source(GnMediaViewer*, GnMediaSource* /*nullable*/)`: with no source, behaviour is today's
  (emit `load-requested` only). With a source: Load / auto-load (policy ALLOW only) calls `fetch_async` with a
  per-generation `GCancellable` that is cancelled on navigate, `set_gallery`, close and dispose.
- Host-driven result API (used by Groundhog's signal path and by the source path internally):
  `gn_media_viewer_set_loading(self, generation, index)`, `gn_media_viewer_set_error(self, generation, index, const char *message)`,
  `gn_media_viewer_set_blocked(self, gboolean blocked, const char *reason)`, existing `set_*_for_generation`; add
  `gn_media_viewer_set_paintable_for_generation`.
- Decode, bounded and off-thread: `gn_media_decode_async(GBytes*, const GnMediaDecodeLimits*, GCancellable*, cb, data)` /
  `_finish -> GdkPaintable*` with `GnMediaDecodeLimits { gsize max_bytes; guint max_dimension; GnMediaFormats allowed; }`.
  Sniffs magic bytes; PNG/JPEG dimension probe before `gdk_texture_new_from_bytes`; GIF via `GnAnimatedImage`; other
  formats only if `allowed` includes `GN_MEDIA_FORMAT_PIXBUF_FALLBACK` (Gnostr opt-in for WebP etc.; Groundhog never).
  Move Groundhog's PNG/JPEG header probe (`gnome/groundhog/src/media/gh-metadata-strip.h:45-52`) or reimplement it
  here; Groundhog keeps its own copy until switched.
- Save: signal `save-requested(index, url, GdkPaintable*)` with default class handler that uses `GtkFileChooserNative`
  (4.0, portal-backed) and `gdk_texture_save_to_png`; Groundhog may override (it already has a portal Save path,
  `gh-attachment-card.h:30`). Property `can-save` (default TRUE).
- Copy link: library writes the clipboard (only for `http(s)` URLs; hidden for opaque slot names like Groundhog's
  `attachment:<id>/<n>`, `gh-attachment-card.c:481`) and emits `link-copied`; Gnostr adapter connects it to
  `gnostr_main_window_show_toast`. No toast dependency in the library.
- Video settings: `GnVideoPlayer` exposes `autoplay`, `loop`, `muted`, `volume` GObject properties; Gnostr binds them
  with `g_settings_bind(settings, "video-autoplay", player, "autoplay", G_SETTINGS_BIND_GET)` in its adapter/factory.
- Video source: `gn_video_player_set_stream(GnVideoPlayer*, GtkMediaStream*)` and
  `gn_video_player_set_file(GnVideoPlayer*, GFile* /*local or caller-vetted*/)`; plus a "not loaded" state with a
  Play/Load button emitting `load-requested(url)`. No `set_uri()` that hands remote URLs to the backend in the
  portable API. Gnostr's adapter keeps `gnostr_video_player_set_uri()` semantics by doing
  `g_file_new_for_uri()` itself (explicitly Gnostr policy, unchanged behaviour), or better by fetching via its media
  service into a `GMemoryInputStream` (follow-up decision; see risks).
- Remote-media allowed check: replaced by `GnMediaSource.get_policy` (Gnostr: `gnostr_is_remote_media_allowed() ?
  ALLOW : ASK`; Groundhog: from `GhLinkPolicy` keys `load-remote-images` / per-sender grants).

## 3. Groundhog requirements to preserve

Verified in `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md` and `gnome/groundhog/tests/check_privacy.py`:
- **Nothing fetched by default; each fetch an explicit per-item action through the configured network mode**
  (charter :60). Groundhog must keep the signal path or a `GnMediaSource` whose `get_policy` returns ASK unless the
  user preference / per-sender grant says otherwise (current auto-load: `gh-message-row.c:567-573`). Navigation,
  construction and bind never fetch — keep the `test_portable.c:235-246` assertions.
- **Network mode incl. Tor**: fetches go through `GhNetHttp`/`GhNetSession` (`gnome/groundhog/src/net/`), TLS without
  resumption (check_privacy `tls-resumption`), libsoup only in `src/net`/`src/media`. The library must therefore never
  link or call libsoup or `g_file_new_for_uri` on remote URIs in portable code (add a portable-lint rule).
- **No gdk-pixbuf**: rule is real — charter :212 ("PNG or JPEG by magic bytes ... GTK's built-in loaders, not gdk-pixbuf"),
  :147 (A9), and lint `no-gdk-pixbuf` (`check_privacy.py:35,346-347,564-592`). The lint scans only
  `gnome/groundhog/src/**`; portable nostr-gtk media code is outside it. Requirement: Groundhog's decode limits must
  exclude the pixbuf fallback, and `gdk_texture_new_from_bytes` may be called only after a PNG/JPEG magic check
  (otherwise GTK itself falls into gdk-pixbuf).
- Limits: images 2 MiB / 4096x4096 (charter :212), profile pictures up to 6 MiB within 4096 px
  (`gh-web-content.c:140-159,188-189`); animated GIF 4096 max dimension; attachments 25 MiB (charter D6).
- **No plaintext at rest** (P3, `no-tmp-cache`): save only to a user-chosen file; no cache/tmp writes in the viewer.
  Decrypted attachment video stays an in-memory `GMemoryInputStream` (`gh-attachment-card.c:433-436`).
- Cancellation on dialog close / navigate / account switch, invalidated by generation (alpha-6 plan §3.1 OG/viewer).
- Performance: `nostr-gtk/tests/test_bind_latency_budget.c`, `test_listview_recycle_stress.c`, `test_widget_churn_leaks.c`
  (nostr-gtk); Groundhog row bind budget via `gh-message-row` + `test_conversation_view.c`. Inline video widget must
  not create a `GtkMediaFile` at bind (Gnostr already defers creation until Play: `nostr-note-card-row.c:298`).
- GTK floor: Groundhog targets 4.14/1.5, but portable code must compile at 4.6/1.2 with MAX_ALLOWED 4.6.
- Existing tests to keep green: `test_attachment_ui.c` (viewer gallery from attachments, GIF in MLS group, video
  `GTK_IS_MEDIA_STREAM`), `test_conversation_view.c:1712-1777` (linked animated GIF opens viewer via Enter),
  `check_privacy.py` (including its mutation self-tests, `:1273-1276`).

## 4. File moves, build changes, call-site switches

### 4.1 Moves (use `git mv` so history follows the Gnostr implementation)
1. `apps/gnostr/src/ui/gnostr-image-viewer.c` -> `nostr-gtk/src/gn-media-viewer.c` (replace the alpha-6 file).
   Rename type to `GnMediaViewer`; delete fetch/soup code (`:691-897`), `gnostr_*` includes, testing env hook (`:850-856`);
   keep zoom/pan/pinch/keys/background-close/spinner/blocked/save/copy UI; graft from alpha-6: `load-requested`,
   generation, `set_paintable` + GtkMediaStream controls + pause on unmap, `GnAnimatedImage` attach, 256 cap, OSD CSS,
   accessible labels, `_()` via portable domain. Replace 4.8/4.10 APIs (1.3).
2. `apps/gnostr/src/ui/gnostr-image-viewer.h` -> merge public docs into `nostr-gtk/include/nostr-gtk-1.0/gn-media-viewer.h`.
3. `apps/gnostr/src/ui/gnostr-video-player.{c,h}` -> `nostr-gtk/src/gn-video-player.c`,
   `nostr-gtk/include/nostr-gtk-1.0/gn-video-player.h` (`GnVideoPlayer`), with GSettings/remote-URI removed (section 2).
   The viewer reuses `GnVideoPlayer`'s control bar for `GtkMediaStream` slots instead of stock `GtkMediaControls`
   (one video UI).
4. New: `nostr-gtk/include/nostr-gtk-1.0/gn-media-source.h`, `src/gn-media-source.c`, `include/.../gn-media-decode.h`,
   `src/gn-media-decode.c`.
5. Video/viewer CSS: move the `.image-viewer*` and `.video-*` rules from `apps/gnostr/data/ui/styles/gnostr.css` into
   the library's install-once provider (as `gn-media-viewer.c:24-54` does), renamed to `gn-media-*`/`gn-video-*`.
6. `gn-animated-image.{c,h}` stay (no Gnostr equivalent; Gnostr gains GIF animation).

### 4.2 Thin Gnostr adapters (keep the old API until all callers migrate, then delete)
- Keep `apps/gnostr/src/ui/gnostr-image-viewer.h` as a header-only/inline or ~80-line wrapper: `gnostr_image_viewer_new()`
  returns a `GnMediaViewer` with the Gnostr `GnMediaSource` set and `link-copied` -> toast; `set_image_url` ->
  `set_gallery({url})` + `request_load` when policy ALLOW. Then migrate callers and delete the wrapper in the same
  series:
  - `apps/gnostr/src/ui/gnostr-picture-grid.c:638`
  - `nostr-gtk/src/nostr-note-card-row.c:3521-3530,3568-3589,3686` and `:259-266,405,525-528`
  - `nostr-gtk/src/gnostr-profile-pane.c:710-712,3651-3653,3687-3693`
  The two nostr-gtk files are coupled; they get the source via an injected factory/`GnMediaSource` on the timeline
  view/profile pane (property), which removes two Gnostr symbol imports from `NOSTR_GTK_GNOSTR_COUPLED_SOURCES`.
- New `apps/gnostr/src/services/gnostr-media-source.c`: implements `GnMediaSource` over `GnostrMediaService` / shared
  soup session, policy from `gnostr_is_remote_media_allowed()`, GSettings binding helper for `GnVideoPlayer`.
- Delete: the hidden `portable_viewer` adapter (`gnostr-image-viewer.c:62,166-167,...`), `gnostr-video-player.{c,h}`,
  `gnostr-image-viewer.{c,h}`, `GNOSTR_IMAGE_VIEWER_TEST_SKIP_FETCH`.

### 4.3 Build
- `nostr-gtk/CMakeLists.txt`: add `gn-video-player`, `gn-media-source`, `gn-media-decode` to
  `NOSTR_GTK_PORTABLE_SOURCES`/`_HEADERS` (`:155-183`) and to `NOSTR_GTK_HEADERS`; no libsoup in
  `nostr_gtk_portable` link line (`:233-239`). Add `nostr-gtk/po/POTFILES` entries.
- `nostr-gtk/meson.build:111-153`: same file lists.
- `apps/gnostr/CMakeLists.txt:666-667`: remove `gnostr-image-viewer.c`, `gnostr-video-player.c`; add
  `gnostr-media-source.c`; rework test target `:1207-1238` (no longer compiles the viewer source directly; link
  `nostr_gtk_portable`).
- `apps/gnostr/meson.build` (if it lists the sources): same.
- GIR/VAPI: new types appear in `nostr-gtk` GIR; update `test_vapi_smoke.vala` if it enumerates types.
- Groundhog CMake: no change beyond already linking `nostr_gtk_portable` (`gnome/groundhog/CMakeLists.txt:1384-1405,3455-3495`).

### 4.4 Groundhog files switching to the ported API
- `gh-message-row.c:535-551` (linked image viewer): unchanged calls; optionally `set_source` with a Groundhog
  `GhMediaSource` that wraps `gh_conversation_view` consent + `GhNet` so the viewer's Load button works for unloaded
  slots; `can-save` routed to the existing portal save.
- `gh-attachment-card.c:420-491`: unchanged calls; set `show-copy-link` off for `attachment:` slots; connect
  `save-requested` to the attachment provider's save (`gh-attachment-card.h:30`).
- `gh-web-content.c:139-159`: switch its decode to `gn_media_decode_*` with Groundhog limits (PNG/JPEG/GIF, no
  fallback), removing the duplicate probe/animation glue; keep `gh-metadata-strip` for upload stripping.
- New `gnome/groundhog/src/ui/gh-media-source.{c,h}` (only if Load-in-viewer is wanted in this step).

## 5. Tests

Keep / move:
- `apps/gnostr/tests/test_image_viewer_remote_media.c` -> `nostr-gtk/tests/test_media_viewer.c` re-expressed against a fake
  `GnMediaSource`: blocked state with policy ASK, Load visible and triggers exactly one fetch, ALLOW auto-fetches once.
- `nostr-gtk/tests/test_portable.c:196-262,338-380` (gallery/no-fetch/generation/GIF bounds): keep.
- Groundhog `test_attachment_ui.c`, `test_conversation_view.c:1712-1777`, `check_privacy.py`: keep unchanged; they are the
  acceptance gate for Groundhog.
Add:
- Late result after navigate is dropped (generation) and its `GCancellable` was cancelled; close cancels.
- Decode limits: oversize bytes, >max_dimension PNG/JPEG header rejected *before* decode, WebP rejected without
  fallback flag, GIF frame/byte caps; malformed headers fuzz (reuse GIF test fixtures).
- Zoom: fit never upscales; additive step from fit; pinch clamp; `1` = 100%; arrows pan when zoomed.
- Keyboard + a11y: every button has an accessible label; Esc closes; space toggles video.
- Video: `GnVideoPlayer` creates no `GtkMediaFile` until play/stream set; pauses when unmapped/scrolled out; autoplay
  property honoured; no remote `GFile` created from `load-requested` path (assert via fake).
- Portable lint (new `scripts/check-portable-media.py` or extend `nostr-gtk` standalone test): no `soup_`, `g_settings_new`,
  `gnostr_`, `gdk_pixbuf_` without the fallback guard, `g_file_new_for_uri` in `nostr-gtk/src/gn-*.c`; link
  `test_standalone_core`-style check that `nostr_gtk_portable` has no Gnostr symbols (exists; extend to new files).
- Build at floor: CI job compiling `nostr_gtk_portable` against GTK 4.6/adw 1.2 headers (MAX_ALLOWED already enforces
  deprecation/availability warnings; make them errors for these files).
- Perf: run `test_bind_latency_budget`, `test_listview_recycle_stress`, `test_widget_churn_leaks` with the inline video
  widget in cards.

## 6. Risks and ordering

Ordering (each step lands green on both apps):
1. Add `gn-media-decode` + `gn-media-source` + tests (no behaviour change).
2. Port the Gnostr viewer body into `gn-media-viewer.c` (git mv + graft), API-compatible superset of the alpha-6 header.
   Run Groundhog UI tests and `test_portable` first — Groundhog is the current consumer.
3. Gnostr adapter `gnostr-media-source.c`; make `gnostr_image_viewer_*` a thin wrapper; switch
   `gnostr-picture-grid.c`; delete hidden `portable_viewer`.
4. Port `GnVideoPlayer`; viewer uses its controls; Gnostr wrapper for `gnostr_video_player_*` with GSettings binding.
5. Migrate coupled nostr-gtk callers (`nostr-note-card-row.c`, `gnostr-profile-pane.c`) to injected source; delete wrappers
   and the Gnostr files; move CSS.
6. Groundhog: decode helper in `gh-web-content.c`, save/copy wiring, optional viewer Load via `GhNet`.

Risks:
- **GTK API floor**: content-fit (4.8) and GtkFileDialog (4.10) must be replaced; visual fit behaviour may change slightly
  (`keep_aspect_ratio` + size requests). Screenshot-compare Gnostr before/after.
- **Gnostr video network path**: keeping `g_file_new_for_uri(remote)` in the Gnostr adapter preserves behaviour but keeps
  backend-side HTTP outside any proxy; switching to fetched bytes changes streaming (whole file in memory). Needs an owner
  decision; do not let it into the portable API either way.
- **Format regression for Gnostr**: bounded decode without pixbuf fallback drops WebP/AVIF; Gnostr must opt into the
  fallback flag explicitly.
- **Behaviour change for Gnostr auto-load**: today `navigate` fetches immediately when allowed; preserved via policy ALLOW,
  but now cancellable — verify no flicker.
- **ABI**: `gnostr_image_viewer_*`/`gnostr_video_player_*` are not exported from nostr-gtk (they live in the app), so removal
  is app-internal; new portable symbols add to `nostr-gtk-portable-1.0` ABI (minor bump in pkg-config/soname notes).
- **Privacy lint blind spot**: `check_privacy.py` doesn't see nostr-gtk; until the portable lint exists, reviewers must
  check every new `gdk_texture_new_from_bytes`/fetch site manually.
- **Dual-window regressions**: viewer is modal/undecorated; GtkFileChooserNative from a modal undecorated transient
  window — test on Wayland and macOS.
- **Large diff in one file**: do step 2 as git mv first commit (no edits), then edits, so blame/history survive review.
