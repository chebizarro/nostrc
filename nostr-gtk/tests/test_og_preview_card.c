/* GnOgPreviewCard driving a GnOgPreviewProvider (nostrc-8xfib.3): no I/O
 * unless asked, stale results dropped, cancellation chained, unbind quiesces. */
#include <nostr-gtk-1.0/gn-og-preview-card.h>
#include "nostrc-test-gdk-frame.h"

#define FAKE_TYPE_PROVIDER (fake_provider_get_type())
G_DECLARE_FINAL_TYPE(FakeProvider, fake_provider, FAKE, PROVIDER, GObject)
struct _FakeProvider {
  GObject parent_instance;
  guint metadata_calls, image_calls;
  GTask *pending_metadata, *pending_image;
  GCancellable *last_cancellable;
};
static void fake_iface_init(GnOgPreviewProviderInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(FakeProvider, fake_provider, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_OG_PREVIEW_PROVIDER, fake_iface_init))

/* A superseded request still returns (cancelled), as a real provider would. */
static void drop(GTask **task) {
  if (!*task) return;
  g_task_return_new_error(*task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "superseded");
  g_clear_object(task);
}
static void fake_meta_async(GnOgPreviewProvider *p, const char *url, GCancellable *c,
                            GAsyncReadyCallback cb, gpointer data) {
  FakeProvider *self = FAKE_PROVIDER(p);
  self->metadata_calls++;
  g_set_object(&self->last_cancellable, c);
  GTask *task = g_task_new(p, c, cb, data);
  g_task_set_task_data(task, g_strdup(url), g_free);
  drop(&self->pending_metadata);
  self->pending_metadata = task;
}
static GnOgMetadata *fake_meta_finish(GnOgPreviewProvider *p, GAsyncResult *r, GError **e) {
  (void)p;
  return g_task_propagate_pointer(G_TASK(r), e);
}
static void fake_image_async(GnOgPreviewProvider *p, const char *url, int w, int h,
                             GCancellable *c, GAsyncReadyCallback cb, gpointer data) {
  (void)url; (void)w; (void)h;
  FakeProvider *self = FAKE_PROVIDER(p);
  self->image_calls++;
  drop(&self->pending_image);
  self->pending_image = g_task_new(p, c, cb, data);
}
static GdkPaintable *fake_image_finish(GnOgPreviewProvider *p, GAsyncResult *r, GError **e) {
  (void)p;
  return g_task_propagate_pointer(G_TASK(r), e);
}
static void fake_iface_init(GnOgPreviewProviderInterface *iface) {
  iface->load_metadata_async = fake_meta_async;
  iface->load_metadata_finish = fake_meta_finish;
  iface->load_image_async = fake_image_async;
  iface->load_image_finish = fake_image_finish;
}
static void fake_dispose(GObject *o) {
  FakeProvider *self = FAKE_PROVIDER(o);
  drop(&self->pending_metadata);
  drop(&self->pending_image);
  g_clear_object(&self->last_cancellable);
  G_OBJECT_CLASS(fake_provider_parent_class)->dispose(o);
}
static void fake_provider_class_init(FakeProviderClass *k) { G_OBJECT_CLASS(k)->dispose = fake_dispose; }
static void fake_provider_init(FakeProvider *self) { (void)self; }

static void drain(void) { while (g_main_context_iteration(NULL, FALSE)); }

static void complete_metadata(FakeProvider *fake, const char *title, const char *image) {
  GTask *task = g_steal_pointer(&fake->pending_metadata);
  g_assert_nonnull(task);
  const char *url = g_task_get_task_data(task);
  g_task_return_pointer(task, gn_og_metadata_new(url, title, "Desc", NULL, image),
                        (GDestroyNotify)gn_og_metadata_unref);
  g_object_unref(task);
  drain();
}

static GtkWidget *find(GtkWidget *root, const char *name) {
  if (g_strcmp0(gtk_widget_get_name(root), name) == 0) return root;
  for (GtkWidget *c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *hit = find(c, name);
    if (hit) return hit;
  }
  return NULL;
}

static GnOgPreviewCard *new_card(FakeProvider **fake_out) {
  GnOgPreviewCard *card = g_object_ref_sink(gn_og_preview_card_new());
  FakeProvider *fake = g_object_new(FAKE_TYPE_PROVIDER, NULL);
  gn_og_preview_card_set_provider(card, GN_OG_PREVIEW_PROVIDER(fake));
  *fake_out = fake;
  return card;
}

static void test_no_io_by_default(void) {
  FakeProvider *fake;
  GnOgPreviewCard *card = new_card(&fake);
  gn_og_preview_card_set_url(card, "https://example.org/x");
  g_assert_cmpuint(fake->metadata_calls, ==, 0);
  g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(card), "og_load")));
  gn_og_preview_card_request_load(card);
  g_assert_cmpuint(fake->metadata_calls, ==, 1);
  g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(card), "og_spinner")));
  complete_metadata(fake, NULL, "https://example.org/i.png");
  /* Missing title: the host stands in; artwork waits for consent. */
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(GTK_WIDGET(card), "og_title"))), ==,
                  "example.org");
  g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(card), "og_spinner")));
  g_assert_cmpuint(fake->image_calls, ==, 0);
  g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(card), "og_load_image")));
  gn_og_preview_card_request_image(card);
  g_assert_cmpuint(fake->image_calls, ==, 1);
  guint8 px[] = { 1, 2, 3, 255 };
  g_autoptr(GBytes) b = g_bytes_new(px, 4);
  GdkTexture *tex = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, b, 4);
  GTask *t = g_steal_pointer(&fake->pending_image);
  g_task_return_pointer(t, tex, g_object_unref);
  g_object_unref(t);
  drain();
  g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(card), "og_image")));
  g_object_unref(card);
  g_object_unref(fake);
}

static void test_auto_load(void) {
  FakeProvider *fake;
  GnOgPreviewCard *card = new_card(&fake);
  gn_og_preview_card_set_layout(card, GN_OG_PREVIEW_LAYOUT_COMPACT);
  gn_og_preview_card_set_auto_load(card, TRUE);
  gn_og_preview_card_set_auto_load_image(card, TRUE);
  gn_og_preview_card_set_media_badge(card, "media-playback-start-symbolic");
  gn_og_preview_card_set_url(card, "https://example.org/x");
  g_assert_cmpuint(fake->metadata_calls, ==, 1);
  complete_metadata(fake, "Title", "https://example.org/i.png");
  g_assert_cmpuint(fake->image_calls, ==, 1);
  g_assert_nonnull(find(GTK_WIDGET(card), "og_card"));
  g_assert_nonnull(find(GTK_WIDGET(card), "og_badge"));
  gn_og_preview_card_set_layout(card, GN_OG_PREVIEW_LAYOUT_STACKED);
  g_assert_null(find(GTK_WIDGET(card), "og_card"));
  g_assert_true(gtk_widget_get_parent(find(GTK_WIDGET(card), "og_title")) == GTK_WIDGET(card));
  g_object_unref(card);
  g_object_unref(fake);
}

static void test_stale_result_ignored(void) {
  FakeProvider *fake;
  GnOgPreviewCard *card = new_card(&fake);
  gn_og_preview_card_set_url(card, "https://example.org/old");
  gn_og_preview_card_request_load(card);
  GTask *old = g_steal_pointer(&fake->pending_metadata);
  gn_og_preview_card_set_url(card, "https://example.org/new");
  g_task_return_pointer(old, gn_og_metadata_new("https://example.org/old", "Old", NULL, NULL, NULL),
                        (GDestroyNotify)gn_og_metadata_unref);
  g_object_unref(old);
  drain();
  g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(card), "og_title")));
  g_assert_cmpstr(gn_og_preview_card_get_url(card), ==, "https://example.org/new");
  g_object_unref(card);
  g_object_unref(fake);
}

static void test_parent_cancel_and_unbind(void) {
  FakeProvider *fake;
  GnOgPreviewCard *card = new_card(&fake);
  g_autoptr(GCancellable) parent = g_cancellable_new();
  gn_og_preview_card_set_url_with_cancellable(card, "https://example.org/x", parent);
  gn_og_preview_card_request_load(card);
  g_assert_false(g_cancellable_is_cancelled(fake->last_cancellable));
  g_cancellable_cancel(parent);
  g_assert_true(g_cancellable_is_cancelled(fake->last_cancellable));
  /* A new parent on the same URL keeps the card. */
  g_autoptr(GCancellable) parent2 = g_cancellable_new();
  gn_og_preview_card_set_url_with_cancellable(card, "https://example.org/x", parent2);
  g_assert_cmpstr(gn_og_preview_card_get_url(card), ==, "https://example.org/x");
  gn_og_preview_card_request_load(card);
  GCancellable *inflight = g_object_ref(fake->last_cancellable);
  gn_og_preview_card_prepare_for_unbind(card);
  g_assert_true(g_cancellable_is_cancelled(inflight));
  g_assert_null(gn_og_preview_card_get_url(card));
  g_object_unref(inflight);
  /* Results arriving after unbind are dropped. */
  GTask *late = g_steal_pointer(&fake->pending_metadata);
  g_task_return_pointer(late, gn_og_metadata_new("https://example.org/x", "Late", NULL, NULL, NULL),
                        (GDestroyNotify)gn_og_metadata_unref);
  g_object_unref(late);
  drain();
  g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(card), "og_title")));
  g_cancellable_cancel(parent2); /* disconnected: no effect, no crash */
  g_object_unref(card);
  g_object_unref(fake);
}

static guint activations;
static void on_activate(GnOgPreviewCard *card, const char *url, gpointer data) {
  (void)card; (void)data;
  g_assert_cmpstr(url, ==, "https://example.org/x");
  activations++;
}
static void test_error_and_activate_signal(void) {
  FakeProvider *fake;
  GnOgPreviewCard *card = new_card(&fake);
  g_signal_connect(card, "activate", G_CALLBACK(on_activate), NULL);
  gn_og_preview_card_set_url(card, "https://example.org/x");
  gn_og_preview_card_request_load(card);
  GTask *t = g_steal_pointer(&fake->pending_metadata);
  g_task_return_new_error(t, G_IO_ERROR, G_IO_ERROR_FAILED, "boom");
  g_object_unref(t);
  drain();
  g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(card), "og_status")));
  g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(card), "og_load")));
  g_signal_emit_by_name(card, "activate", "https://example.org/x");
  g_assert_cmpuint(activations, ==, 1);
  g_object_unref(card);
  g_object_unref(fake);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  if (!gtk_init_check()) {
    g_print("1..0 # SKIP no display\n");
    return 77;
  }
  g_test_add_func("/og-preview-card/no-io-by-default", test_no_io_by_default);
  g_test_add_func("/og-preview-card/auto-load", test_auto_load);
  g_test_add_func("/og-preview-card/stale-result", test_stale_result_ignored);
  g_test_add_func("/og-preview-card/parent-cancel-unbind", test_parent_cancel_and_unbind);
  g_test_add_func("/og-preview-card/error-activate", test_error_and_activate_signal);
  return g_test_run();
}
