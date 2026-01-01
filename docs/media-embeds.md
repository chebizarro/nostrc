# Inline Media Embeds

## Overview

The Nostr client now automatically detects and displays inline images and videos from URLs in note content. Media is loaded asynchronously and displayed directly within note cards.

## Features

✅ **Image Support** - Displays .jpg, .jpeg, .png, .gif, .webp, .bmp, .svg
✅ **Video Support** - Displays .mp4, .webm, .mov, .avi, .mkv, .m4v
✅ **Async Loading** - Images fetched via libsoup without blocking UI
✅ **Multiple Media** - Supports multiple images/videos per note
✅ **Auto-Detection** - Automatically detects media URLs in content
✅ **Cancellable** - Requests cancelled when note card is destroyed
✅ **Styled** - Rounded corners, proper spacing, theme-aware

## How It Works

### Detection
1. Note content is parsed for HTTP(S) URLs
2. URLs are checked against image/video extensions
3. Matching URLs trigger media widget creation

### Image Loading
```
URL detected → GtkPicture created → libsoup fetch → GdkTexture → Display
```

### Video Loading
```
URL detected → GtkVideo created → GFile from URI → Display with controls
```

## Implementation Details

### Image Extensions
- `.jpg`, `.jpeg` - JPEG images
- `.png` - PNG images
- `.gif` - Animated GIFs
- `.webp` - WebP images
- `.bmp` - Bitmap images
- `.svg` - SVG vector graphics

### Video Extensions
- `.mp4` - MPEG-4 video
- `.webm` - WebM video
- `.mov` - QuickTime video
- `.avi` - AVI video
- `.mkv` - Matroska video
- `.m4v` - MPEG-4 video

### Media Widget Properties

**Images (GtkPicture)**
- Content fit: `GTK_CONTENT_FIT_CONTAIN` (preserves aspect ratio)
- Can shrink: `TRUE`
- Size: Width expands, height fixed at 300px
- CSS class: `.note-media-image`

**Videos (GtkVideo)**
- Autoplay: `FALSE` (user must click to play)
- Loop: `TRUE` (videos loop when finished)
- Size: Width expands, height fixed at 300px
- CSS class: `.note-media-video`
- Controls: Built-in GTK video controls

### Async Image Loading

Images are loaded asynchronously using libsoup:

```c
static void load_media_image(GnostrNoteCardRow *self, const char *url, GtkPicture *picture) {
  GCancellable *cancellable = g_cancellable_new();
  g_hash_table_insert(self->media_cancellables, g_strdup(url), cancellable);
  
  SoupMessage *msg = soup_message_new("GET", url);
  soup_session_send_and_read_async(
    self->media_session,
    msg,
    G_PRIORITY_LOW,
    cancellable,
    on_media_image_loaded,
    picture
  );
}
```

### Request Management

- Each note card has a `media_session` (SoupSession)
- Each media request has a `GCancellable` stored in `media_cancellables` hash table
- When note card is disposed, all requests are cancelled
- 30-second timeout for media requests

### CSS Styling

```css
.note-media-image {
  border-radius: 8px;
  background: alpha(@theme_bg_color, 0.3);
  margin-top: 8px;
  margin-bottom: 8px;
}

.note-media-video {
  border-radius: 8px;
  background: alpha(@theme_bg_color, 0.3);
  margin-top: 8px;
  margin-bottom: 8px;
}
```

## Usage Examples

### Image in Note
```
Check out this cool image!
https://example.com/photo.jpg
```
Result: Image displayed inline below text

### Video in Note
```
Watch this video:
https://example.com/video.mp4
```
Result: Video player with controls displayed inline

### Multiple Media
```
Here are some photos:
https://example.com/photo1.jpg
https://example.com/photo2.png
And a video:
https://example.com/video.webm
```
Result: All media items displayed in order

## Performance Considerations

- **Lazy Loading**: Media only loaded when note is visible
- **Cancellation**: Requests cancelled if note card is destroyed
- **Priority**: Media requests use `G_PRIORITY_LOW`
- **Timeout**: 30-second timeout prevents hanging
- **Memory**: GdkTexture automatically manages image memory
- **Cleanup**: All widgets and requests properly cleaned up

## Interaction with Other Features

### OG Preview Widget
- Media URLs are **not** used for OG previews
- OG preview only triggers for non-media URLs
- This prevents duplicate displays

### Note Embeds
- Media embeds are separate from NIP-19/21 embeds
- Both can appear in the same note
- Media appears first, then embeds

## Video Controls

GTK4's `GtkVideo` provides built-in controls:
- Play/Pause button
- Seek bar
- Volume control
- Fullscreen toggle (if supported)

Users can:
- Click to play/pause
- Drag seek bar to navigate
- Adjust volume
- Right-click for context menu

## Error Handling

### Image Load Failures
- Failed requests logged to debug output
- Empty/invalid images silently ignored
- Widget remains but shows no content

### Video Load Failures
- GTK handles video errors internally
- Error icon may be shown by GTK
- User can still see URL in note text

## Testing

To test media embeds:

```bash
# Build
cmake --build build --target gnostr

# Run
GNOSTR_LIVE=TRUE ./build/apps/gnostr/gnostr
```

Post test notes:
- `https://picsum.photos/800/600` - Random image
- `https://example.com/sample.mp4` - Video
- Multiple URLs on separate lines

## Future Enhancements

- [ ] Image gallery/lightbox view
- [ ] Thumbnail generation for videos
- [ ] Progress indicator during image load
- [ ] Image caching to disk
- [ ] Support for image albums (multiple images in grid)
- [ ] GIF animation controls
- [ ] Video thumbnail preview
- [ ] Lazy loading (only load when scrolled into view)
- [ ] Max image dimensions (prevent huge images)
- [ ] Image compression/optimization

## Files Modified

- `apps/gnostr/src/ui/note_card_row.c`
  - Added `media_session` and `media_cancellables`
  - Implemented `is_image_url()` and `is_video_url()`
  - Implemented `load_media_image()` and `on_media_image_loaded()`
  - Updated media detection to create GtkPicture/GtkVideo widgets
  - Added proper cleanup in dispose

- `apps/gnostr/data/ui/styles/gnostr.css`
  - Added `.note-media-image` styles
  - Added `.note-media-video` styles

## Dependencies

- GTK4 (GtkPicture, GtkVideo)
- libsoup-3.0 (HTTP requests)
- GdkTexture (image rendering)
- GFile (video file handling)
