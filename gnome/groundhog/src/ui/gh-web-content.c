#include "gh-web-content.h"
#include "gh-metadata-strip.h"
#include "gh-link-policy.h"
#include <nostr-gtk-1.0/gn-og-preview.h>
#include <nostr-gtk-1.0/gn-animated-image.h>
#include <string.h>

struct _GhWebContent {
  GObject parent_instance;
  GhNetHttp *http;
  GhHttpTransport transport;
  gpointer data;
};
G_DEFINE_FINAL_TYPE(GhWebContent, gh_web_content, G_TYPE_OBJECT)

const gchar *
gh_web_content_setting(GhWebKind kind)
{
  static const gchar *keys[] = { "link-previews", "load-remote-images", "load-profile-pictures",
                                 "load-remote-images" };
  g_return_val_if_fail(kind < GH_WEB_N_KINDS, NULL);
  return keys[kind];
}

void
gh_web_result_free(GhWebResult *result)
{
  if (!result) return;
  g_free(result->title);
  g_free(result->description);
  g_free(result->image_url);
  g_free(result->site_name);
  g_clear_object(&result->texture);
  g_free(result);
}

/* nostrc-8xfib.3: the <head> parser is nostr-gtk's shared one; the
 * og:image address passes Groundhog's link policy. */
static gboolean
image_policy(const char *image_url, gpointer data)
{
  (void)data;
  return gh_link_policy_can_preview(image_url);
}

static GhWebResult *
parse_preview(GBytes *bytes, const gchar *uri, GError **error)
{
  g_autoptr(GnOgMetadata) metadata = gn_og_metadata_parse_html(bytes, uri, image_policy, NULL, error);
  if (!metadata) return NULL;
  GhWebResult *result = g_new0(GhWebResult, 1);
  result->title = g_strdup(gn_og_metadata_get_title(metadata));
  result->description = g_strdup(gn_og_metadata_get_description(metadata));
  result->site_name = g_strdup(gn_og_metadata_get_site_name(metadata));
  result->image_url = g_strdup(gn_og_metadata_get_image_url(metadata));
  return result;
}

static GhWebResult *
parse_result(GBytes *bytes, GhWebKind kind, const gchar *uri, GError **error)
{
  g_autoptr(GhWebResult) result = g_new0(GhWebResult, 1);
  if (kind != GH_WEB_PREVIEW) {
    /* JPEG and PNG are measured before decoding; any other format GTK's
     * loaders know (WebP, GIF, AVIF - what most Nostr image hosts serve
     * profile pictures as; owner report: pictures did not load) is decoded
     * from the size-capped download and measured after. */
    guint width = 0, height = 0;
    GhMediaFormat format = GH_MEDIA_FORMAT_OTHER;
    g_autoptr(GError) probe_error = NULL;
    if (gh_media_probe_dimensions(bytes, &format, &width, &height, &probe_error)) {
      if (!width || !height || width > 4096 || height > 4096) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Image dimensions exceed the limit");
        return NULL;
      }
    } else if (!g_error_matches(probe_error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
      g_propagate_error(error, g_steal_pointer(&probe_error));
      return NULL;
    }
    result->texture = gdk_texture_new_from_bytes(bytes, error);
    if (!result->texture) return NULL;
    if (gdk_texture_get_width(result->texture) > 4096 || gdk_texture_get_height(result->texture) > 4096) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Image dimensions exceed the limit");
      return NULL;
    }
    /* nostrc-p15n5.8: an animated GIF also travels as its animation, kept
     * on the still texture (gn_animated_image_paintable_for_texture()). */
    g_autoptr(GnAnimatedImage) animation =
      gn_animated_image_probe(bytes, NULL, NULL) ? gn_animated_image_new_from_bytes(bytes, 4096, NULL)
                                                 : NULL;
    if (animation) gn_animated_image_set_for_texture(result->texture, animation);
    return g_steal_pointer(&result);
  }
  return parse_preview(bytes, uri, error);
}

typedef struct {
  GhWebKind kind;
  gchar *uri;
} LoadData;

static void
load_data_free(gpointer data)
{
  LoadData *load = data;
  g_free(load->uri);
  g_free(load);
}

static void
loaded(GObject *source, GAsyncResult *answer, gpointer data)
{
  (void)source;
  g_autoptr(GTask) task = data;
  GhWebContent *self = g_task_get_source_object(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = self->transport.get_finish
    ? self->transport.get_finish(self->data, answer, &error)
    : gh_net_http_get_finish(self->http, answer, &error);
  if (!bytes) { g_task_return_error(task, g_steal_pointer(&error)); return; }
  if (g_task_return_error_if_cancelled(task)) return;
  LoadData *load = g_task_get_task_data(task);
  GhWebResult *result = parse_result(bytes, load->kind, load->uri, &error);
  if (result) g_task_return_pointer(task, result, (GDestroyNotify)gh_web_result_free);
  else g_task_return_error(task, g_steal_pointer(&error));
}

void
gh_web_content_load_async(GhWebContent *self, const gchar *uri, GhWebKind kind,
                           GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new(self, cancel, callback, data);
  LoadData *load = g_new0(LoadData, 1);
  load->kind = kind;
  load->uri = g_strdup(uri);
  g_task_set_task_data(task, load, load_data_free);
  /* Pictures up to 6 MiB (W33: several common profile pictures are 2.5-3
   * MiB); still decoded only within the 4096 px bound. */
  gsize limit = kind == GH_WEB_PREVIEW ? 256 * 1024 : 6 * 1024 * 1024;
  if (self->transport.get_async)
    self->transport.get_async(self->data, uri, limit, cancel, loaded, task);
  else if (kind == GH_WEB_PREVIEW)
    /* nostrc-p15n5.7: only the page's first 256 KiB are read, and that is
     * not an error. Popular pages are larger (GitHub's ~400 KiB) and every
     * preview of one failed with "larger than 262144 bytes"; the og: tags
     * are in the head, at the start. */
    gh_net_http_get_public_prefix_async(self->http, uri, "text/html,application/xhtml+xml",
                                        limit, cancel, loaded, task);
  else
    gh_net_http_get_public_async(self->http, uri, "image/*", limit, cancel, loaded, task);
}

GhWebResult *
gh_web_content_load_finish(GhWebContent *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
gh_web_content_dispose(GObject *object)
{
  g_clear_object(&GH_WEB_CONTENT(object)->http);
  G_OBJECT_CLASS(gh_web_content_parent_class)->dispose(object);
}
static void gh_web_content_class_init(GhWebContentClass *klass)
{ G_OBJECT_CLASS(klass)->dispose = gh_web_content_dispose; }
static void gh_web_content_init(GhWebContent *self) { (void)self; }

GhWebContent *
gh_web_content_new(GSettings *settings, const GhHttpTransport *transport, gpointer data)
{
  GhWebContent *self = g_object_new(GH_TYPE_WEB_CONTENT, NULL);
  self->http = gh_net_http_new(settings);
  if (transport) self->transport = *transport;
  self->data = data;
  return self;
}
