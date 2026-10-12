/*
 * test_timeline_repost — a kind-6 repost whose original is not stored
 * locally (nostrc-uoe2q, nostrc-8xfib.10).
 *
 * Real NostrGtkNoteCardRow + GnNostrReferenceCard, through the helper the
 * timeline factory's legacy and snapshot bind paths call:
 *   - the repost's raw content (the original's JSON) is never the body;
 *   - the card goes Finding on relays… -> the original, or -> unavailable,
 *     through the resolver's fetch, never the legacy request-embed;
 *   - snapshot rows (no bind-time fetch) get the same card, inert;
 *   - recycling the row cancels an in-flight fetch.
 * With GNOSTR_TEST_SCREENSHOTS=<dir> the states are saved as PNGs, styled
 * with gnostr.css.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nostrc-test-gdk-frame.h"
#include "ui/gnostr-timeline-repost.h"

#include <gtk/gtk.h>
#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include <nostr-event.h>
#include <string.h>

extern GResource *nostr_gtk_get_resource(void);

#define SK "7f7ff03d123792d6ac594bfa67bf6d0c0ab55b6b1fdb6249303fe861f1ccba9a"

static char *
signed_note(const char *content)
{
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 1);
  nostr_event_set_created_at(ev, 1700000000);
  nostr_event_set_content(ev, content);
  nostr_event_set_tags(ev, nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(ev, SK), ==, 0);
  char *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  return json;
}

/* The descriptor the factory parses from a kind-6 event's tags. */
static GnNostrRepostDescriptor *
repost_of(const char *original_json)
{
  g_autoptr(GnNostrEventInfo) info = gn_nostr_event_parse(original_json, NULL);
  g_assert_nonnull(info);
  g_autofree gchar *tags = g_strdup_printf("[[\"e\",\"%s\",\"wss://relay.example\"],[\"p\",\"%s\"]]",
                                           info->id, info->pubkey);
  GnNostrRepostDescriptor *d = gn_nostr_repost_descriptor_parse_tags(6, tags);
  g_assert_nonnull(d);
  g_assert_false(d->quote);
  return d;
}

/* ---- resolver whose relay fetch the test completes ---- */
#define FAKE_TYPE_RESOLVER (fake_resolver_get_type())
G_DECLARE_FINAL_TYPE(FakeResolver, fake_resolver, FAKE, RESOLVER, GObject)
struct _FakeResolver {
  GObject parent_instance;
  guint lookups, fetches;
  GTask *pending;
};
static void fake_iface_init(GnNostrReferenceResolverInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(FakeResolver, fake_resolver, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_NOSTR_REFERENCE_RESOLVER, fake_iface_init))

static gchar *fake_lookup(GnNostrReferenceResolver *r, const GnNostrReference *ref) {
  (void)ref;
  FAKE_RESOLVER(r)->lookups++;
  return NULL; /* not stored locally */
}
static gchar *fake_name(GnNostrReferenceResolver *r, const gchar *pubkey) {
  (void)r; (void)pubkey;
  return g_strdup("Alice");
}
static gboolean fake_can_fetch(GnNostrReferenceResolver *r) { (void)r; return TRUE; }
static void fake_fetch_async(GnNostrReferenceResolver *r, const GnNostrReference *ref,
                             GCancellable *c, GAsyncReadyCallback cb, gpointer data) {
  (void)ref;
  FakeResolver *self = FAKE_RESOLVER(r);
  self->fetches++;
  g_clear_object(&self->pending);
  self->pending = g_task_new(self, c, cb, data);
}
static gchar *fake_fetch_finish(GnNostrReferenceResolver *r, GAsyncResult *res, GError **error) {
  (void)r;
  return g_task_propagate_pointer(G_TASK(res), error);
}
static void fake_iface_init(GnNostrReferenceResolverInterface *iface) {
  iface->lookup_local = fake_lookup;
  iface->display_name = fake_name;
  iface->can_fetch = fake_can_fetch;
  iface->fetch_async = fake_fetch_async;
  iface->fetch_finish = fake_fetch_finish;
}
static void fake_finalize(GObject *o) {
  g_clear_object(&FAKE_RESOLVER(o)->pending);
  G_OBJECT_CLASS(fake_resolver_parent_class)->finalize(o);
}
static void fake_resolver_class_init(FakeResolverClass *k) { G_OBJECT_CLASS(k)->finalize = fake_finalize; }
static void fake_resolver_init(FakeResolver *self) { (void)self; }

/* ---- helpers ---- */
static void
settle(void)
{
  while (g_main_context_iteration(NULL, FALSE));
}

static GtkWidget *
find_named(GtkWidget *w, const char *name)
{
  if (g_strcmp0(gtk_widget_get_name(w), name) == 0) return w;
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *hit = find_named(c, name);
    if (hit) return hit;
  }
  return NULL;
}

static const char *
card_text(GnNostrReferenceCard *card, const char *name)
{
  GtkWidget *label = find_named(GTK_WIDGET(card), name);
  g_assert_nonnull(label);
  return gtk_widget_get_visible(label) ? gtk_label_get_text(GTK_LABEL(label)) : NULL;
}

static void
count_embed(NostrGtkNoteCardRow *row, const char *target, guint *count)
{
  (void)row; (void)target;
  (*count)++;
}

typedef struct {
  GtkWidget *window;
  NostrGtkNoteCardRow *row;
  FakeResolver *resolver;
  guint embeds;
} Fixture;

static void
fixture_set_up(Fixture *f, gconstpointer data)
{
  (void)data;
  f->window = gtk_window_new();
  gtk_window_set_default_size(GTK_WINDOW(f->window), 480, -1);
  f->row = nostr_gtk_note_card_row_new();
  gtk_window_set_child(GTK_WINDOW(f->window), GTK_WIDGET(f->row));
  f->resolver = g_object_new(FAKE_TYPE_RESOLVER, NULL);
  g_signal_connect(f->row, "request-embed", G_CALLBACK(count_embed), &f->embeds);
  gtk_window_present(GTK_WINDOW(f->window));
  while (!gtk_widget_get_mapped(GTK_WIDGET(f->row)))
    g_main_context_iteration(NULL, TRUE);
}

static void
fixture_tear_down(Fixture *f, gconstpointer data)
{
  (void)data;
  gtk_window_destroy(GTK_WINDOW(f->window));
  settle();
  g_clear_object(&f->resolver);
}

/* What the factory does for a kind-6 row before the repost block: bind
 * the reposter, then the (raw) content is replaced by the repost path. */
static void
bind_repost(Fixture *f, const char *raw_content)
{
  nostr_gtk_note_card_row_prepare_for_bind(f->row);
  nostr_gtk_note_card_row_set_author_name_only(f->row, "Bob", "bob");
  nostr_gtk_note_card_row_set_content(f->row, raw_content);
  nostr_gtk_note_card_row_set_is_repost(f->row, TRUE);
  nostr_gtk_note_card_row_set_repost_info(f->row, NULL, "Bob", 1700000100);
}

static void
set_true(gboolean *flag)
{
  *flag = TRUE;
}

static void
save_png(Fixture *f, const char *name)
{
  const char *dir = g_getenv("GNOSTR_TEST_SCREENSHOTS");
  if (!dir) return;
  /* Resize to the new natural height and wait for that frame's paint. */
  gtk_window_set_default_size(GTK_WINDOW(f->window), 480, 1);
  gtk_widget_queue_resize(f->window);
  GdkFrameClock *clock = gtk_widget_get_frame_clock(f->window);
  gboolean painted = FALSE;
  gulong id = g_signal_connect_swapped(clock, "after-paint", G_CALLBACK(set_true), &painted);
  gdk_frame_clock_request_phase(clock, GDK_FRAME_CLOCK_PHASE_LAYOUT | GDK_FRAME_CLOCK_PHASE_PAINT);
  while (!painted)
    g_main_context_iteration(NULL, TRUE);
  g_signal_handler_disconnect(clock, id);
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(f->window);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snapshot, gdk_paintable_get_intrinsic_width(paintable),
                         gdk_paintable_get_intrinsic_height(paintable));
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  graphene_rect_t bounds;
  gsk_render_node_get_bounds(node, &bounds);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(f->window));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &bounds);
  g_autofree gchar *path = g_strdup_printf("%s/gnostr-repost-pending-%s.png", dir, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s", path);
}

/* ---- tests ---- */

static void
test_fetch_resolves(Fixture *f, gconstpointer data)
{
  (void)data;
  g_autofree char *original = signed_note("the original note");
  g_autoptr(GnNostrRepostDescriptor) d = repost_of(original);
  bind_repost(f, original); /* kind-6 content: the original's JSON */
  g_assert_null(d->original_json); /* tags only: nothing embedded */

  gnostr_timeline_repost_show_pending(f->row, d, GN_NOSTR_REFERENCE_RESOLVER(f->resolver), TRUE);
  GnNostrReferenceCard *card = nostr_gtk_note_card_row_get_reference_card(f->row);
  g_assert_nonnull(card);
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(card)));
  g_assert_cmpstr(nostr_gtk_note_card_row_get_content_text(f->row), ==, "");
  g_assert_cmpuint(f->resolver->lookups, ==, 1);
  g_assert_cmpuint(f->resolver->fetches, ==, 1);
  g_assert_cmpuint(f->embeds, ==, 0);
  g_assert_cmpint(gn_nostr_reference_card_get_state(card), ==, GN_NOSTR_REFERENCE_CARD_FETCHING);
  g_assert_cmpstr(card_text(card, "reference-status"), ==, "Finding on relays…");
  g_assert_cmpstr(card_text(card, "reference-title"), ==, "Reposted Nostr note");
  g_assert_true(g_str_has_prefix(card_text(card, "reference-address"), "nevent1"));
  save_png(f, "fetching");

  g_task_return_pointer(f->resolver->pending, g_strdup(original), g_free);
  settle();
  g_assert_cmpint(gn_nostr_reference_card_get_state(card), ==, GN_NOSTR_REFERENCE_CARD_RESOLVED);
  g_assert_cmpstr(card_text(card, "reference-title"), ==, "Reposted from Alice");
  g_assert_cmpstr(card_text(card, "reference-content"), ==, "the original note");
  g_assert_null(card_text(card, "reference-status"));
  g_assert_null(card_text(card, "reference-address"));
  g_assert_cmpstr(nostr_gtk_note_card_row_get_content_text(f->row), ==, "");
  g_assert_cmpuint(f->embeds, ==, 0);
  save_png(f, "resolved");
}

static void
test_fetch_unavailable(Fixture *f, gconstpointer data)
{
  (void)data;
  g_autofree char *original = signed_note("never arrives");
  g_autoptr(GnNostrRepostDescriptor) d = repost_of(original);
  bind_repost(f, original);
  gnostr_timeline_repost_show_pending(f->row, d, GN_NOSTR_REFERENCE_RESOLVER(f->resolver), TRUE);
  GnNostrReferenceCard *card = nostr_gtk_note_card_row_get_reference_card(f->row);

  g_task_return_new_error(f->resolver->pending, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "none");
  settle();
  g_assert_cmpint(gn_nostr_reference_card_get_state(card), ==, GN_NOSTR_REFERENCE_CARD_UNAVAILABLE);
  g_assert_cmpstr(card_text(card, "reference-status"), ==, "No matching signed note was found.");
  g_assert_null(card_text(card, "reference-content"));
  g_assert_true(gtk_widget_get_visible(find_named(GTK_WIDGET(card), "reference-find")));
  g_assert_cmpstr(nostr_gtk_note_card_row_get_content_text(f->row), ==, "");
  g_assert_null(strstr(gtk_label_get_text(GTK_LABEL(find_named(GTK_WIDGET(card), "reference-title"))),
                       "never arrives"));
  g_assert_cmpuint(f->embeds, ==, 0);
  save_png(f, "unavailable");
}

/* Snapshot rows do no bind-time relay work: same card, inert, offering
 * Find on Relays; the explicit find uses the same resolver fetch. */
static void
test_snapshot_inert(Fixture *f, gconstpointer data)
{
  (void)data;
  g_autofree char *original = signed_note("snapshot original");
  g_autoptr(GnNostrRepostDescriptor) d = repost_of(original);
  bind_repost(f, original);
  gnostr_timeline_repost_show_pending(f->row, d, GN_NOSTR_REFERENCE_RESOLVER(f->resolver), FALSE);
  GnNostrReferenceCard *card = nostr_gtk_note_card_row_get_reference_card(f->row);
  g_assert_cmpint(gn_nostr_reference_card_get_state(card), ==, GN_NOSTR_REFERENCE_CARD_INERT);
  g_assert_cmpuint(f->resolver->fetches, ==, 0);
  g_assert_cmpstr(nostr_gtk_note_card_row_get_content_text(f->row), ==, "");
  GtkWidget *find = find_named(GTK_WIDGET(card), "reference-find");
  g_assert_true(gtk_widget_get_visible(find));
  save_png(f, "snapshot-inert");

  g_signal_emit_by_name(find, "clicked");
  g_assert_cmpuint(f->resolver->fetches, ==, 1);
  g_assert_cmpint(gn_nostr_reference_card_get_state(card), ==, GN_NOSTR_REFERENCE_CARD_FETCHING);
  g_assert_cmpuint(f->embeds, ==, 0);
}

/* A recycled row drops the fetch: its late answer changes nothing. */
static void
test_recycle_cancels(Fixture *f, gconstpointer data)
{
  (void)data;
  g_autofree char *original = signed_note("late");
  g_autoptr(GnNostrRepostDescriptor) d = repost_of(original);
  bind_repost(f, original);
  gnostr_timeline_repost_show_pending(f->row, d, GN_NOSTR_REFERENCE_RESOLVER(f->resolver), TRUE);
  GnNostrReferenceCard *card = nostr_gtk_note_card_row_get_reference_card(f->row);
  GTask *stale = g_object_ref(f->resolver->pending);
  GCancellable *cancellable = g_task_get_cancellable(stale);

  /* The factory's unbind, then the row's next (non-repost) note. */
  nostr_gtk_note_card_row_prepare_for_unbind(f->row);
  bind_repost(f, "plain note");
  g_assert_true(g_cancellable_is_cancelled(cancellable));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(card)));
  g_task_return_pointer(stale, g_strdup(original), g_free);
  g_object_unref(stale);
  settle();
  g_assert_cmpint(gn_nostr_reference_card_get_state(card), ==, GN_NOSTR_REFERENCE_CARD_EMPTY);
  g_assert_cmpstr(nostr_gtk_note_card_row_get_content_text(f->row), ==, "plain note");
}

/* gnostr.css carries rules this GTK may not know (e.g. "overflow" before
 * GTK 4.x supports it); the app logs them, the test must not abort on them. */
static void
on_css_error(GtkCssProvider *provider, GtkCssSection *section, const GError *error, gpointer data)
{
  (void)section; (void)data;
  g_test_message("gnostr.css: %s", error->message);
  g_signal_stop_emission_by_name(provider, "parsing-error");
}

int
main(int argc, char **argv)
{
  gtk_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  g_resources_register(nostr_gtk_get_resource());
  g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
  g_signal_connect(css, "parsing-error", G_CALLBACK(on_css_error), NULL);
  gtk_css_provider_load_from_resource(css, "/org/gnostr/ui/ui/styles/gnostr.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
#define ADD(path, fn) g_test_add("/gnostr/timeline-repost/" path, Fixture, NULL, \
                                 fixture_set_up, fn, fixture_tear_down)
  ADD("fetch-resolves", test_fetch_resolves);
  ADD("fetch-unavailable", test_fetch_unavailable);
  ADD("snapshot-inert", test_snapshot_inert);
  ADD("recycle-cancels", test_recycle_cancels);
  return g_test_run();
}
