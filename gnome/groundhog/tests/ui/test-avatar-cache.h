#pragma once

#include "gh-picture-cache.h"

/* Real decode/cache, with an in-process HTTP answer; never a network fetch. */
static inline void
gh_test_avatar_get(gpointer data, const gchar *uri, gsize limit, GCancellable *cancel,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  (void)uri;
  (void)limit;
  g_autoptr(GTask) task = g_task_new(NULL, cancel, callback, user_data);
  if (!g_task_return_error_if_cancelled(task))
    g_task_return_pointer(task, g_bytes_ref(data), (GDestroyNotify)g_bytes_unref);
}

static inline GBytes *
gh_test_avatar_finish(gpointer data, GAsyncResult *result, GError **error)
{
  (void)data;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static inline gchar *
gh_test_avatar_uri(const gchar *pubkey, gpointer source)
{
  (void)pubkey;
  (void)source;
  return g_strdup("https://pictures.test.invalid/avatar.png");
}

static inline gboolean
gh_test_avatar_hidden(gpointer data, const gchar *pubkey)
{
  (void)pubkey;
  return *(gboolean *)data;
}

static inline GhPictureCache *
gh_test_avatar_cache(const gchar *pubkey)
{
  const guint8 pixel[] = { 255, 0, 0, 255 };
  g_autoptr(GBytes) bytes = g_bytes_new(pixel, sizeof pixel);
  g_autoptr(GdkTexture) texture = GDK_TEXTURE(gdk_memory_texture_new(
    1, 1, GDK_MEMORY_R8G8B8A8, bytes, 4));
  GBytes *png = gdk_texture_save_to_png_bytes(texture);
  static const GhHttpTransport transport = { gh_test_avatar_get, gh_test_avatar_finish };
  g_autoptr(GhWebContent) web = gh_web_content_new(NULL, &transport, png);
  GhPictureCache *cache = gh_picture_cache_new(web);
  g_object_set_data_full(G_OBJECT(cache), "test-avatar-bytes", png, (GDestroyNotify)g_bytes_unref);
  gh_picture_cache_set_uri_source(cache, gh_test_avatar_uri, NULL);
  gh_picture_cache_allow(cache, pubkey);
  gh_picture_cache_get_for(cache, pubkey);
  while (gh_picture_cache_is_loading(cache, "https://pictures.test.invalid/avatar.png"))
    g_main_context_iteration(NULL, TRUE);
  g_assert_nonnull(gh_picture_cache_get_for(cache, pubkey));
  return cache;
}

static inline AdwAvatar *
gh_test_avatar_with_picture(GtkWidget *root, GdkPaintable *picture)
{
  if (ADW_IS_AVATAR(root) && adw_avatar_get_custom_image(ADW_AVATAR(root)) == picture)
    return ADW_AVATAR(root);
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child)) {
    AdwAvatar *found = gh_test_avatar_with_picture(child, picture);
    if (found)
      return found;
  }
  return NULL;
}
