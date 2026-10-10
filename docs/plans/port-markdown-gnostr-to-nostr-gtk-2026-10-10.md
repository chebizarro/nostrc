# Port plan: Markdown rendering — gnostr `markdown_pango` → nostr-gtk (2026-10-10)

Scope: `apps/gnostr/src/util/markdown_pango.c` (657 lines) vs `nostr-gtk/src/gn-markdown.c` (393, parser)
plus `gnome/groundhog/src/ui/gh-link-policy.c:378-606` (Pango formatter). Read-only planning; no code changed.

## 0. Key finding (changes the shape of the port)

The situation is not "gnostr has a full implementation, nostr-gtk a thin copy". It is split by layer:

| Layer | gnostr | nostr-gtk / Groundhog |
|---|---|---|
| Parser | ad-hoc byte scanner inside the emitter (`markdown_pango.c:88-378`) | `gn_markdown_parse` token model (`gn-markdown.c:340`), bounded, GFM tables/lists/tasks/strike/autolinks |
| Pango emitter | `markdown_to_pango` (`:380`), `markdown_to_pango_summary` (`:457`), `markdown_strip_to_plain` (`:597`), `markdown_extract_first_image` (`:574`) | only Groundhog's `gh_link_policy_format_markdown` (`gh-link-policy.c:546`) — app-private |

gnostr already calls the portable parser but throws the tokens away and only uses its normalised
`source` (`markdown_pango.c:385-389`), then re-parses with the legacy scanner. So there are two
parsers and two emitters. The real "port" is: **promote one Pango emitter into `nostr_gtk_portable`,
built on `GnMarkdownDocument`, carrying gnostr's public API surface (full / summary / plain /
first-image, length caps, heading sizes) and Groundhog's safety rules (literal targets, link policy,
no fetch)**, then delete `markdown_pango.c` and Groundhog's formatter body. Porting gnostr's
scanner verbatim would regress safety and features; port its *API and visual output*, not its parser.

`markdown_pango.c` has **no** gnostr singleton/service/GSettings/network dependencies (only
`markdown_pango.h`, `gn-markdown.h`, libc). The only app coupling in this area is Groundhog's
mention display-name lookup (`gh_display_name_for`, `gnome/groundhog/src/app/gh-display-name.h:18`,
used by `gh_link_policy_to_mention_markup` `gh-link-policy.c:346-376`) and the link classifier.

## 1. Feature / behaviour diff

### gnostr original has, reimplementation lacks (must be carried over)
- Heading sizes: `xx-large`/`x-large`/`large`/`medium` + bold (`markdown_pango.c:319-331`); Groundhog uses plain `<b>` (`gh-link-policy.c:560-563`). Needed for article reader/wiki.
- Blockquote styling `<span alpha="80%" style="italic">` (`:342`); Groundhog uses `│ ` prefix (`gh-link-policy.c:578-581`).
- HR as `<span alpha="50%">---</span>` (`:305`); Groundhog `────────` (`:582`).
- Ordered list keeps source numeral, indented "  N. " / "  • " (`:353,368`).
- Fenced code block body as one `<tt>` run (`:274-291`); portable parser emits per-line CODE tokens (`gn-markdown.c:366`) – emitter must merge consecutive CODE+LINE_BREAK into one `<tt>` block.
- `max_length` output cap with "..." (`:423-427`) — API needed by callers passing 0 today; keep the parameter but fix semantics (see bugs).
- Summary mode: collapse whitespace, drop headings/code fences, keep bold/italic, link text only, char-count cap with "…" (`:457-572`). Callers: `gnostr-article-card.c:669`, `gnostr-wiki-card.c:827`, `nostr-gtk/src/nostr-note-card-row.c:6649`.
- `markdown_strip_to_plain` (`:597`) and `markdown_extract_first_image` (`:574`) — exported but **no callers** found (grep over apps/, nostr-gtk/); `gnostr-classified-card.c:10` includes the header without calling it. Port `extract_first_image` as a token query (cheap, useful for article hero image, gated by caller consent), drop `strip_to_plain` or re-implement as token text concat for a11y labels.
- Link label shown with target `label (url)` — same as Groundhog. Groundhog additionally linkifies the target via policy.

### Reimplementation has, gnostr lacks (must be kept)
- Bounded parsing: 64 KiB input, 4096 tokens, depth 8, 32 cols, indent 8 (`gn-markdown.h` MAX_* ; `gn-markdown.c:25-28,55,77,313-316`). gnostr's `find_closing`/`strchr`/`strstr` scan unbounded and recurse without depth limit (`markdown_pango.c:57,104-147,166,488,507`) — quadratic and stack-depth risk on hostile notes; and when the portable doc is `truncated` gnostr falls back to the full unbounded string (`:388`).
- Truncation honesty: Groundhog shows the full escaped body when `truncated` (`gh-link-policy.c:598-605`, `gh-conversation-view.c:1343-1349`).
- GFM: tables rendered as aligned monospace grid with wide-char column math (`gh-link-policy.c:441-544`), task lists ☐/☑, nested bullets ◦, `~~strike~~` → `<s>`, `<autolink>`, `\` escapes only for punctuation, `snake_case` not italic (`gn-markdown.c:51-64,82-83`), `~~~` fences, CRLF.
- Real links: raw http(s)/nostr URIs become `<a href>` only after `gh_link_policy_find_links` acceptance; labels never become links; `javascript:`/`data:`/nsec stay text (`gh-link-policy.h:9-23,52-55`). gnostr emits no `<a>` at all.
- Mentions: valid `nostr:npub` → `@displayname`, own account bold (`gh-link-policy.c:346-376`).
- Images never fetched; IMAGE token shows alt + literal address (`gn-markdown.h:20`, `gh-link-policy.c:414-423`).
- Off-UI-thread parse: document parsed in a `GTask` worker, formatted on UI thread, LRU render cache with byte budget (`gh-conversation-view.c:1300-1395`).
- Escaping via `g_markup_escape_text` on whole valid UTF-8 strings (no per-byte escaping).

### Bugs in gnostr original that the port must NOT carry
- `markdown_to_pango` truncates *markup* bytes (`:424-426`): can cut inside a tag, entity or UTF-8 sequence → invalid Pango markup / dropped label. Cap must apply to source/visible text, then close open tags.
- Summary opens `<i>`/`<b>` on any unmatched `*`/`_` (`:503-529`) — "2 * 3" italicises the rest; `#hashtag` mid-text is stripped as heading (`:496`).
- `strip_to_plain` byte cap splits UTF-8 (`:647`), no UTF-8 validation.
- nostr: entity scan accepts any alnum (`:234`); no nsec exclusion (harmless since not linked, but don't regress).

### a11y / i18n
- Neither emitter has translatable strings except none; "[Image]" fallback (`markdown_pango.c:205`) is untranslated — route through `gn-portable-i18n.h` (`_()` in the `nostr-gtk` domain) and add to `nostr-gtk/po/POTFILES`.
- Decorative glyphs (•, ◦, ☐, ☑, │, ─) are read by screen readers; provide a plain-text accessible description variant (token text join) for `GTK_ACCESSIBLE_PROPERTY_DESCRIPTION` — this is the legitimate replacement for `strip_to_plain`.

## 2. Dependencies to replace with injected interfaces

gnostr side: none (pure function). Groundhog side: mention names and link acceptance. New public header
`nostr-gtk/include/nostr-gtk-1.0/gn-markdown-pango.h` (GTK-free, GLib only):

```c
typedef enum { GN_MARKDOWN_PANGO_FULL, GN_MARKDOWN_PANGO_SUMMARY } GnMarkdownPangoMode;
typedef enum {
  GN_MARKDOWN_PANGO_HEADING_SIZES = 1 << 0, /* gnostr article look; off = <b> */
  GN_MARKDOWN_PANGO_GLYPH_BLOCKS  = 1 << 1, /* │ quote, ──── rule (Groundhog look) */
  GN_MARKDOWN_PANGO_TABLE_GRID    = 1 << 2, /* monospace grid; off = cells " | " */
} GnMarkdownPangoFlags;

/* Called for each candidate raw address (RAW_URL, NOSTR_REFERENCE, link/image target).
 * Append markup for @text to @out and return TRUE, or FALSE to have it escaped literally.
 * Must not perform I/O. Default (NULL) = everything literal (no <a>). */
typedef gboolean (*GnMarkdownLinkFormatFunc)(GString *out, const gchar *text,
                                             gpointer user_data);

typedef struct {
  GnMarkdownPangoMode mode;
  guint flags;
  gsize max_chars;               /* visible chars; 0 = unlimited; ellipsis "…" */
  GnMarkdownLinkFormatFunc format_text; /* nullable */
  gpointer user_data;
} GnMarkdownPangoOptions;

gchar *gn_markdown_pango_format(const GnMarkdownDocument *doc,
                                const GnMarkdownPangoOptions *options); /* UI or worker thread */
gchar *gn_markdown_pango_from_text(const gchar *text, const GnMarkdownPangoOptions *options);
      /* truncated doc → full escaped text, matching gh-link-policy.c:598-605 */
gchar *gn_markdown_plain_text(const GnMarkdownDocument *doc, gsize max_chars); /* a11y */
gchar *gn_markdown_dup_first_image_target(const GnMarkdownDocument *doc);     /* never fetches */
```

Groundhog supplies `format_text` = a thin adapter calling `gh_link_policy_to_mention_markup(text, account)`
(keeps `gh_display_name_for` and policy in the app). gnostr supplies NULL (today's no-`<a>` behaviour) or
later its own linkifier. No GSettings/singletons/network enter nostr-gtk.

## 3. Groundhog requirements to preserve

- Charter PD-2 / §2 rule 2 / D13: nothing fetched by default; images and previews only on per-item tap
  (`docs/designs/groundhog-privacy-ux-charter-2026-09-28.md:60,104,194,209`). Formatter must stay
  I/O-free; `dup_first_image_target` returns a string only, consent stays in caller.
- A9: size bounds before parsing, no markup injection (`charter:147`) — keep MAX_* bounds and the
  truncated → escaped fallback.
- gdk-pixbuf: verified — charter §include-boundaries "no `gdk-pixbuf`" (`charter:1274`) and image decode via
  `gdk_texture_new_from_bytes` (`charter:212`). The formatter touches no images; new file must not include
  `gdk-pixbuf`/libsoup (enforced for Groundhog `src/` by `gnome/groundhog/tests/check_privacy.py`; nostr-gtk
  portable already links only GTK4/Adw/GLib).
- Network mode / Tor (GhNet): not touched by markdown; preview URI selection `gh_link_policy_dup_preview_uri`
  stays in Groundhog.
- Render cache/perf: parse in worker, format on UI thread, account-keyed LRU byte budget, eager threshold
  (`gh-conversation-view.c:1300-1395`). Keep `gn_markdown_pango_format` taking a pre-parsed document so this
  split survives; `format_text` adapter must be thread-agnostic (it is called on UI thread today).
- GTK 4.6 / Adw 1.2 floor: pure GLib, no issue. `<s>` is Pango ≥1.x, fine.
- Existing tests: `gnome/groundhog/tests/ui/test_link_policy.c` (`/groundhog/link-policy/markdown-safe-links`,
  `markdown-table`, `markdown-gfm-blocks`, `markdown-bounded-invalid-utf8`, lines 267-383).

## 4. File moves / build changes

1. Add `nostr-gtk/src/gn-markdown-pango.c` + `include/nostr-gtk-1.0/gn-markdown-pango.h`. Content =
   Groundhog formatter (`gh-link-policy.c:378-606`: `append_markdown_inline`, `format_inline_token`,
   `append_block_inline`, `markup_columns`, `format_table`, main loop) with `account_pubkey`/mention
   calls replaced by `format_text`, plus gnostr presentation as flags, summary mode reimplemented over
   tokens (skip CODE/SEPARATOR/table, HEADING text inline, collapse LINE_BREAK to space, char cap with
   tag-safe ellipsis), merged code-fence `<tt>` runs.
2. `nostr-gtk/CMakeLists.txt`: add source to `NOSTR_GTK_PORTABLE_SOURCES` (~l.174), header to
   `NOSTR_GTK_PORTABLE_HEADERS` (~l.163) and `NOSTR_GTK_HEADERS` (~l.154). `nostr-gtk/meson.build:114,125,147`
   same three lists. `include/nostr-gtk-1.0/nostr-gtk.h:21` add include. POTFILES if "[Image]" string kept.
3. Groundhog: `gh-link-policy.c` delete l.378-606 bodies; keep `gh_link_policy_format_markdown` /
   `_to_markdown_markup` as 5-line wrappers (or switch callers and delete them):
   `gh-conversation-view.c:1350,1388` → `gn_markdown_pango_format(doc, &opts)` with Groundhog opts
   (GLYPH_BLOCKS|TABLE_GRID, format_text = mention adapter). Update `gh-link-policy.h:52-59` comments.
   `gnome/groundhog/CMakeLists.txt` unchanged (already links nostr-gtk portable via gn-markdown).
4. gnostr: replace callers — `gnostr-article-reader.c:553`, `gnostr-article-composer.c:92`,
   `gnostr-wiki-card.c:397` → `gn_markdown_pango_from_text(text, &(…){FULL, HEADING_SIZES|TABLE_GRID})`;
   `gnostr-article-card.c:669`, `gnostr-wiki-card.c:827` → SUMMARY mode with max_chars.
   Remove dead include `gnostr-classified-card.c:10`.
5. nostr-gtk note card: `nostr-gtk/src/nostr-note-card-row.c:34,6649` switch from app header
   `markdown_pango.h` (resolved through the transitional gnostr include path,
   `nostr-gtk/CMakeLists.txt:298`) to `<nostr-gtk-1.0/gn-markdown-pango.h>` — removes one app-coupling edge.
6. Delete `apps/gnostr/src/util/markdown_pango.{c,h}`; drop from `apps/gnostr/CMakeLists.txt:671` and the
   compat test at `:1197`.

## 5. Tests

- Move/extend: `nostr-gtk/tests/test_portable.c` gains `/nostr-gtk/markdown-pango/*`: golden cases from
  `apps/gnostr/tests/test_markdown_portable_compat.c` (re-baselined: headings sized, quote span), Groundhog
  table/GFM goldens lifted from `test_link_policy.c:267-365` using a test `format_text`; summary mode
  (whitespace collapse, cap with "…", "2 * 3" not italic, `#tag` kept); max_chars never yields invalid
  markup (`pango_parse_markup` on every output, incl. fuzz loop over random byte strings and the hostile
  nesting case already at `test_portable.c:129`); truncated → escaped fallback; code fence merged `<tt>`;
  image never produces `<a>`/`<img>`; first-image target query; plain-text a11y variant.
- Keep in Groundhog: `test_link_policy.c` markdown cases (now exercising wrapper + adapter); privacy
  check (`check_privacy.py`) still green.
- Delete: `apps/gnostr/tests/test_markdown_portable_compat.c` after its cases move.
- Perf: reuse existing `test_nostr_gtk_bind_latency_perf` (`nostr-gtk/CMakeLists.txt:550`); add a
  64 KiB worst-case format timing assertion.

## 6. Risks & ordering

1. Land `gn-markdown-pango` + tests in nostr-gtk (no callers) — zero risk.
2. Switch Groundhog (wrappers delegate) — must be byte-identical to current goldens; gate on `test_link_policy`.
3. Switch nostr-gtk note card summary (removes app include).
4. Switch gnostr callers; visual review of article reader/wiki (heading sizes, quotes, code). Expected
   intended differences: GFM tables/tasks/strike now render, `snake_case` no longer italic, unmatched
   `*` literal, >64 KiB articles show escaped text instead of formatted (risk for very long NIP-23
   articles — consider raising MAX_INPUT_BYTES for FULL mode via an option, keeping Groundhog at 64 KiB).
5. Delete `markdown_pango.{c,h}` + compat test.
Other risks: ABI — `GnMarkdownPangoOptions` struct is public; add a `reserved` padding or use a
boxed/builder to allow growth. Article reader formats on UI thread today (`gnostr-article-reader.c:553`);
long articles may need the Groundhog worker split. No dependency on gnostr singletons is introduced.
