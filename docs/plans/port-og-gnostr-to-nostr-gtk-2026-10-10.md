# Port plan: OG link preview (gnostr) -> nostr-gtk, shared by gnostr and Groundhog

Date: 2026-10-10. Read-only investigation; nothing changed but this file.

## 0. Current state (the actual shape of the duplication)

- `apps/gnostr/src/ui/og-preview-widget.c` (659 lines) is the widget gnostr really renders. It is
  consumed from **nostr-gtk's own coupled source** `nostr-gtk/src/nostr-note-card-row.c`
  (:5 include, :386 prepare_for_unbind, :3855/:4383/:4774/:5212 `og_preview_widget_new` +
  `set_url_with_cancellable`). That is why note-card-row sits in
  `NOSTR_GTK_GNOSTR_COUPLED_SOURCES` (`nostr-gtk/CMakeLists.txt:128-133`).
- `nostr-gtk/src/gn-og-preview-card.c` (188 lines) is the alpha-6 "portable" card. gnostr's widget
  creates one as a **hidden shadow** (`og-preview-widget.c:442-445`, `g_object_ref_sink`, never
  parented). It mirrors state into it (`:122`, `:145`, `:230`, `:555`, `:582`, `:624`, `:651`) and
  routes the load through its `load-requested` signal (`:325-342`). The user never sees it.
  This is adapter theatre, not a port.
- Groundhog renders the card for real (`gnome/groundhog/src/ui/gh-message-row.c:718-742`, `:650`
  image request), fed by its own fetch/parse stack: `gnome/groundhog/src/ui/gh-web-content.c`
  (libxml2 `<head>` parser `:58-128`, 256 KiB prefix fetch `:183-200`) over
  `gh_net_http_get_public_prefix_async` (`gnome/groundhog/src/net/gh-net-http.h:92`).
- gnostr's fetch/parse/cache lives in `apps/gnostr/src/services/gnostr-media-service.c`:
  `GnostrOgMetadata` (:278-320), memory LRU+TTL og cache (:906-980; 256 entries, 30 min, :266-267),
  disk og cache (:449 `og-metadata/` v1), 2 MiB body cap (:42, :1777-1785), strstr-based parser
  `html_attr_value`/`extract_meta_content`/`extract_html_title` (:1582-1660) run in a thread
  (`parse_og_worker` :1672-1728), image via `request_texture_with_intent` (header :186) which
  decodes with **gdk-pixbuf** (:1359-1431, :2051).

So there are two parsers, two fetchers, two widget skins, one of which is invisible.

## 1. Feature / behaviour diff

### gnostr original has, GnOgPreviewCard lacks
| Feature | gnostr ref | Card status |
|---|---|---|
| Horizontal media-card layout: 120px cover image left, title(1 line)/description(2)/site(1) right, CSS names `og-preview`, `og-preview-card/-image/-title/-description/-site` | og-preview-widget.c:422-531 | Vertical box, CONTAIN image under text (gn-og-preview-card.c:62-93); different CSS (`gn-og-preview-card`) |
| Loading spinner state | :91-98, :335 | none (only status text) |
| Distinct error state label "Preview Not Available" | :100-107, :491-495 | status label, OK |
| Custom `measure` (height bounded for list rows; nostrc-14wu, referenced nostr-note-card-row.c:1982) | :395-420 | none -> row height jitter risk |
| Domain fallback for title when og:title missing, `extract_domain` | :80-89, :150-153 | shows raw URL in site, no title fallback |
| UTF-8 sanitisation of all labels (`gnostr_sanitize_utf8`) | :154-157 | none (Groundhog sanitises upstream in `bounded_text`, gh-web-content.c:40-45; gnostr path does not) |
| Click card -> open URL via GtkUriLauncher | :288-322 | none (Groundhog has its own link handling) |
| YouTube play overlay + optional WebKit embed (`HAVE_WEBKITGTK`) | :184-201, :303-317 | none |
| External parent cancellable chaining (row unbind cancels fetch) | :239-285, :587-605 | single internal cancellable only, no parent |
| `prepare_for_unbind` (GtkListView recycling, label-guard against finalized labels) | :640-659 | `clear()` only |
| Stale-callback guard via weak context + `request_generation` | :48-75, :216-219 | `_for_url` variants (OK, equivalent) |
| Same-URL early-return on rebind | :548-549 | set_url always clears (Groundhog compares URL itself, gh-message-row.c:725) |
| Host-naming tooltip "reveals your network address" | :557-561 | none (Groundhog names host in its AdwAlertDialog instead) |
| Auto-load when `gnostr_is_remote_media_allowed()` | :571-572 | intentionally absent |
| Image auto-fetched once metadata arrives (`GNOSTR_MEDIA_FETCH_USER_INITIATED`) | :172-182 | separate explicit `image-load-requested` |

### Card does better (must be kept)
- **No I/O in construction/set_url**; fetch only via signal (gn-og-preview-card.h comment; .c:122-133). Pure view.
- **Image is a separate consent step** (`load_image` button, tooltip "Nothing is loaded until you ask", .c:73-76, :167-177). Required by charter D13/§2.1.
- **i18n**: `_()` through the `nostr-gtk` gettext domain (`gn_portable_gettext_domain`, .c:2, :54). gnostr strings are untranslated literals ("Load Preview", "Preview Not Available", tooltip printf, :475-493, :557).
- **Hidden empty labels** (nostrc-p15n5.7, .c:14-17).
- **Same image across rebind keeps texture** (.c:150-157).
- Stable widget names (`og_title`, `og_load`, ...) used by tests (nostr-gtk/tests/test_portable.c:215-224, groundhog tests/ui/test_conversation_view.c).
- GTK 4.6 floor: no GtkUriLauncher (4.10) — note gnostr's `gtk_uri_launcher_*` (:320) is **above the floor** and cannot move as-is.

### Groundhog parser does better than gnostr's (adopt as the shared parser)
- Real HTML parse, `<head>` only, `HTML_PARSE_NONET`, no entity/script/resource loading (gh-web-content.c:66-73).
- Field precedence og > twitter > plain independent of document order (`take_field`, :47-56); gnostr's is first-match strstr over the whole body (:1619-1660), so a `<meta>` in `<body>` or in a comment/script string can win.
- 512-char bounding + `g_utf8_make_valid` per field (:40-45).
- og:image accepted only through link policy (`gh_link_policy_can_preview`, :88-93) - this check is Groundhog policy and must become an injected predicate.
- 256 KiB prefix read, truncation not an error (:191-198) vs gnostr 2 MiB hard cap that errors.
- gnostr has twitter:image fallback (:1701-1703) which Groundhog lacks - keep it in the shared parser (still subject to the policy predicate).

## 2. gnostr internal dependencies -> injected interfaces

| Dependency in og-preview-widget.c | Replacement |
|---|---|
| `gnostr_media_service_get_default()` + `request_og_metadata_with_intent` (:337) | `GnOgPreviewProvider` interface (below) |
| `gnostr_media_service_request_texture_with_intent(... OG_IMAGE, 240,160 ...)` (:172-182) | same interface, `load_image_async` |
| `GnostrOgMetadata` getters (:139-142) | new boxed `GnOgMetadata` in nostr-gtk; gnostr's type becomes a typedef/alias or is converted at the adapter |
| `gnostr_is_remote_media_allowed()` (utils.c:312) / GSettings | **not** moved; app decides. Provide `gn_og_preview_card_set_auto_load(card, gboolean)` default FALSE. gnostr sets it from its setting; Groundhog never sets it |
| `gnostr_sanitize_utf8` (utils.h) | inline `g_utf8_make_valid` + bound in the card (GLib only) |
| `gnostr_youtube_url_is_youtube`/`extract_video_id` (util/youtube_url.h), `gnostr_youtube_embed_new` (WebKit) | `GnOgPreviewCard::activate` signal (url) with default handler that does nothing; gnostr connects and does YouTube/UriLauncher. Play overlay via `gn_og_preview_card_set_media_badge(card, const char *icon_name)` |
| `GNOSTR_LABEL_SAFE` (gnostr-label-guard.h) | not needed once the card owns its children and unbind = `clear()`; if kept, copy the macro into a private nostr-gtk header |
| `gnostr_get_shared_soup_session()` (media-service.c:24) | stays in gnostr adapter; nostr-gtk never touches libsoup |
| gdk-pixbuf scaling/decode (media-service.c:1359-1431) | stays behind gnostr's provider; card receives a `GdkPaintable` |

Proposed public API (`nostr-gtk/include/nostr-gtk-1.0/gn-og-preview.h`, portable target):

```c
typedef struct _GnOgMetadata GnOgMetadata;           /* boxed, refcounted */
GType        gn_og_metadata_get_type(void);
GnOgMetadata *gn_og_metadata_new(const char *source_url, const char *title,
                                 const char *description, const char *site_name,
                                 const char *image_url);
GnOgMetadata *gn_og_metadata_ref(GnOgMetadata *m);
void          gn_og_metadata_unref(GnOgMetadata *m);
const char   *gn_og_metadata_get_title/_description/_site_name/_image_url/_source_url(const GnOgMetadata *m);

/* Pure, GLib+libxml2, no I/O. accept_image may be NULL (accept all http(s)). */
typedef gboolean (*GnOgImagePolicy)(const char *image_url, gpointer user_data);
GnOgMetadata *gn_og_metadata_parse_html(GBytes *head_prefix, const char *source_url,
                                        GnOgImagePolicy accept_image, gpointer user_data,
                                        GError **error);

#define GN_TYPE_OG_PREVIEW_PROVIDER (gn_og_preview_provider_get_type())
G_DECLARE_INTERFACE(GnOgPreviewProvider, gn_og_preview_provider, GN, OG_PREVIEW_PROVIDER, GObject)
struct _GnOgPreviewProviderInterface {
  GTypeInterface parent;
  void          (*load_metadata_async)(GnOgPreviewProvider *self, const char *url,
                                       GCancellable *c, GAsyncReadyCallback cb, gpointer data);
  GnOgMetadata *(*load_metadata_finish)(GnOgPreviewProvider *self, GAsyncResult *r, GError **e);
  void          (*load_image_async)(GnOgPreviewProvider *self, const char *image_url,
                                    int width_hint, int height_hint,
                                    GCancellable *c, GAsyncReadyCallback cb, gpointer data);
  GdkPaintable *(*load_image_finish)(GnOgPreviewProvider *self, GAsyncResult *r, GError **e);
};
```

Card additions (`gn-og-preview-card.h`):
```c
void gn_og_preview_card_set_provider(GnOgPreviewCard *, GnOgPreviewProvider *); /* weak-free; may be NULL */
void gn_og_preview_card_set_url_with_cancellable(GnOgPreviewCard *, const char *url, GCancellable *parent);
void gn_og_preview_card_set_auto_load(GnOgPreviewCard *, gboolean);   /* default FALSE */
void gn_og_preview_card_set_auto_load_image(GnOgPreviewCard *, gboolean); /* default FALSE */
void gn_og_preview_card_set_media_badge(GnOgPreviewCard *, const char *icon_name);
void gn_og_preview_card_prepare_for_unbind(GnOgPreviewCard *);
/* signals: load-requested, image-load-requested (kept), activate(url) (new) */
```
Rule: with a provider set, the card drives the provider itself on `request_load`/`request_image`
(no signal handler needed); without one, it only emits the existing signals (Groundhog's current
contract stays valid, so step ordering below is safe). Fetch never starts from set_url unless
`auto_load` is TRUE — and only gnostr ever sets it.

Caching: gnostr's LRU/TTL/disk og cache stays inside gnostr's provider (it is per-npub
namespaced, media-service.c:347-369, app policy). nostr-gtk ships no cache. Groundhog's
conversation-view already memoises results per message.

## 3. Groundhog requirements to preserve

- Charter `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`: §2 item 2 (line 60) nothing
  fetched by default; D13 (line 104) per-message "Show Preview" tap, off by default; PD-2 (line 194);
  §2.1 (line 209) consent `AdwAlertDialog` naming host + network mode, switch-off revokes and cancels.
  -> card never auto-loads by default; consent stays in Groundhog (gh-conversation-view.c, before
  `gh_web_content_load_async` :2090). The provider is invoked only after consent.
- Network mode / Tor: all fetches go through `GhNetHttp` (gh-net-http.h:9 charter §2.1/§4.2,
  prefix API :92/:97, SOCKS tests `tests/net/socks5-fixture.c`). Groundhog's provider implementation
  = thin GObject wrapping `GhWebContent`; nostr-gtk must not link libsoup or open sockets.
- gdk-pixbuf rule — **verified, with a caveat**: charter §2.1 line 212 says images are PNG/JPEG by
  magic, ≤2 MiB, decoded with `gdk_texture_new_from_bytes` "not gdk-pixbuf". The code has drifted:
  6 MiB limit and any format GTK loads (gh-web-content.c:137-140, :188-190), and on GTK < 4.? the
  non-PNG/JPEG/TIFF path of `gdk_texture_new_from_bytes` itself goes through gdk-pixbuf (packaging
  already pulls `pkgconfig(gdk-pixbuf-2.0)` + webp-pixbuf-loader, packaging/rpm/groundhog.spec:50,66).
  GIF uses `GnAnimatedImage` "never gdk-pixbuf" (gh-attachment-card.c:329). So: the shared card
  must take a `GdkPaintable`/`GdkTexture` and never decode itself; gnostr's pixbuf scaler stays in
  gnostr. File a separate bead for the charter/code drift (2 MiB vs 6 MiB, format list).
- Parser bounds: 256 KiB prefix, `<head>` only, NONET, 512-char fields, og:image through
  `gh_link_policy_can_preview` (now passed as `GnOgImagePolicy`).
- Image dimension cap 4096 before/after decode (gh-web-content.c:141-153) stays in Groundhog provider.
- Render/perf: preview row is inside GtkListView rows; keep "same image keeps texture"
  (gn-og-preview-card.c:150-157) and the stable widget names used by screenshot tests
  (`test_conversation_view.c` g12 screenshots, line 16). New horizontal layout will change
  Groundhog screenshots -> make layout a property (`GN_OG_PREVIEW_LAYOUT_COMPACT|STACKED`) and
  keep STACKED as Groundhog default, or rebaseline deliberately.
- Tests that exist: `gnome/groundhog/tests/ui/test_conversation_view.c` (counting fake fetcher
  :370+, "previews behind consent and none without a fetcher"), `tests/privacy/test_privacy_e2e.c`
  + `canary-scan.c` (no unexpected network), `tests/net/*`. No unit test of
  `gh_web_result_parse_html` was found — add one when it moves.

## 4. File moves / build changes

New in nostr-gtk (portable/core, GLib/GTK 4.6/libadwaita 1.2 only):
1. `nostr-gtk/src/gn-og-metadata.c` + `include/nostr-gtk-1.0/gn-og-metadata.h` — boxed type +
   `gn_og_metadata_parse_html`: **moved from** `gnome/groundhog/src/ui/gh-web-content.c:37-128`
   (`bounded_text`, `take_field`, `gh_web_result_parse_html`), plus gnostr's twitter:image
   fallback from media-service.c:1701-1703. Adds `libxml-2.0` to `nostr_gtk_portable` PRIVATE deps
   and to `Requires.private` of `nostr-gtk-portable-1.0.pc` (CMakeLists.txt:441-465).
2. `nostr-gtk/src/gn-og-preview-provider.c` + header — GInterface above.
3. `nostr-gtk/src/gn-og-preview-card.c` — rewritten by **moving the body of
   apps/gnostr/src/ui/og-preview-widget.c into it**: spinner, compact layout, measure (:395-420),
   cancellable chaining (:239-285), weak request context (:48-75), unbind (:640-659), domain
   fallback; minus YouTube/UriLauncher (-> `activate` signal), minus media-service calls
   (-> provider), with card's existing consent-gated image button, i18n `_()` and hidden-empty-label
   behaviour. Add `.ui`-less CSS names for both skins.
4. Add sources/headers to `NOSTR_GTK_PORTABLE_SOURCES`/headers lists (CMakeLists.txt:156-182);
   G-IR annotations if the portable lib is introspected (:335-355).
5. Translations: new strings into nostr-gtk `po/POTFILES` for the `nostr-gtk` domain (:185-190).

gnostr:
6. New `apps/gnostr/src/services/gnostr-og-provider.c/.h`: `GnostrMediaService` implements (or a
   small wrapper object implements) `GnOgPreviewProvider`; converts `GnostrOgMetadata` -> `GnOgMetadata`
   (or make `GnostrOgMetadata` a typedef of `GnOgMetadata` and delete its getters :278-320).
7. Replace `parse_og_worker`'s strstr parser (media-service.c:1582-1728) with a call to
   `gn_og_metadata_parse_html`; keep the thread, caches, body cap (consider lowering to 256 KiB
   prefix semantics).
8. `nostr-gtk/src/nostr-note-card-row.c` (:5, :386, :3855-3858, :4383-4388, :4774-4777, :5212):
   switch to `gn_og_preview_card_new()` + `set_provider(gnostr_og_provider_get_default())` +
   `set_auto_load(gnostr_is_remote_media_allowed())` + `set_url_with_cancellable` +
   `prepare_for_unbind`; connect `activate` to a gnostr handler holding the YouTube/UriLauncher code
   (moved to `apps/gnostr/src/ui/gnostr-og-activate.c`). Provider/auto-load injection should come
   through the existing note-card binding ctx (`note-card-binding-ctx.c`) so that this does not add
   another gnostr symbol to the coupled row.
9. **Delete** `apps/gnostr/src/ui/og-preview-widget.c/.h`; remove from `apps/gnostr/CMakeLists.txt`
   (it is listed there and in `gnostr-widget-test-support`) and from any `meson.build` entry
   (charter line 668 lists `src/ui/og-preview-widget.c` as a reference - update text).
   Remove CSS rules for `og-preview*` only if the card adopts its own names; otherwise card uses them.

Groundhog:
10. `gnome/groundhog/src/ui/gh-web-content.c`: delete the parser (:37-128), call
    `gn_og_metadata_parse_html(bytes, uri, gh_link_policy_can_preview_cb, NULL, error)`;
    `GhWebResult` text fields become (or wrap) a `GnOgMetadata`.
11. New `gnome/groundhog/src/ui/gh-og-provider.c`: implements `GnOgPreviewProvider` over
    `GhWebContent` (GH_WEB_PREVIEW / GH_WEB_PICTURE). Optional: keep Groundhog on signals and
    skip provider until step 12.
12. `gh-message-row.c:650-742` and `gh-conversation-view.c:2090,:2236`: either keep the current
    signal flow (zero behaviour change) or set provider after consent. Consent dialog code stays.
13. `gnome/groundhog/meson.build`/CMake: add libxml2 no-op (already linked); nothing else.

## 5. Tests

Keep: `nostr-gtk/tests/test_portable.c:215-224` (extend), `test_portable_i18n.c` (add new strings),
Groundhog `test_conversation_view.c` (consent, counting fetcher, screenshots), privacy e2e/canary.
Move/add:
- `nostr-gtk/tests/test_og_metadata.c` (new, no display): precedence og>twitter>plain irrespective
  of order; `<meta>` in body ignored; truncated head; invalid UTF-8; 512-char bound; image policy
  rejection; twitter:image fallback; no title/desc -> G_IO_ERROR_INVALID_DATA; NONET (entity /
  external DTD not fetched). Port cases from any gnostr parser tests (grep apps/gnostr/tests for og).
- `nostr-gtk/tests/test_og_preview_card.c` (xvfb, `gn_add_gtest_xvfb` :506): fake provider counts
  calls -> zero calls after new/set_url with auto_load FALSE; one metadata call on request_load;
  image never requested until request_image unless auto_load_image; stale callback after set_url to
  another URL ignored; parent cancellable cancels in-flight; prepare_for_unbind cancels; measure
  bounds height; hidden empty labels.
- Coupled-source guard: after step 8 the row no longer includes `og-preview-widget.h`; add a
  configure-time grep/compile check that `gn-og-*.c` include no `apps/gnostr` header (already enforced
  for core by include-dir properties :295-296 — ensure new files are in the core list).
- gnostr: provider adapter test with the media service in a temp cache dir (metadata cache hit,
  disk round-trip).

## 6. Risks and ordering

Order (each step builds and passes on its own):
1. Add `GnOgMetadata` + parser to nostr-gtk (moved from Groundhog) + tests. Groundhog switches to it
   (step 10). No UI change.
2. Add provider interface + new card capabilities, keeping existing signals/API backward compatible
   (Groundhog untouched beyond rebuild; screenshots unchanged with STACKED default).
3. gnostr provider adapter; switch gnostr parser to shared parser.
4. Switch nostr-note-card-row to the card; delete og-preview-widget.{c,h} and the shadow card.
5. Optionally move Groundhog to provider flow; file bead for charter image-size/format drift.

Risks:
- **Behaviour change in gnostr**: auto-load is gnostr's default today (:568-572); preserve exactly via
  `set_auto_load`, and keep image auto-fetch after metadata for gnostr (`auto_load_image`), or users
  see an extra "Load image" step. Decide explicitly with owner.
- GTK floor: `GtkUriLauncher` (4.10) and `GtkSpinner` deprecation (4.20) — launcher stays in gnostr;
  spinner OK at 4.6.
- libxml2 becomes a dependency of the portable library (packaging: .pc, flatpak, debian/rpm).
  Alternative: keep parser GLib-only — rejected, the strstr parser is the weaker one.
- Row recycling regressions (nostrc-14wu measure, label-guard crashes): port measure + unbind
  verbatim, run gnostr scroll/recycle tests before deleting the old widget.
- ABI: nostr-gtk 1.x installs gn-og-preview-card.h; only add API, do not change signal signatures.
- Groundhog screenshot/test churn if layout default changes; gate behind layout property.
- Cache semantics: gnostr disk cache is keyed on its own metadata version (OG_METADATA_VERSION :51);
  if the stored struct changes, bump version.
