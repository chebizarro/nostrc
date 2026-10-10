#include <nostr-gtk-1.0/gn-animated-image.h>
#include "gn-media-decode-private.h"
#include <string.h>

/* nostrc-p15n5.8: a bounded GIF decoder (GIF87a/89a, LZW, local and global
 * palettes, transparency, interlacing, disposal 0-3) producing one RGBA
 * texture per frame. Nothing here touches gdk-pixbuf. */

#define TRACKER_DATA "gn-animated-image-tracker"
#define TEXTURE_DATA "gn-animated-image"
#define MIN_DELAY_MS 20
#define SLOW_DELAY_MS 100
#define MAX_CODES 4096

struct _GnAnimatedImage {
  GObject parent_instance;
  guint width, height;
  GPtrArray *frames;   /* GdkTexture* */
  GArray *delays;      /* guint ms */
  guint frame;
  guint visible;       /* attached widgets that are mapped */
  guint timer;
};

static void paintable_iface_init(GdkPaintableInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(GnAnimatedImage, gn_animated_image, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE, paintable_iface_init))

/* ---- decoding ------------------------------------------------------------ */

static guint
le16(const guint8 *p)
{
  return (guint)p[0] | ((guint)p[1] << 8);
}

gboolean
gn_animated_image_probe(GBytes *bytes, guint *out_width, guint *out_height)
{
  gsize len = 0;
  const guint8 *d = bytes ? g_bytes_get_data(bytes, &len) : NULL;
  if (!d || len < 10 || (memcmp(d, "GIF87a", 6) != 0 && memcmp(d, "GIF89a", 6) != 0))
    return FALSE;
  if (out_width) *out_width = le16(d + 6);
  if (out_height) *out_height = le16(d + 8);
  return TRUE;
}

/* LZW codes into out (out_len indices); a short stream leaves the rest 0,
 * as viewers do for truncated files. FALSE for an impossible code. */
static gboolean
lzw_decode(const guint8 *data, gsize len, guint min_size, guint8 *out, gsize out_len)
{
  g_autofree guint16 *prefix = g_new0(guint16, MAX_CODES);
  g_autofree guint8 *suffix = g_new0(guint8, MAX_CODES);
  g_autofree guint8 *stack = g_new(guint8, MAX_CODES + 1);
  guint clear = 1u << min_size, eoi = clear + 1, avail = clear + 2;
  guint size = min_size + 1;
  gint old = -1;
  guint8 first = 0;
  guint32 bits = 0;
  guint nbits = 0;
  gsize pos = 0, o = 0;
  for (guint i = 0; i < clear; i++) suffix[i] = (guint8)i;
  while (o < out_len) {
    while (nbits < size) {
      if (pos >= len) return TRUE;
      bits |= (guint32)data[pos++] << nbits;
      nbits += 8;
    }
    guint code = bits & ((1u << size) - 1);
    bits >>= size;
    nbits -= size;
    if (code == clear) {
      size = min_size + 1;
      avail = clear + 2;
      old = -1;
      continue;
    }
    if (code == eoi) return TRUE;
    if (old < 0) {
      if (code >= clear) return FALSE;
      out[o++] = (guint8)code;
      first = (guint8)code;
      old = (gint)code;
      continue;
    }
    if (code > avail) return FALSE;
    guint in = code, sp = 0;
    if (code == avail) {
      stack[sp++] = first;
      code = (guint)old;
    }
    while (code >= clear) {
      if (sp >= MAX_CODES) return FALSE;
      stack[sp++] = suffix[code];
      code = prefix[code];
    }
    first = suffix[code];
    stack[sp++] = first;
    if (avail < MAX_CODES) {
      prefix[avail] = (guint16)old;
      suffix[avail] = first;
      avail++;
      if (avail == (1u << size) && size < 12) size++;
    }
    old = (gint)in;
    while (sp && o < out_len) out[o++] = stack[--sp];
  }
  return TRUE;
}

/* Skips data sub-blocks at *pos; appends them to collect when non-NULL.
 * FALSE when the file ends before the terminator. */
static gboolean
sub_blocks(const guint8 *d, gsize len, gsize *pos, GByteArray *collect)
{
  while (*pos < len) {
    guint n = d[(*pos)++];
    if (!n) return TRUE;
    if (*pos + n > len) return FALSE;
    if (collect) g_byte_array_append(collect, d + *pos, n);
    *pos += n;
  }
  return FALSE;
}

/* Interlaced rows arrive in passes: every 8th from 0, every 8th from 4,
 * every 4th from 2, every 2nd from 1. */
static guint
interlaced_row(guint r, guint h)
{
  static const guint start[] = { 0, 4, 2, 1 }, step[] = { 8, 8, 4, 2 };
  for (guint p = 0; p < 4; p++) {
    guint n = h > start[p] ? (h - start[p] + step[p] - 1) / step[p] : 0;
    if (r < n) return start[p] + r * step[p];
    r -= n;
  }
  return h;
}

/* still: stop after the first frame and accept a single-frame GIF. */
static gboolean
decode(GnAnimatedImage *self, GBytes *bytes, guint max_dimension, gboolean still, GError **error)
{
  gsize len = 0;
  const guint8 *d = g_bytes_get_data(bytes, &len);
  guint W = 0, H = 0;
  if (!gn_animated_image_probe(bytes, &W, &H) || len < 13) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Not a GIF image");
    return FALSE;
  }
  if (!W || !H || W > max_dimension || H > max_dimension) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Image dimensions exceed the limit");
    return FALSE;
  }
  self->width = W;
  self->height = H;
  gsize stride = (gsize)W * 4, canvas_size = stride * H;
  guint8 flags = d[10];
  gsize pos = 13;
  const guint8 *gct = NULL;
  guint gct_n = 0;
  if (flags & 0x80) {
    gct_n = 2u << (flags & 7);
    if (pos + 3 * gct_n > len) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Damaged GIF image");
      return FALSE;
    }
    gct = d + pos;
    pos += 3 * gct_n;
  }
  g_autofree guint8 *canvas = g_malloc0(canvas_size);
  g_autofree guint8 *saved = NULL;
  gint transparent = -1;
  guint disposal = 0, delay = 0;
  gsize budget = 0;
  while (pos < len) {
    guint8 block = d[pos++];
    if (block == 0x3B) break;
    if (block == 0x21) {
      if (pos >= len) break;
      guint8 label = d[pos++];
      if (label == 0xF9 && pos + 5 <= len && d[pos] >= 4) {
        guint8 packed = d[pos + 1];
        delay = le16(d + pos + 2) * 10;
        transparent = (packed & 1) ? d[pos + 4] : -1;
        disposal = (packed >> 2) & 7;
      }
      if (!sub_blocks(d, len, &pos, NULL)) break;
      continue;
    }
    if (block != 0x2C || pos + 9 > len) break;
    guint fx = le16(d + pos), fy = le16(d + pos + 2);
    guint fw = le16(d + pos + 4), fh = le16(d + pos + 6);
    guint8 packed = d[pos + 8];
    pos += 9;
    const guint8 *palette = gct;
    guint palette_n = gct_n;
    if (packed & 0x80) {
      palette_n = 2u << (packed & 7);
      if (pos + 3 * palette_n > len) break;
      palette = d + pos;
      pos += 3 * palette_n;
    }
    if (pos >= len) break;
    guint min_size = d[pos++];
    g_autoptr(GByteArray) lzw = g_byte_array_new();
    gboolean complete = sub_blocks(d, len, &pos, lzw);
    if (!fw || !fh || fw > max_dimension || fh > max_dimension || min_size < 1 ||
        min_size > 11)
      break;
    if (budget + canvas_size > GN_ANIMATED_IMAGE_MAX_BYTES ||
        self->frames->len >= GN_ANIMATED_IMAGE_MAX_FRAMES)
      break;
    g_autofree guint8 *indices = g_malloc0((gsize)fw * fh);
    if (!lzw_decode(lzw->data, lzw->len, min_size, indices, (gsize)fw * fh))
      break;
    if (disposal == 3) {
      g_free(saved);
      saved = g_memdup2(canvas, canvas_size);
    }
    for (guint r = 0; r < fh; r++) {
      guint y = (packed & 0x40) ? interlaced_row(r, fh) : r;
      if (y >= fh || fy + y >= H) continue;
      const guint8 *src = indices + (gsize)r * fw;
      guint8 *dst = canvas + (gsize)(fy + y) * stride;
      for (guint x = 0; x < fw && fx + x < W; x++) {
        guint idx = src[x];
        if ((gint)idx == transparent || !palette || idx >= palette_n) continue;
        guint8 *px = dst + (gsize)(fx + x) * 4;
        px[0] = palette[3 * idx];
        px[1] = palette[3 * idx + 1];
        px[2] = palette[3 * idx + 2];
        px[3] = 255;
      }
    }
    g_autoptr(GBytes) pixels = g_bytes_new(canvas, canvas_size);
    g_ptr_array_add(self->frames,
                    gdk_memory_texture_new((int)W, (int)H, GDK_MEMORY_R8G8B8A8, pixels, stride));
    guint ms = delay < MIN_DELAY_MS ? SLOW_DELAY_MS : delay;
    g_array_append_val(self->delays, ms);
    budget += canvas_size;
    if (disposal == 2 && fx < W) {
      for (guint y = fy; y < MIN(fy + fh, H); y++)
        memset(canvas + (gsize)y * stride + (gsize)fx * 4, 0, (gsize)(MIN(fx + fw, W) - fx) * 4);
    } else if (disposal == 3 && saved) {
      memcpy(canvas, saved, canvas_size);
    }
    transparent = -1;
    disposal = 0;
    delay = 0;
    if (!complete || still) break;
  }
  if (still && self->frames->len == 1) return TRUE;
  if (self->frames->len < 2) {
    g_set_error_literal(error, G_IO_ERROR,
                        self->frames->len ? G_IO_ERROR_NOT_SUPPORTED : G_IO_ERROR_INVALID_DATA,
                        self->frames->len ? "Not an animation" : "Damaged GIF image");
    return FALSE;
  }
  return TRUE;
}

/* ---- playback ------------------------------------------------------------ */

static void schedule(GnAnimatedImage *self);

static gboolean
on_tick(gpointer data)
{
  GnAnimatedImage *self = data;
  self->timer = 0;
  gn_animated_image_advance(self);
  schedule(self);
  return G_SOURCE_REMOVE;
}

static void
schedule(GnAnimatedImage *self)
{
  if (self->timer || !self->visible || self->frames->len < 2) return;
  self->timer = g_timeout_add(g_array_index(self->delays, guint, self->frame), on_tick, self);
}

static void
stop(GnAnimatedImage *self)
{
  g_clear_handle_id(&self->timer, g_source_remove);
}

void
gn_animated_image_advance(GnAnimatedImage *self)
{
  g_return_if_fail(GN_IS_ANIMATED_IMAGE(self));
  if (self->frames->len < 2) return;
  self->frame = (self->frame + 1) % self->frames->len;
  gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
}

gboolean
gn_animated_image_get_playing(GnAnimatedImage *self)
{
  g_return_val_if_fail(GN_IS_ANIMATED_IMAGE(self), FALSE);
  return self->timer != 0;
}

typedef struct {
  GnAnimatedImage *image;
  GtkWidget *widget;
  gulong map_id, unmap_id;
  gboolean counted;
} Tracker;

static void
tracker_set_counted(Tracker *tracker, gboolean counted)
{
  if (tracker->counted == counted) return;
  tracker->counted = counted;
  if (counted) {
    tracker->image->visible++;
    schedule(tracker->image);
  } else if (--tracker->image->visible == 0) {
    stop(tracker->image);
  }
}

static void
on_map(GtkWidget *widget, Tracker *tracker)
{
  (void)widget;
  tracker_set_counted(tracker, TRUE);
}

static void
on_unmap(GtkWidget *widget, Tracker *tracker)
{
  (void)widget;
  tracker_set_counted(tracker, FALSE);
}

static void
tracker_free(gpointer data)
{
  Tracker *tracker = data;
  if (g_signal_handler_is_connected(tracker->widget, tracker->map_id))
    g_signal_handler_disconnect(tracker->widget, tracker->map_id);
  if (g_signal_handler_is_connected(tracker->widget, tracker->unmap_id))
    g_signal_handler_disconnect(tracker->widget, tracker->unmap_id);
  tracker_set_counted(tracker, FALSE);
  g_object_unref(tracker->image);
  g_free(tracker);
}

void
gn_animated_image_attach(GtkWidget *widget, GdkPaintable *paintable)
{
  g_return_if_fail(GTK_IS_WIDGET(widget));
  Tracker *old = g_object_get_data(G_OBJECT(widget), TRACKER_DATA);
  if (old && paintable && (gpointer)old->image == (gpointer)paintable) return;
  if (!paintable || !GN_IS_ANIMATED_IMAGE(paintable)) {
    g_object_set_data(G_OBJECT(widget), TRACKER_DATA, NULL);
    return;
  }
  Tracker *tracker = g_new0(Tracker, 1);
  tracker->image = g_object_ref(GN_ANIMATED_IMAGE(paintable));
  tracker->widget = widget;
  tracker->map_id = g_signal_connect(widget, "map", G_CALLBACK(on_map), tracker);
  tracker->unmap_id = g_signal_connect(widget, "unmap", G_CALLBACK(on_unmap), tracker);
  g_object_set_data_full(G_OBJECT(widget), TRACKER_DATA, tracker, tracker_free);
  tracker_set_counted(tracker, gtk_widget_get_mapped(widget));
}

/* ---- paintable ----------------------------------------------------------- */

static GdkTexture *
current(GnAnimatedImage *self)
{
  return self->frames->len ? g_ptr_array_index(self->frames, self->frame) : NULL;
}

static void
paintable_snapshot(GdkPaintable *paintable, GdkSnapshot *snapshot, double width, double height)
{
  GdkTexture *texture = current(GN_ANIMATED_IMAGE(paintable));
  if (texture) gdk_paintable_snapshot(GDK_PAINTABLE(texture), snapshot, width, height);
}

static GdkPaintable *
paintable_current_image(GdkPaintable *paintable)
{
  GdkTexture *texture = current(GN_ANIMATED_IMAGE(paintable));
  return texture ? GDK_PAINTABLE(g_object_ref(texture)) : gdk_paintable_new_empty(0, 0);
}

static GdkPaintableFlags
paintable_flags(GdkPaintable *paintable)
{
  (void)paintable;
  return GDK_PAINTABLE_STATIC_SIZE;
}

static int
paintable_width(GdkPaintable *paintable)
{
  return (int)GN_ANIMATED_IMAGE(paintable)->width;
}

static int
paintable_height(GdkPaintable *paintable)
{
  return (int)GN_ANIMATED_IMAGE(paintable)->height;
}

static void
paintable_iface_init(GdkPaintableInterface *iface)
{
  iface->snapshot = paintable_snapshot;
  iface->get_current_image = paintable_current_image;
  iface->get_flags = paintable_flags;
  iface->get_intrinsic_width = paintable_width;
  iface->get_intrinsic_height = paintable_height;
}

/* ---- object -------------------------------------------------------------- */

static void
gn_animated_image_finalize(GObject *object)
{
  GnAnimatedImage *self = GN_ANIMATED_IMAGE(object);
  stop(self);
  g_ptr_array_unref(self->frames);
  g_array_unref(self->delays);
  G_OBJECT_CLASS(gn_animated_image_parent_class)->finalize(object);
}

static void
gn_animated_image_class_init(GnAnimatedImageClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gn_animated_image_finalize;
}

static void
gn_animated_image_init(GnAnimatedImage *self)
{
  self->frames = g_ptr_array_new_with_free_func(g_object_unref);
  self->delays = g_array_new(FALSE, FALSE, sizeof(guint));
}

GnAnimatedImage *
gn_animated_image_new_from_bytes(GBytes *bytes, guint max_dimension, GError **error)
{
  g_return_val_if_fail(bytes != NULL, NULL);
  g_autoptr(GnAnimatedImage) self = g_object_new(GN_TYPE_ANIMATED_IMAGE, NULL);
  if (!decode(self, bytes, max_dimension ? max_dimension : 4096, FALSE, error)) return NULL;
  return g_steal_pointer(&self);
}

/* nostrc-8xfib.4: the first frame of any GIF with the same bounded decoder,
 * so a still GIF never needs gdk-pixbuf (gn-media-decode.c). */
GdkTexture *
gn_animated_image_decode_first_frame(GBytes *bytes, guint max_dimension, GError **error)
{
  g_return_val_if_fail(bytes != NULL, NULL);
  g_autoptr(GnAnimatedImage) self = g_object_new(GN_TYPE_ANIMATED_IMAGE, NULL);
  if (!decode(self, bytes, max_dimension ? max_dimension : 4096, TRUE, error)) return NULL;
  return g_object_ref(g_ptr_array_index(self->frames, 0));
}

guint
gn_animated_image_get_n_frames(GnAnimatedImage *self)
{
  g_return_val_if_fail(GN_IS_ANIMATED_IMAGE(self), 0);
  return self->frames->len;
}

guint
gn_animated_image_get_frame(GnAnimatedImage *self)
{
  g_return_val_if_fail(GN_IS_ANIMATED_IMAGE(self), 0);
  return self->frame;
}

GdkTexture *
gn_animated_image_get_current_texture(GnAnimatedImage *self)
{
  g_return_val_if_fail(GN_IS_ANIMATED_IMAGE(self), NULL);
  return current(self);
}

void
gn_animated_image_set_for_texture(GdkTexture *texture, GnAnimatedImage *animation)
{
  g_return_if_fail(GDK_IS_TEXTURE(texture));
  g_object_set_data_full(G_OBJECT(texture), TEXTURE_DATA,
                         animation ? g_object_ref(animation) : NULL, g_object_unref);
}

GnAnimatedImage *
gn_animated_image_get_for_texture(GdkTexture *texture)
{
  return texture ? g_object_get_data(G_OBJECT(texture), TEXTURE_DATA) : NULL;
}

GdkPaintable *
gn_animated_image_paintable_for_texture(GdkTexture *texture)
{
  GnAnimatedImage *animation = gn_animated_image_get_for_texture(texture);
  return animation ? GDK_PAINTABLE(animation) : GDK_PAINTABLE(texture);
}
