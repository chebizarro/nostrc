# Open Graph Preview Widget

## Overview

A reusable GTK4 widget that fetches and displays Open Graph metadata from URLs, creating Twitter-style link preview cards within Nostr note cards.

## Features

✅ **Async HTML Fetching** - Uses libsoup for non-blocking HTTP requests
✅ **Open Graph Parsing** - Extracts og:title, og:description, og:image, og:url, og:site_name
✅ **Fallback Support** - Falls back to `<title>` and `<meta name="description">` tags
✅ **Image Loading** - Async image fetching with GdkTexture
✅ **Caching** - In-memory cache to avoid re-fetching the same URL
✅ **Clickable Cards** - Opens URL in browser via GtkUriLauncher
✅ **Loading States** - Shows spinner while fetching
✅ **Error Handling** - Gracefully handles fetch failures and malformed HTML
✅ **Theme-Aware Styling** - Beautiful CSS with hover effects

## Files Created

### Widget Implementation
- `apps/gnostr/src/ui/og-preview-widget.h` - Public API header
- `apps/gnostr/src/ui/og-preview-widget.c` - Widget implementation with HTML parsing

### UI Integration
- `apps/gnostr/data/ui/widgets/note-card-row.ui` - Added og_preview_container
- `apps/gnostr/src/ui/note_card_row.c` - URL detection and widget instantiation

### Styling
- `apps/gnostr/data/ui/styles/gnostr.css` - Added .og-preview-* classes

## Architecture

### Widget Structure
```
OgPreviewWidget (GtkWidget)
├── GtkSpinner (loading state)
└── GtkBox (card_box)
    ├── GtkPicture (og:image)
    └── GtkBox (text_box)
        ├── GtkLabel (title)
        ├── GtkLabel (description)
        └── GtkLabel (site_name)
```

### Data Flow
1. Note card detects first HTTP(S) URL in content
2. Creates OgPreviewWidget and calls `og_preview_widget_set_url()`
3. Widget checks cache, if miss:
   - Fetches HTML via libsoup async
   - Parses Open Graph meta tags
   - Caches result
4. Updates UI with metadata
5. Loads og:image asynchronously
6. Card becomes clickable

## HTML Parsing

Uses lightweight string parsing (no external HTML parser needed):
- Case-insensitive substring search
- Extracts `<meta property="og:*" content="...">` tags
- Handles both single and double quotes
- Falls back to `<title>` and standard meta tags

## Public API

```c
// Create new widget
OgPreviewWidget *og_preview_widget_new(void);

// Set URL to fetch and preview
void og_preview_widget_set_url(OgPreviewWidget *self, const char *url);

// Clear preview and cancel requests
void og_preview_widget_clear(OgPreviewWidget *self);
```

## Integration Example

```c
// In note card content setter:
if (url_detected) {
  self->og_preview = og_preview_widget_new();
  gtk_box_append(GTK_BOX(self->og_preview_container), GTK_WIDGET(self->og_preview));
  og_preview_widget_set_url(self->og_preview, url);
  gtk_widget_set_visible(self->og_preview_container, TRUE);
}
```

## Performance Considerations

- **Caching**: URL → OgMetadata hash table prevents duplicate fetches
- **Async Everything**: No blocking calls, all network I/O is async
- **Lazy Image Loading**: Images load separately after metadata
- **Request Cancellation**: Cancels in-flight requests when widget is destroyed
- **Timeout**: 10-second timeout on HTTP requests
- **One Preview Per Note**: Only first URL is previewed to avoid clutter

## CSS Classes

- `.og-preview` - Container widget
- `.og-preview-card` - Card frame with hover effects
- `.og-preview-image` - Image container
- `.og-preview-title` - Title label (bold)
- `.og-preview-description` - Description (dimmed, 2 lines max)
- `.og-preview-site` - Site name/domain (small, muted)

## Limitations & Future Enhancements

### Current Limitations
- HTML parsing is regex-based (good enough for OG tags)
- No video support (og:video)
- No favicon support
- Fixed 200px image height

### Potential Enhancements
- [ ] Support og:video for video previews
- [ ] Add domain favicon next to site name
- [ ] Configurable image height
- [ ] Content-type sniffing before fetch
- [ ] Persistent cache (SQLite)
- [ ] Twitter Card support (twitter:*)
- [ ] Better error UI (show minimal card on failure)

## Testing

To test the widget:
1. Build: `cmake --build build --target gnostr`
2. Run: `./build/apps/gnostr/gnostr`
3. Post a note with a URL (e.g., "Check out https://github.com")
4. The preview card should appear below the note content

## Dependencies

- GTK4 (widgets, layout)
- libsoup-3.0 (HTTP requests)
- GdkPixbuf/GdkTexture (image loading)
- GLib (utilities, async)

## Build Integration

The widget is automatically included via `file(GLOB)` in CMakeLists.txt:
```cmake
file(GLOB APP_SRC CONFIGURE_DEPENDS src/*.c src/ui/*.c ...)
```

No manual CMakeLists.txt changes needed for new files in `src/ui/`.
