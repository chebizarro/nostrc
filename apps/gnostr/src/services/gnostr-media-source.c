/*
 * gnostr-media-source.c - Gnostr policy and plumbing for nostr-gtk media
 * widgets (nostrc-8xfib.4). See the header.
 *
 * The fetch, status checks and settings binding were in
 * gnostr-image-viewer.c and gnostr-video-player.c before those widgets moved
 * into nostr-gtk; their behaviour is unchanged.
 */

#include "gnostr-media-source.h"
#include <nostr-gtk-1.0/gn-media-viewer.h>
#include <nostr-gtk-1.0/gn-video-player.h>
#include "../ui/gnostr-main-window.h"
#include "../util/utils.h"
#include <glib/gi18n.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define GNOSTR_CLIENT_SCHEMA_ID "org.gnostr.Client"

struct _GnostrMediaSource {
  GObject parent_instance;
};

static void media_source_iface_init(GnMediaSourceInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnostrMediaSource, gnostr_media_source, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GN_TYPE_MEDIA_SOURCE, media_source_iface_init))

static gboolean
remote_media_allowed(void)
{
#ifdef HAVE_SOUP3
  return gnostr_is_remote_media_allowed();
#else
  return FALSE;
#endif
}

static GnMediaPolicy
get_policy(GnMediaSource *source, const char *url, GnMediaKind kind)
{
  (void)source; (void)url;
  if (remote_media_allowed()) return GN_MEDIA_POLICY_ALLOW;
  /* Gnostr offered Load for a blocked image but not for a video. */
  return kind == GN_MEDIA_KIND_VIDEO ? GN_MEDIA_POLICY_BLOCKED : GN_MEDIA_POLICY_ASK;
}

#ifdef HAVE_SOUP3
typedef struct {
  SoupMessage *msg;
  char *url;
  gsize max_bytes;
} FetchData;

static void
fetch_data_free(gpointer data)
{
  FetchData *fd = data;
  g_clear_object(&fd->msg);
  g_free(fd->url);
  g_free(fd);
}

static void
on_read(GObject *session, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  FetchData *fd = g_task_get_task_data(task);
  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(session), result, &error);
  if (g_task_return_error_if_cancelled(task)) {
    g_clear_error(&error);
  } else if (!bytes) {
    g_task_return_error(task, error);
  } else {
    /* hq-snq39: non-2xx bodies are error pages, not images. */
    guint status = soup_message_get_status(fd->msg);
    if (status < 200 || status >= 300)
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u for %s", status, fd->url);
    else if (g_bytes_get_size(bytes) == 0)
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Empty image data");
    else if (fd->max_bytes && g_bytes_get_size(bytes) > fd->max_bytes)
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "Image is too large");
    else
      g_task_return_pointer(task, g_bytes_ref(bytes), (GDestroyNotify)g_bytes_unref);
  }
  g_clear_pointer(&bytes, g_bytes_unref);
  g_object_unref(task);
}
#endif

static void
fetch_async(GnMediaSource *source, const char *url, GnMediaKind kind, gsize max_bytes,
            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  (void)kind;
  GTask *task = g_task_new(source, cancellable, callback, user_data);
  g_task_set_source_tag(task, fetch_async);
#ifdef HAVE_SOUP3
  g_autoptr(SoupSession) session = gnostr_get_shared_soup_session();
  if (!session) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "%s",
                            _("Remote images unavailable"));
    g_object_unref(task);
    return;
  }
  SoupMessage *msg = url ? soup_message_new("GET", url) : NULL;
  if (!msg) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s",
                            _("Remote image URL is invalid"));
    g_object_unref(task);
    return;
  }
  FetchData *fd = g_new0(FetchData, 1);
  fd->msg = msg;
  fd->url = g_strdup(url);
  fd->max_bytes = max_bytes;
  g_task_set_task_data(task, fd, fetch_data_free);
  /* nostrc-soup-dblf: requests on the shared session are never cancelled;
   * a cancelled one completes harmlessly and its GTask reports CANCELLED. */
  soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, NULL, on_read, task);
#else
  (void)url; (void)max_bytes;
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s",
                          _("Remote images unavailable"));
  g_object_unref(task);
#endif
}

static GBytes *
fetch_finish(GnMediaSource *source, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, source), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* Gnostr policy, kept as it was: GTK media backend opens the URL itself. */
static GtkMediaStream *
open_stream(GnMediaSource *source, const char *url, GError **error)
{
  (void)source;
  if (!url || !*url) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No video URL");
    return NULL;
  }
  g_autoptr(GFile) file = g_file_new_for_uri(url);
  return gtk_media_file_new_for_file(file);
}

static GSettings *
client_settings(void)
{
  static GSettings *settings;
  if (settings) return settings;
  GSettingsSchemaSource *schemas = g_settings_schema_source_get_default();
  g_autoptr(GSettingsSchema) schema =
    schemas ? g_settings_schema_source_lookup(schemas, GNOSTR_CLIENT_SCHEMA_ID, TRUE) : NULL;
  if (!schema) return NULL;
  settings = g_settings_new(GNOSTR_CLIENT_SCHEMA_ID);
  return settings;
}

static void
on_link_copied(GnMediaViewer *viewer, const char *url, gpointer data)
{
  (void)url; (void)data;
  GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(viewer));
  if (parent) gnostr_main_window_show_toast(GTK_WIDGET(parent), _("Link copied"));
}

static void
adopt(GnMediaSource *source, GtkWidget *widget)
{
  (void)source;
  if (GN_IS_VIDEO_PLAYER(widget)) {
    GSettings *settings = client_settings();
    if (settings) {
      g_settings_bind(settings, "video-autoplay", widget, "autoplay", G_SETTINGS_BIND_GET);
      g_settings_bind(settings, "video-loop", widget, "loop", G_SETTINGS_BIND_GET);
    }
  } else if (GN_IS_MEDIA_VIEWER(widget)) {
    /* Gnostr decoded any format GTK knows (WebP, AVIF via gdk-pixbuf) at
     * full size; keep that, now with size bounds. */
    GnMediaDecodeLimits limits = {
      .max_bytes = 64u * 1024u * 1024u,
      .max_dimension = 16384,
      .allowed = GN_MEDIA_FORMAT_PNG | GN_MEDIA_FORMAT_JPEG | GN_MEDIA_FORMAT_GIF |
                 GN_MEDIA_FORMAT_PIXBUF_FALLBACK,
    };
    gn_media_viewer_set_decode_limits(GN_MEDIA_VIEWER(widget), &limits);
    g_signal_connect(widget, "link-copied", G_CALLBACK(on_link_copied), NULL);
  }
}

static void
media_source_iface_init(GnMediaSourceInterface *iface)
{
  iface->get_policy = get_policy;
  iface->fetch_async = fetch_async;
  iface->fetch_finish = fetch_finish;
  iface->open_stream = open_stream;
  iface->adopt = adopt;
}

static void gnostr_media_source_class_init(GnostrMediaSourceClass *klass) { (void)klass; }
static void gnostr_media_source_init(GnostrMediaSource *self) { (void)self; }

GnostrMediaSource *
gnostr_media_source_new(void)
{
  return g_object_new(GNOSTR_TYPE_MEDIA_SOURCE, NULL);
}

GnMediaSource *
gnostr_media_source_get(void)
{
  static GnMediaSource *instance;
  if (!instance) instance = GN_MEDIA_SOURCE(gnostr_media_source_new());
  return instance;
}

void
gnostr_media_source_install(void)
{
  gn_media_source_set_default(gnostr_media_source_get());
}
