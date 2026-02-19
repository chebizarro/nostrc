# Content Safety Audit — nostrc-csaf

## Summary

Audit of all code paths where **untrusted Nostr relay content** reaches GTK
label widgets (`gtk_label_set_markup`). Malformed UTF-8, invalid Pango markup,
and zero-width Unicode characters from relay events can cause segmentation
faults in Pango's layout engine during allocation or widget disposal.

**Tag prefix:** `nostrc-csaf`

---

## Vulnerability Classes Found

### V1: Unescaped relay strings in Pango markup (CRITICAL)

NIP-34 (git repos/patches/issues) functions in `nostr-note-card-row.c` built
Pango markup by interpolating user-provided strings (name, description,
clone_urls, web_urls, license, title, repo_name, commit_id) directly into
`g_string_append_printf` format strings **without** `g_markup_escape_text`.

If a relay sends `name = "</span><invalid>"`, the resulting markup is
malformed, causing `pango_parse_markup` to fail or produce a corrupted
`PangoLayout` that segfaults during finalization.

**Affected paths:**
- `set_git_repo_mode()` — name, description, clone_urls[], web_urls[], license
- `set_git_patch_mode()` — title, repo_name, commit_id
- `set_git_issue_mode()` — title, repo_name

### V2: Byte-at-a-time UTF-8 splitting (HIGH)

`markdown_to_pango_summary()` and `process_inline()` in `markdown_pango.c`
called `escape_pango_text(p, 1)` followed by `p++`, advancing one **byte** at
a time. Multi-byte UTF-8 sequences (e.g. U+2019 RIGHT SINGLE QUOTATION MARK =
`E2 80 99`) were split into individual bytes, producing invalid UTF-8 that
corrupts Pango.

**Also affected:** The content renderer fallback path (when NDB parser fails)
had the same byte-at-a-time processing without any UTF-8 validation.

### V3: No UTF-8 validation on fallback path (MEDIUM)

`content_renderer.c` fallback path (when `storage_ndb_parse_content_blocks`
returns NULL) processed raw content without calling `g_utf8_validate` or
`g_utf8_make_valid`. Invalid UTF-8 bytes passed directly to Pango markup.

### V4: Insufficient zero-width character stripping (LOW)

`gnostr_strip_zwsp()` strips U+200B, U+200C, U+2060, U+FEFF but other
invisible/formatting Unicode characters (e.g. U+200E LRM, U+200F RLM,
U+2028 LINE SEPARATOR, U+2029 PARAGRAPH SEPARATOR, U+202A-U+202E bidi
overrides) can still reach Pango. The `gnostr_safe_set_markup` fallback
provides defense-in-depth for these.

---

## Fixes Applied

### F1: `gnostr_sanitize_utf8()` function (content_renderer.c)

New function that:
1. Validates UTF-8 via `g_utf8_validate`
2. Replaces invalid sequences with U+FFFD via `g_utf8_make_valid`
3. Strips dangerous zero-width characters via `gnostr_strip_zwsp`

### F2: `gnostr_safe_set_markup()` inline function (content_renderer.h)

Defense-in-depth wrapper around `gtk_label_set_markup` that:
1. Sanitizes UTF-8 via `gnostr_sanitize_utf8`
2. Validates Pango markup via `pango_parse_markup`
3. Falls back to `gtk_label_set_text` (stripping tags) if markup is invalid

This is now the **primary entry point** for setting markup from relay content.

### F3: NIP-34 markup escaping (nostr-note-card-row.c)

All user-provided strings in git repo/patch/issue modes now pass through
`g_markup_escape_text` before interpolation into Pango markup.

### F4: Content renderer fallback UTF-8 fix (content_renderer.c)

Fallback path now:
1. Sanitizes input via `gnostr_sanitize_utf8()` before processing
2. Uses `g_utf8_next_char()` to advance by whole codepoints, not bytes
3. Uses `g_markup_escape_text()` per codepoint instead of byte-level escaping

### F5: markdown_to_pango UTF-8 fix (markdown_pango.c)

- Added `append_escaped_utf8_char()` helper that escapes full codepoints
- Added `append_span_text_utf8()` helper for bold/italic/link spans
- Replaced all `escape_pango_text(p, 1); p++` patterns with proper
  UTF-8 advancement via `g_utf8_next_char`
- Added UTF-8 validation at function entry

### F6: Global gtk_label_set_markup replacement

All `gtk_label_set_markup` calls processing relay content now use
`gnostr_safe_set_markup()`:
- `nostr-note-card-row.c` — 8 call sites (content, subject, NIP-34 modes)
- `gnostr-note-embed.c` — 1 call site (embedded note content)
- `gnostr-profile-pane.c` — 1 call site (bio linkification)
- `gnostr-article-card.c` — 1 call site (summary)
- `gnostr-wiki-card.c` — 2 call sites (summary, full content)
- `gnostr-article-reader.c` — 1 call site (full content)

### Paths verified safe (no changes needed)

- `gnostr-profile-pane.c:1959,2004` — uses `g_markup_printf_escaped`
- `gnostr-highlight-card.c:336,360` — uses `g_markup_escape_text` first
- `gnostr-main-window.c:780,1380` — internal UI text, no relay content
- `gnostr-article-reader.c:129` — hashtag tags pre-escaped
- `nostr-note-card-row.c:421` — empty string literal
- `nip39_identity.c:253` — pre-escaped identity links

---

## Testing Recommendations

1. **Fuzz test**: Feed crafted NDB events with invalid UTF-8, embedded
   markup tags, zero-width characters, and bidi overrides through the
   content rendering pipeline.

2. **Specific attack vectors to test:**
   - Content containing `</span><b>injection</b>`
   - Content with raw bytes `\xFF\xFE` (invalid UTF-8)
   - Content with mixed valid/invalid: `Hello \x80 World`
   - NIP-34 repo name: `Test&<>'"Repo`
   - Long sequences of U+200B zero-width spaces
   - Content with U+2028 LINE SEPARATOR / U+2029 PARAGRAPH SEPARATOR

3. **ASan + UBSan**: Run under sanitizers while scrolling timeline with
   diverse relay content to catch remaining memory errors.
